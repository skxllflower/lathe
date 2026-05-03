#include "convert.h"
#include "process.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <shellapi.h>
  #pragma comment(lib, "Shell32.lib")
#endif

namespace {

int print_help() {
  std::puts(
    "lathe " "0.1.0" " - any-to-any media converter (ffmpeg wrapper)\n"
    "\n"
    "Usage:\n"
    "  lathe convert <input> <output>\n"
    "  lathe --version\n"
    "  lathe --help\n"
    "\n"
    "Output container/codec is auto-detected from the output file extension.\n"
    "Paths are UTF-8 (Windows) — non-ASCII filenames are preserved.\n"
    "\n"
    "Progress is emitted as newline-delimited JSON on stdout, one event per line:\n"
    "  {\"type\":\"start\",    \"input\":..., \"output\":..., \"duration_s\":...}\n"
    "  {\"type\":\"progress\", \"percent\":..., \"time_s\":..., \"speed\":...}\n"
    "  {\"type\":\"done\",     \"output\":...}\n"
    "  {\"type\":\"cancelled\"}\n"
    "  {\"type\":\"error\",    \"message\":...}\n"
    "\n"
    "Cancellation: terminate the process (Ctrl+C, or TerminateProcess from a\n"
    "parent). The wrapped ffmpeg child dies with us via Windows Job Object.\n"
  );
  return 0;
}

int run_cli(const std::vector<std::string>& args) {
  if (args.size() < 2) return print_help();

  const std::string& cmd = args[1];

  if (cmd == "--help" || cmd == "-h") return print_help();
  if (cmd == "--version" || cmd == "-v") {
    std::puts("lathe 0.1.0");
    return 0;
  }

  if (cmd == "convert") {
    if (args.size() < 4) {
      std::fputs("error: convert requires <input> <output>\n", stderr);
      return 2;
    }
    auto r = lathe::convert(args[2], args[3]);
    switch (r) {
      case lathe::ConvertResult::Ok:            return 0;
      case lathe::ConvertResult::Cancelled:     return 130;
      case lathe::ConvertResult::InputNotFound: return 1;
      case lathe::ConvertResult::FfmpegFailed:  return 1;
    }
    return 1;
  }

  std::fprintf(stderr, "error: unknown command '%s'\n", cmd.c_str());
  return 2;
}

}

int main(int /*argc*/, char** /*argv*/) {
#ifdef _WIN32
  // Pull wide argv directly so non-ASCII paths survive — bypasses the
  // narrow main(argc, argv) which is in the system ANSI codepage.
  int wargc = 0;
  LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  if (!wargv) return 1;
  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(wargc));
  for (int i = 0; i < wargc; ++i) {
    args.push_back(lathe::utf16_to_utf8(wargv[i]));
  }
  LocalFree(wargv);

  // Make stdout unbuffered enough for line-by-line streaming.
  // (progress.cpp also calls fflush after each line.)
  SetConsoleOutputCP(CP_UTF8);
  return run_cli(args);
#else
  // POSIX: argv is already the user's encoding; on modern systems UTF-8.
  // (Stub — Lathe is Windows-first today.)
  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
  return run_cli(args);
#endif
}
