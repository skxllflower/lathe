#include "convert.h"

#include "process.h"
#include "progress.h"

#include <filesystem>
#include <regex>
#include <string>
#include <vector>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

namespace lathe {

namespace fs = std::filesystem;

#ifdef _WIN32
// Build a fs::path from UTF-8 in a way that survives non-ANSI characters
// on Windows (default narrow std::string ctor uses the system codepage).
static fs::path path_from_utf8(const std::string& utf8) {
  return fs::path(utf8_to_utf16(utf8));
}
static std::string path_to_utf8(const fs::path& p) {
  return utf16_to_utf8(p.wstring());
}
#else
static fs::path path_from_utf8(const std::string& utf8) { return fs::path(utf8); }
static std::string path_to_utf8(const fs::path& p) { return p.string(); }
#endif

static std::string ffmpeg_path() {
  return path_to_utf8(path_from_utf8(exe_dir()) / "ffmpeg.exe");
}

static double probe_duration_seconds(const std::string& input) {
  std::vector<std::string> argv = {ffmpeg_path(), "-hide_banner", "-i", input};
  double duration = 0.0;
  std::regex re(R"(Duration:\s*(\d+):(\d+):(\d+(?:\.\d+)?))");
  run_subprocess(argv, [&](const std::string& line) {
    std::smatch m;
    if (std::regex_search(line, m, re)) {
      double h  = std::stod(m[1].str());
      double mn = std::stod(m[2].str());
      double s  = std::stod(m[3].str());
      duration = h * 3600.0 + mn * 60.0 + s;
    }
  });
  return duration;
}

ConvertResult convert(const std::string& input, const std::string& output) {
  fs::path in_path  = path_from_utf8(input);
  fs::path out_path = path_from_utf8(output);

  std::error_code ec;
  if (!fs::exists(in_path, ec)) {
    progress_error("input file not found: " + input);
    return ConvertResult::InputNotFound;
  }

  if (out_path.has_parent_path()) {
    fs::create_directories(out_path.parent_path(), ec);
  }

  double total_duration = probe_duration_seconds(input);
  progress_start(input, output, total_duration);

  std::vector<std::string> argv = {
    ffmpeg_path(),
    "-y",
    "-i", input,
    "-progress", "pipe:1",
    "-loglevel", "error",
    "-stats_period", "0.25",
    output,
  };

  std::string last_error_line;
  double batch_time_s = -1.0;
  std::string batch_speed;

  run_subprocess(argv, [&](const std::string& line) {
    auto eq = line.find('=');
    if (eq == std::string::npos) {
      if (!line.empty()) last_error_line = line;
      return;
    }
    std::string key = line.substr(0, eq);
    std::string val = line.substr(eq + 1);

    if (key == "out_time_us" || key == "out_time_ms") {
      try { batch_time_s = std::stod(val) / 1e6; } catch (...) {}
    } else if (key == "speed") {
      batch_speed = val;
    } else if (key == "progress") {
      if (batch_time_s >= 0.0) {
        double percent = total_duration > 0.0
          ? (batch_time_s / total_duration) * 100.0
          : 0.0;
        if (percent > 100.0) percent = 100.0;
        progress_update(percent, batch_time_s, batch_speed);
      }
      batch_time_s = -1.0;
      batch_speed.clear();
    }
  });

  if (was_cancelled()) {
    fs::remove(out_path, ec);  // tear down partial output
    progress_cancelled();
    return ConvertResult::Cancelled;
  }

  if (!fs::exists(out_path, ec)) {
    progress_error(last_error_line.empty()
      ? "ffmpeg completed but output file is missing"
      : last_error_line);
    return ConvertResult::FfmpegFailed;
  }

  progress_done(output);
  return ConvertResult::Ok;
}

}
