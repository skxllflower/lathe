#include "process.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <signal.h>
  #include <sys/wait.h>
  #include <unistd.h>
  #if defined(__APPLE__)
    #include <cstdint>
    #include <filesystem>
    #include <mach-o/dyld.h>
  #endif
#endif

namespace lathe {

namespace {

std::atomic<bool> g_cancelled{false};
std::string       g_last_error;

#ifdef _WIN32

HANDLE g_job = nullptr;

BOOL WINAPI ctrl_handler(DWORD dw_type) {
  switch (dw_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
      g_cancelled.store(true);
      if (g_job) TerminateJobObject(g_job, 1);
      return TRUE;
    default:
      return FALSE;
  }
}

// Quote an argument for CreateProcessW's command-line buffer per the
// rules msvcrt's argv parser uses (the "Parsing C++ Command-Line
// Arguments" reference). Backslashes only need escaping if they precede
// a quote; trailing-before-close-quote runs need doubling.
std::wstring quote_arg_w(const std::wstring& arg) {
  bool needs = arg.empty() ||
               arg.find_first_of(L" \t\n\v\"") != std::wstring::npos;
  if (!needs) return arg;
  std::wstring out;
  out.push_back(L'"');
  for (size_t i = 0; i < arg.size(); ++i) {
    size_t bs = 0;
    while (i < arg.size() && arg[i] == L'\\') { ++bs; ++i; }
    if (i == arg.size()) {
      out.append(bs * 2, L'\\');
      break;
    }
    if (arg[i] == L'"') {
      out.append(bs * 2 + 1, L'\\');
      out.push_back(L'"');
    } else {
      out.append(bs, L'\\');
      out.push_back(arg[i]);
    }
  }
  out.push_back(L'"');
  return out;
}

// stderr-drain thread for run_subprocess_streaming. A plain Win32 thread (not
// std::thread): MSVC's <thread> pulls in the CRT <process.h>, which collides
// with lathe's own process.h on the include path. Reads the child's stderr
// pipe and hands each line to the callback.
struct ErrDrainCtx {
  HANDLE pipe;
  const std::function<void(const std::string&)>* on_line;
};

DWORD WINAPI err_drain_proc(LPVOID param) {
  auto* ctx = static_cast<ErrDrainCtx*>(param);
  std::string line;
  char buf[2048];
  DWORD n = 0;
  while (ReadFile(ctx->pipe, buf, sizeof(buf), &n, nullptr) && n > 0) {
    for (DWORD i = 0; i < n; ++i) {
      char c = buf[i];
      if (c == '\n' || c == '\r') {
        if (!line.empty()) {
          if (*ctx->on_line) (*ctx->on_line)(line);
          line.clear();
        }
      } else {
        line.push_back(c);
      }
    }
  }
  if (!line.empty() && *ctx->on_line) (*ctx->on_line)(line);
  return 0;
}

#endif

}

#ifdef _WIN32

std::wstring utf8_to_utf16(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                              nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
  return out;
}

std::string utf16_to_utf8(const std::wstring& s) {
  if (s.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(),
                              nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(),
                      &out[0], n, nullptr, nullptr);
  return out;
}

int run_subprocess(const std::vector<std::string>& argv,
                   const std::function<void(const std::string&)>& on_line) {
  g_cancelled.store(false);
  g_last_error.clear();

  if (argv.empty()) { g_last_error = "empty argv"; return -1; }

  std::wstring cmd_line;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (i) cmd_line.push_back(L' ');
    cmd_line += quote_arg_w(utf8_to_utf16(argv[i]));
  }

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) {
    g_last_error = "CreatePipe failed";
    return -1;
  }
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (!job) {
    CloseHandle(rd); CloseHandle(wr);
    g_last_error = "CreateJobObjectW failed";
    return -1;
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
  jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                          &jeli, sizeof(jeli));

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = wr;
  si.hStdError  = wr;
  si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION pi{};
  std::wstring cmd_buf = cmd_line;  // CreateProcessW writes into the buffer

  BOOL ok = CreateProcessW(
    nullptr,
    &cmd_buf[0],
    nullptr, nullptr,
    TRUE,
    CREATE_SUSPENDED | CREATE_NO_WINDOW,
    nullptr, nullptr,
    &si, &pi);

  if (!ok) {
    DWORD err = GetLastError();
    char msg[128];
    std::snprintf(msg, sizeof(msg),
                  "CreateProcessW failed (GetLastError=%lu)", err);
    g_last_error = msg;
    CloseHandle(rd); CloseHandle(wr); CloseHandle(job);
    return -1;
  }

  AssignProcessToJobObject(job, pi.hProcess);
  ResumeThread(pi.hThread);

  CloseHandle(wr);  // parent end of stdout pipe is read-only

  g_job = job;
  SetConsoleCtrlHandler(ctrl_handler, TRUE);

  std::string line;
  char buf[4096];
  DWORD n = 0;
  while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) {
    for (DWORD i = 0; i < n; ++i) {
      char c = buf[i];
      if (c == '\n' || c == '\r') {
        if (!line.empty()) {
          if (on_line) on_line(line);
          line.clear();
        }
      } else {
        line.push_back(c);
      }
    }
  }
  if (!line.empty() && on_line) on_line(line);

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exit_code = 0;
  GetExitCodeProcess(pi.hProcess, &exit_code);

  SetConsoleCtrlHandler(ctrl_handler, FALSE);
  g_job = nullptr;

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CloseHandle(rd);
  CloseHandle(job);

  return static_cast<int>(exit_code);
}

