#include "bootstrap.h"

#include "process.h"
#include "progress.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

namespace lathe {

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
fs::path path_from_utf8(const std::string& utf8) {
  return fs::path(utf8_to_utf16(utf8));
}
std::string path_to_utf8(const fs::path& p) {
  return utf16_to_utf8(p.wstring());
}
#else
fs::path path_from_utf8(const std::string& utf8) { return fs::path(utf8); }
std::string path_to_utf8(const fs::path& p) { return p.string(); }
#endif

void emit_bootstrap(const std::string& stage,
                    const std::string& binary,
                    const std::string& message = std::string()) {
  // We don't pull in a JSON lib for this one event type — escaping is
  // trivial since stage / binary are constant strings and message is
  // already plaintext from stderr or our own tooling.
  std::string m;
  for (char c : message) {
    if      (c == '"')  m += "\\\"";
    else if (c == '\\') m += "\\\\";
    else if (c == '\n' || c == '\r' || c == '\t') m += ' ';
    else                m += c;
  }
  std::printf(
    "{\"type\":\"bootstrap\",\"stage\":\"%s\",\"binary\":\"%s\"%s%s%s}\n",
    stage.c_str(),
    binary.c_str(),
    message.empty() ? "" : ",\"message\":\"",
    message.empty() ? "" : m.c_str(),
    message.empty() ? "" : "\"");
  std::fflush(stdout);
}

bool powershell_run(const std::vector<std::string>& ps_argv) {
  // Re-use our standard Win32 spawn (UTF-8 / Job Object / Ctrl-C aware).
  std::string captured;
  int rc = run_subprocess(ps_argv, [&](const std::string& line) {
    if (!line.empty()) {
      if (!captured.empty()) captured += "\n";
      captured += line;
    }
  });
  if (rc != 0 && !captured.empty()) {
    emit_bootstrap("info", "powershell", captured);
  }
  return rc == 0;
}

bool download_url_to(const std::string& url, const fs::path& dest) {
  // Quote the URL/path inside the PS one-liner. Single-quoted strings
  // in PowerShell are literal, so we escape any embedded ' as ''.
  auto ps_quote = [](const std::string& s) {
    std::string out = "'";
    for (char c : s) { if (c == '\'') out += "''"; else out += c; }
    out += "'";
    return out;
  };

  // -UseBasicParsing keeps Invoke-WebRequest from spinning up IE COM
  // for HTML scraping; for raw downloads it's faster and works on
  // headless / Server Core. SilentlyContinue suppresses the verbose
  // PS progress UI which slows large downloads.
  std::string ps =
    "$ProgressPreference='SilentlyContinue';"
    " [Net.ServicePointManager]::SecurityProtocol = "
    "[Net.SecurityProtocolType]::Tls12;"
    " try {"
    "  Invoke-WebRequest -Uri " + ps_quote(url) +
    " -OutFile " + ps_quote(path_to_utf8(dest)) +
    " -UseBasicParsing;"
    "  exit 0"
    " } catch {"
    "  Write-Output $_.Exception.Message;"
    "  exit 1"
    " }";

  std::vector<std::string> argv = {
    "powershell", "-NoProfile", "-NonInteractive", "-Command", ps,
  };
  return powershell_run(argv) && fs::exists(dest);
}

bool extract_zip(const fs::path& zip, const fs::path& dest) {
  auto ps_quote = [](const std::string& s) {
    std::string out = "'";
    for (char c : s) { if (c == '\'') out += "''"; else out += c; }
    out += "'";
    return out;
  };
  std::string ps =
    "Expand-Archive -Path " + ps_quote(path_to_utf8(zip)) +
    " -DestinationPath " + ps_quote(path_to_utf8(dest)) +
    " -Force";
  std::vector<std::string> argv = {
    "powershell", "-NoProfile", "-NonInteractive", "-Command", ps,
  };
  return powershell_run(argv);
}

// Walk the extracted tree for a target filename. Returns first match
// (BtbN's GPL archive lays out as <root>/bin/ffmpeg.exe).
fs::path find_recursive(const fs::path& root, const std::string& filename) {
  std::error_code ec;
  if (!fs::exists(root, ec)) return fs::path();
  for (auto& p : fs::recursive_directory_iterator(root, ec)) {
    if (ec) break;
    if (p.is_regular_file(ec) && p.path().filename() == filename) {
      return p.path();
    }
  }
  return fs::path();
}

}

bool ffmpeg_present() {
  fs::path target = path_from_utf8(exe_dir()) / "ffmpeg.exe";
  std::error_code ec;
  return fs::exists(target, ec);
}

bool ensure_ffmpeg() {
  if (ffmpeg_present()) return true;

  emit_bootstrap("download", "ffmpeg");

  fs::path bin_dir   = path_from_utf8(exe_dir());
  fs::path zip_path  = bin_dir / "_ffmpeg_download.zip";
  fs::path extract_d = bin_dir / "_ffmpeg_extract";
  fs::path target    = bin_dir / "ffmpeg.exe";

  std::error_code ec;
  fs::remove(zip_path, ec);
  fs::remove_all(extract_d, ec);

  // BtbN's "latest" tag rolls forward continuously and ships a static
  // GPL-licensed Windows x64 build. The archive layout puts the binary
  // at <root>/bin/ffmpeg.exe.
  const std::string url =
    "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/"
    "ffmpeg-master-latest-win64-gpl.zip";

  if (!download_url_to(url, zip_path)) {
    emit_bootstrap("failed", "ffmpeg", "download failed");
    fs::remove(zip_path, ec);
    return false;
  }

  emit_bootstrap("extracting", "ffmpeg");
  if (!extract_zip(zip_path, extract_d)) {
    emit_bootstrap("failed", "ffmpeg", "archive extraction failed");
    fs::remove(zip_path, ec);
    fs::remove_all(extract_d, ec);
    return false;
  }

  fs::path found = find_recursive(extract_d, "ffmpeg.exe");
  if (found.empty()) {
    emit_bootstrap("failed", "ffmpeg", "ffmpeg.exe not found in extracted archive");
    fs::remove(zip_path, ec);
    fs::remove_all(extract_d, ec);
    return false;
  }

  fs::copy_file(found, target, fs::copy_options::overwrite_existing, ec);
  if (ec) {
    emit_bootstrap("failed", "ffmpeg", "could not copy ffmpeg.exe into binaries dir: " + ec.message());
    fs::remove(zip_path, ec);
    fs::remove_all(extract_d, ec);
    return false;
  }

  fs::remove(zip_path, ec);
  fs::remove_all(extract_d, ec);

  emit_bootstrap("done", "ffmpeg");
  return true;
}

bool ensure_required() {
  return ensure_ffmpeg();
}

}
