#include "paths.h"

#include "process.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

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

const char* ffmpeg_name() {
#ifdef _WIN32
  return "ffmpeg.exe";
#else
  return "ffmpeg";
#endif
}

// <platform data dir>/Vacant Systems — mirrors the layout WAVdesk's
// get_appdata_dir() established; keep the three branches in lockstep
// across the Vacant Systems repos.
fs::path vendor_root() {
#ifdef _WIN32
  const char* local = std::getenv("LOCALAPPDATA");
  if (local) return path_from_utf8(local) / "Vacant Systems";
  return fs::path("C:/Users/Public") / "Vacant Systems";  // fallback
#elif defined(__APPLE__)
  const char* home = std::getenv("HOME");
  if (home) return fs::path(home) / "Library" / "Application Support" / "Vacant Systems";
  return fs::path("/tmp") / "Vacant Systems";
#else
  const char* xdg = std::getenv("XDG_DATA_HOME");
  if (xdg) return fs::path(xdg) / "vacant-systems";
  const char* home = std::getenv("HOME");
  if (home) return fs::path(home) / ".local" / "share" / "vacant-systems";
  return fs::path("/tmp") / "vacant-systems";
#endif
}

fs::path shared_bin_path() {
#ifdef _WIN32
  return vendor_root() / "Shared" / "bin";
#elif defined(__APPLE__)
  return vendor_root() / "Shared" / "bin";
#else
  return vendor_root() / "shared" / "bin";
#endif
}

std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

// Move src into dst (same name). Same-volume rename first; cross-volume
// falls back to copy + delete. Best effort — a failure just leaves the
// source in place, which the resolution order still finds.
void move_file(const fs::path& src, const fs::path& dst) {
  std::error_code ec;
  fs::create_directories(dst.parent_path(), ec);
  fs::rename(src, dst, ec);
  if (ec) {
    ec.clear();
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (!ec) fs::remove(src, ec);
  }
}

}

std::string shared_bin_dir() {
  fs::path p = shared_bin_path();
  std::error_code ec;
  fs::create_directories(p, ec);
  return path_to_utf8(p);
}

std::string resolved_ffmpeg() {
  std::error_code ec;

  const char* env = std::getenv("LATHE_FFMPEG");
  if (env && *env) {
    fs::path p = path_from_utf8(env);
    if (fs::exists(p, ec)) return path_to_utf8(p);
  }

  fs::path portable = path_from_utf8(exe_dir()) / ffmpeg_name();
  if (fs::exists(portable, ec)) return path_to_utf8(portable);

  return path_to_utf8(shared_bin_path() / ffmpeg_name());
}

bool ffmpeg_exists() {
  std::error_code ec;
  return fs::exists(path_from_utf8(resolved_ffmpeg()), ec);
}

void migrate_legacy_binaries() {
  std::error_code ec;
  fs::path shared = shared_bin_path() / ffmpeg_name();
  fs::path legacy = path_from_utf8(exe_dir()) / ffmpeg_name();
  if (!fs::exists(shared, ec) && fs::exists(legacy, ec)) {
    move_file(legacy, shared);
    if (fs::exists(shared, ec)) {
      write_binary_manifest(path_to_utf8(shared),
                            "migrated from " + path_to_utf8(legacy.parent_path()),
                            "-version");
    }
  }
}

void write_binary_manifest(const std::string& binary_path_utf8,
                           const std::string& source,
                           const std::string& version_flag) {
  fs::path bin = path_from_utf8(binary_path_utf8);

  std::error_code ec;
  uint64_t size = fs::file_size(bin, ec);
  if (ec) size = 0;

  // Best-effort self-reported version (first output line). A failure or
  // hang-proof concern doesn't apply — both ffmpeg -version and
  // yt-dlp --version exit immediately.
  std::string version;
  std::vector<std::string> argv = {binary_path_utf8, version_flag};
  run_subprocess(argv, [&](const std::string& line) {
    if (version.empty() && !line.empty()) version = line;
  });

  char stamp[32] = "";
  std::time_t now = std::time(nullptr);
  if (std::tm* tm = std::gmtime(&now)) {
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", tm);
  }

  std::string json = "{\n";
  json += "  \"binary\": \"" + json_escape(path_to_utf8(bin.filename())) + "\",\n";
  json += "  \"source\": \"" + json_escape(source) + "\",\n";
  json += "  \"recorded_at\": \"" + std::string(stamp) + "\",\n";
  json += "  \"size_bytes\": " + std::to_string(size) + ",\n";
  json += "  \"version\": \"" + json_escape(version) + "\"\n";
  json += "}\n";

  fs::path manifest = bin;
  manifest.replace_extension(".json");
#ifdef _WIN32
  FILE* f = _wfopen(manifest.wstring().c_str(), L"wb");
#else
  FILE* f = std::fopen(manifest.string().c_str(), "wb");
#endif
  if (f) {
    std::fwrite(json.data(), 1, json.size(), f);
    std::fclose(f);
  }
}

}