int run_subprocess_streaming(
    const std::vector<std::string>& argv,
    const std::function<bool(const char*, std::size_t)>& on_stdout,
    const std::function<void(const std::string&)>& on_stderr_line) {
  g_cancelled.store(false);
  g_last_error.clear();
  if (argv.empty()) { g_last_error = "empty argv"; return -1; }

  std::wstring cmd_line;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (i) cmd_line.push_back(L' ');
    cmd_line += quote_arg_w(utf8_to_utf16(argv[i]));
  }

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  // Separate pipes for stdout (binary frames) and stderr (text lines) so the
  // RGBA stream is never corrupted by interleaved log text.
  HANDLE out_rd = nullptr, out_wr = nullptr;
  HANDLE err_rd = nullptr, err_wr = nullptr;
  if (!CreatePipe(&out_rd, &out_wr, &sa, 0)) {
    g_last_error = "CreatePipe(out) failed";
    return -1;
  }
  SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);
  if (!CreatePipe(&err_rd, &err_wr, &sa, 0)) {
    CloseHandle(out_rd); CloseHandle(out_wr);
    g_last_error = "CreatePipe(err) failed";
    return -1;
  }
  SetHandleInformation(err_rd, HANDLE_FLAG_INHERIT, 0);

  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (!job) {
    CloseHandle(out_rd); CloseHandle(out_wr);
    CloseHandle(err_rd); CloseHandle(err_wr);
    g_last_error = "CreateJobObjectW failed";
    return -1;
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
  jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                          &jeli, sizeof(jeli));

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = out_wr;
  si.hStdError  = err_wr;
  si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION pi{};
  std::wstring cmd_buf = cmd_line;

  BOOL ok = CreateProcessW(
    nullptr, &cmd_buf[0], nullptr, nullptr, TRUE,
    CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

  if (!ok) {
    DWORD err = GetLastError();
    char msg[128];
    std::snprintf(msg, sizeof(msg),
                  "CreateProcessW failed (GetLastError=%lu)", err);
    g_last_error = msg;
    CloseHandle(out_rd); CloseHandle(out_wr);
    CloseHandle(err_rd); CloseHandle(err_wr); CloseHandle(job);
    return -1;
  }

  AssignProcessToJobObject(job, pi.hProcess);
  ResumeThread(pi.hThread);

  CloseHandle(out_wr);  // parent only reads
  CloseHandle(err_wr);

  g_job = job;
  SetConsoleCtrlHandler(ctrl_handler, TRUE);

  // Drain stderr on a worker thread so the main thread can block on the
  // high-bandwidth stdout pipe without deadlocking when the child fills its
  // stderr buffer. ctx lives on this stack until we join the thread below.
  ErrDrainCtx err_ctx{ err_rd, &on_stderr_line };
  HANDLE err_thread = CreateThread(nullptr, 0, err_drain_proc, &err_ctx, 0, nullptr);

  // Forward stdout bytes raw. A false return from on_stdout (consumer closed
  // the downstream pipe) terminates the child so it stops decoding.
  char buf[65536];
  DWORD n = 0;
  while (ReadFile(out_rd, buf, sizeof(buf), &n, nullptr) && n > 0) {
    if (on_stdout && !on_stdout(buf, static_cast<std::size_t>(n))) {
      TerminateJobObject(job, 1);
      break;
    }
  }

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exit_code = 0;
  GetExitCodeProcess(pi.hProcess, &exit_code);

  if (err_thread) {
    WaitForSingleObject(err_thread, INFINITE);
    CloseHandle(err_thread);
  }

  SetConsoleCtrlHandler(ctrl_handler, FALSE);
  g_job = nullptr;

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CloseHandle(out_rd);
  CloseHandle(err_rd);
  CloseHandle(job);

  return static_cast<int>(exit_code);
}

