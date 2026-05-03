#include "convert.h"

#include "bootstrap.h"
#include "process.h"
#include "progress.h"

#include <algorithm>
#include <cctype>
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

static std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

static std::string ext_of(const std::string& path) {
  auto dot = path.find_last_of('.');
  if (dot == std::string::npos) return std::string();
  return lower(path.substr(dot + 1));
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

// Build the format-specific ffmpeg flags from the option struct. Choices
// that don't apply to the chosen output container are silently dropped
// (e.g. --bitrate on a .wav target).
static void apply_options(std::vector<std::string>& args,
                          const std::string& out_ext,
                          const ConvertOptions& o) {
  if (!o.sample_rate.empty()) {
    args.push_back("-ar");
    args.push_back(o.sample_rate);
  }

  if (out_ext == "wav") {
    std::string codec;
    if      (o.bit_depth == "16")              codec = "pcm_s16le";
    else if (o.bit_depth == "24")              codec = "pcm_s24le";
    else if (o.bit_depth == "32")              codec = "pcm_s32le";
    else if (o.bit_depth == "f32" ||
             o.bit_depth == "float")           codec = "pcm_f32le";
    if (!codec.empty()) {
      args.push_back("-c:a");
      args.push_back(codec);
    }
  } else if (out_ext == "aiff" || out_ext == "aif") {
    std::string codec;
    if      (o.bit_depth == "16")              codec = "pcm_s16be";
    else if (o.bit_depth == "24")              codec = "pcm_s24be";
    else if (o.bit_depth == "32")              codec = "pcm_s32be";
    if (!codec.empty()) {
      args.push_back("-c:a");
      args.push_back(codec);
    }
  } else if (out_ext == "flac") {
    // FLAC uses sample_fmt; 24-bit FLAC is stored in s32 container.
    if (o.bit_depth == "16") {
      args.push_back("-sample_fmt"); args.push_back("s16");
    } else if (o.bit_depth == "24" || o.bit_depth == "32") {
      args.push_back("-sample_fmt"); args.push_back("s32");
    }
    if (!o.compression_level.empty()) {
      args.push_back("-compression_level");
      args.push_back(o.compression_level);
    }
  } else if (out_ext == "mp3") {
    // VBR (-q:a) takes precedence over CBR (-b:a) when both supplied.
    if (!o.vbr_quality.empty()) {
      args.push_back("-q:a");
      args.push_back(o.vbr_quality);
    } else if (!o.bitrate.empty()) {
      args.push_back("-b:a");
      args.push_back(o.bitrate);
    }
  } else if (out_ext == "ogg" || out_ext == "opus" ||
             out_ext == "aac" || out_ext == "m4a") {
    if (!o.bitrate.empty()) {
      args.push_back("-b:a");
      args.push_back(o.bitrate);
    }
  }
}

ConvertResult convert(const std::string& input,
                      const std::string& output,
                      const ConvertOptions& opts) {
  // Make sure ffmpeg.exe is on disk before we try to spawn it. On a
  // fresh checkout the binaries/ folder is empty (gitignored) and this
  // call downloads into exe_dir().
  if (!ensure_required()) {
    progress_error("required binary (ffmpeg) not available; bootstrap failed");
    return ConvertResult::BootstrapFailed;
  }

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
  };
  apply_options(argv, ext_of(output), opts);
  argv.push_back(output);

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
    fs::remove(out_path, ec);
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