std::string exe_dir() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n == 0) return ".";
  std::wstring s(buf, n);
  auto slash = s.find_last_of(L"\\/");
  std::wstring dir = (slash == std::wstring::npos) ? L"." : s.substr(0, slash);
  return utf16_to_utf8(dir);
}

#else  // POSIX

int run_subprocess(const std::vector<std::string>& argv,
                   const std::function<void(const std::string&)>& on_line) {
  g_cancelled.store(false);
  g_last_error.clear();
  if (argv.empty()) { g_last_error = "empty argv"; return -1; }

  // POSIX stub via popen — non-ASCII handling depends on locale. WAVdesk
  // is Windows-first; this path exists for completeness.
  std::string cmd;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (i) cmd += " ";
    cmd += "\"";
    for (char c : argv[i]) {
      if (c == '"' || c == '\\' || c == '$' || c == '`') cmd += '\\';
      cmd += c;
    }
    cmd += "\"";
  }
  cmd += " 2>&1";

  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) { g_last_error = "popen failed"; return -1; }

  char buf[4096];
  while (std::fgets(buf, sizeof(buf), pipe)) {
    std::string line(buf);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
      line.pop_back();
    if (!line.empty() && on_line) on_line(line);
  }
  int rc = pclose(pipe);
  if (WIFEXITED(rc)) rc = WEXITSTATUS(rc);
  return rc;
}

int run_subprocess_streaming(
    const std::vector<std::string>&,
    const std::function<bool(const char*, std::size_t)>&,
    const std::function<void(const std::string&)>&) {
  // WAVdesk's video engine is Windows-first; the separate-pipe binary stream
  // isn't wired up on POSIX yet. Fail loudly rather than silently no-op.
  g_last_error = "run_subprocess_streaming not implemented on POSIX";
  return -1;
}

std::string exe_dir() {
#if defined(__APPLE__)
  // macOS has no /proc; ask dyld for our own image path. First call with a null
  // buffer reports the needed size, then we fill it. The result can contain
  // symlinks or ".." components, so canonicalize before taking the parent.
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buf(size, '\0');
  if (_NSGetExecutablePath(&buf[0], &size) != 0) return ".";
  if (!buf.empty() && buf.back() == '\0') buf.pop_back();
  std::error_code ec;
  std::filesystem::path p = std::filesystem::canonical(buf, ec);
  if (ec) p = std::filesystem::path(buf);
  std::filesystem::path dir = p.parent_path();
  return dir.empty() ? std::string(".") : dir.string();
#else
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return ".";
  std::string s(buf, n);
  auto slash = s.find_last_of('/');
  return slash == std::string::npos ? std::string(".") : s.substr(0, slash);
#endif
}

#endif

bool was_cancelled() { return g_cancelled.load(); }
std::string last_subprocess_error() { return g_last_error; }

}
