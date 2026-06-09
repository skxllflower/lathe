#include "convert.h"

#include "bootstrap.h"
#include "paths.h"
#include "process.h"
#include "progress.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <regex>
#include <string>
#include <vector>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <fcntl.h>
  #include <io.h>
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
  return resolved_ffmpeg();
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

// Parse the GUI's 0..100 quality string into a clamped int. Returns false
// (leaving *out untouched) when empty or non-numeric, so callers can skip
// the flag entirely and let ffmpeg use the codec default.
static bool parse_quality(const std::string& s, int* out) {
  if (s.empty()) return false;
  try {
    *out = std::max(0, std::min(100, std::stoi(s)));
    return true;
  } catch (...) {
    return false;
  }
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
  } else if (out_ext == "jpg" || out_ext == "jpeg") {
    // JPEG: ffmpeg -q:v runs 2 (best) .. 31 (worst). Invert the GUI's
    // 0..100 (higher = better) onto that range: 100 -> 2, 0 -> 31.
    int q;
    if (parse_quality(o.quality, &q)) {
      int qv = 31 - static_cast<int>(std::lround((q / 100.0) * 29.0));
      args.push_back("-q:v");
      args.push_back(std::to_string(qv));
    }
  } else if (out_ext == "webp") {
    // WebP -quality is already 0..100, higher = better — pass straight through.
    int q;
    if (parse_quality(o.quality, &q)) {
      args.push_back("-quality");
      args.push_back(std::to_string(q));
    }
  } else if (out_ext == "mp4" || out_ext == "mkv" ||
             out_ext == "mov" || out_ext == "m4v") {
    // H.264 (libx264 default): -crf 0 (best) .. 51 (worst). Keep the top of
    // the slider sane on file size — map 100 -> crf 18, 0 -> crf 51.
    int q;
    if (parse_quality(o.quality, &q)) {
      int crf = 51 - static_cast<int>(std::lround((q / 100.0) * 33.0));
      args.push_back("-crf");
      args.push_back(std::to_string(crf));
    }
  } else if (out_ext == "webm") {
    // VP9 constant-quality mode needs -b:v 0 alongside -crf.
    int q;
    if (parse_quality(o.quality, &q)) {
      int crf = 51 - static_cast<int>(std::lround((q / 100.0) * 33.0));
      args.push_back("-crf");
      args.push_back(std::to_string(crf));
      args.push_back("-b:v");
      args.push_back("0");
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
  // Preview-grade video knobs (WAVdesk's video preview): downscale to a max
  // height (keep aspect, even width via -2, never upscale) + a fast encoder
  // preset, so a huge/4K source transcodes quickly and light instead of pinning
  // the CPU at native resolution. Video outputs only.
  {
    const std::string oext = ext_of(output);
    const bool video_out = oext == "mp4" || oext == "mkv" || oext == "mov" ||
                           oext == "m4v" || oext == "webm";
    if (video_out && !opts.max_height.empty()) {
      argv.push_back("-vf");
      argv.push_back("scale=-2:min(" + opts.max_height + "\\,ih)");
    }
    if (video_out && !opts.preset.empty()) {
      argv.push_back("-preset");
      argv.push_back(opts.preset);
    }
  }
  // Output duration cap (-t). Placed among the output options so ffmpeg stops
  // after this many seconds of decoded output — lets a caller decode just the
  // head of a long file (WAVdesk's preview transcode) instead of the whole
  // thing. Empty = no cap (full file).
  if (!opts.duration.empty()) {
    argv.push_back("-t");
    argv.push_back(opts.duration);
  }
  argv.push_back(output);

  std::string last_error_line;
  double batch_time_s = -1.0;
  std::string batch_speed;

  int rc = run_subprocess(argv, [&](const std::string& line) {
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

  // ffmpeg's exit code is authoritative. A non-zero code (disk full, codec
  // error) means the encode failed even when a partial output file was created
  // by `-y`; a missing or zero-byte output is a failure too. Remove any partial
  // so a truncated file never looks like a clean convert to the caller.
  // file_size returns uintmax_t(-1) on error, and (-1 > 0) is TRUE for
  // unsigned — so gate on the error_code, not the size comparison, or a stat
  // failure on the fresh output reads as a clean convert.
  std::error_code sz_ec;
  const std::uintmax_t out_sz = fs::file_size(out_path, sz_ec);
  const bool out_ok = !sz_ec && out_sz > 0;
  if (rc != 0 || !out_ok) {
    fs::remove(out_path, ec);
    progress_error(!last_error_line.empty()
      ? last_error_line
      : ("ffmpeg failed (exit code " + std::to_string(rc) + ")"));
    return ConvertResult::FfmpegFailed;
  }

  progress_done(output);
  return ConvertResult::Ok;
}

ConvertResult extract(const std::string& input,
                      const std::string& output,
                      bool frame_only) {
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
  if (frame_only) {
    // Single still — grab the first decodable frame, written as one image
    // (-update 1 lets a non-%d filename hold a single picture).
    argv.push_back("-frames:v"); argv.push_back("1");
    argv.push_back("-update");   argv.push_back("1");
  } else {
    // Drop the video stream; the audio track lands in the chosen container.
    argv.push_back("-vn");
  }
  argv.push_back(output);

  std::string last_error_line;
  double batch_time_s = -1.0;
  std::string batch_speed;

  int rc = run_subprocess(argv, [&](const std::string& line) {
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

  // ffmpeg's exit code is authoritative. A non-zero code (disk full, codec
  // error) means the encode failed even when a partial output file was created
  // by `-y`; a missing or zero-byte output is a failure too. Remove any partial
  // so a truncated file never looks like a clean convert to the caller.
  // file_size returns uintmax_t(-1) on error, and (-1 > 0) is TRUE for
  // unsigned — so gate on the error_code, not the size comparison, or a stat
  // failure on the fresh output reads as a clean convert.
  std::error_code sz_ec;
  const std::uintmax_t out_sz = fs::file_size(out_path, sz_ec);
  const bool out_ok = !sz_ec && out_sz > 0;
  if (rc != 0 || !out_ok) {
    fs::remove(out_path, ec);
    progress_error(!last_error_line.empty()
      ? last_error_line
      : ("ffmpeg failed (exit code " + std::to_string(rc) + ")"));
    return ConvertResult::FfmpegFailed;
  }

  progress_done(output);
  return ConvertResult::Ok;
}

ConvertResult stream_frames(const std::string& input, const ConvertOptions& opts) {
  if (!ensure_required()) {
    std::fprintf(stderr, "stream-frames: ffmpeg not available; bootstrap failed\n");
    return ConvertResult::BootstrapFailed;
  }

  std::error_code ec;
  if (!fs::exists(path_from_utf8(input), ec)) {
    std::fprintf(stderr, "stream-frames: input not found: %s\n", input.c_str());
    return ConvertResult::InputNotFound;
  }

  // stdout must be raw binary: on Windows the CRT translates '\n' -> '\r\n' in
  // text mode, which would corrupt RGBA frame bytes. Switch to binary once,
  // before a single byte is written.
#ifdef _WIN32
  _setmode(_fileno(stdout), _O_BINARY);
#endif

  int height = 720;
  if (!opts.max_height.empty()) { try { height = std::stoi(opts.max_height); } catch (...) {} }
  if (height <= 0) height = 720;
  int fps = 24;
  if (!opts.fps.empty()) { try { fps = std::stoi(opts.fps); } catch (...) {} }
  if (fps <= 0) fps = 24;
  double start = 0.0;
  if (!opts.start.empty()) { try { start = std::stod(opts.start); } catch (...) {} }
  if (start < 0.0) start = 0.0;

  // Total duration so the consumer can size its scrubber. A quick `-i` probe
  // (no decode), same one extract/convert use.
  const double dur = probe_duration_seconds(input);

  std::vector<std::string> argv = { ffmpeg_path(), "-nostdin",
    "-loglevel", "info",  // so the Output stream line (carrying WxH) hits stderr
    "-nostats" };         // ...without the continuous progress spam
  // Input seeking (-ss BEFORE -i): fast + accurate enough for preview scrub.
  // The consumer asks for a start offset to seek by restarting the stream.
  if (start > 0.0) { argv.push_back("-ss"); argv.push_back(std::to_string(start)); }
  argv.push_back("-i");        argv.push_back(input);
  argv.push_back("-an");       // video-only for now; audio (live PCM) lands later
  argv.push_back("-vf");       argv.push_back("scale=-2:" + std::to_string(height));
  argv.push_back("-r");        argv.push_back(std::to_string(fps));
  argv.push_back("-f");        argv.push_back("rawvideo");
  argv.push_back("-pix_fmt");  argv.push_back("rgba");
  argv.push_back("pipe:1");

  // Pull the negotiated output geometry out of ffmpeg's stderr and announce it
  // once, so the consumer knows the frame byte-size before the stream is useful.
  // The output line reads e.g. "Stream #0:0: Video: rawvideo (RGBA / 0x...),
  // rgba, 406x720, ...". The fourcc "0x41424752" parses as WxH with W=0, which
  // the w>0 guard rejects, so the first valid NxM token is the real geometry.
  bool geom_emitted = false;
  auto on_err = [&](const std::string& line) {
    std::fprintf(stderr, "%s\n", line.c_str());  // echo ffmpeg diagnostics
    if (geom_emitted || line.find("Video: rawvideo") == std::string::npos) return;
    for (size_t i = 0; i + 2 < line.size(); ++i) {
      if (!std::isdigit(static_cast<unsigned char>(line[i]))) continue;
      if (i > 0 && (std::isdigit(static_cast<unsigned char>(line[i - 1])) || line[i - 1] == 'x'))
        continue;  // only start of a token
      int w = 0, h = 0;
      if (std::sscanf(line.c_str() + i, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
        std::fprintf(stderr, "WAVDESK_GEOM w=%d h=%d fps=%d dur=%.3f pix_fmt=rgba\n",
                     w, h, fps, dur);
        std::fflush(stderr);
        geom_emitted = true;
        return;
      }
    }
  };

  // Forward each decoded frame chunk to stdout. A short write means the consumer
  // closed the pipe (end of playback): return false so the child is terminated
  // and we stop decoding instead of draining the whole file.
  auto on_out = [&](const char* data, std::size_t len) -> bool {
    size_t wrote = std::fwrite(data, 1, len, stdout);
    std::fflush(stdout);
    return wrote == len;
  };

  int rc = run_subprocess_streaming(argv, on_out, on_err);
  std::fflush(stdout);

  if (was_cancelled()) return ConvertResult::Cancelled;
  // A consumer closing the pipe (or us terminating ffmpeg on its behalf) leaves
  // a non-zero exit even though frames flowed fine — not a failure. Only treat
  // it as a failure if ffmpeg never even produced a decodable stream.
  if (rc != 0 && !geom_emitted) {
    std::fprintf(stderr, "stream-frames: ffmpeg failed (exit %d): %s\n",
                 rc, last_subprocess_error().c_str());
    return ConvertResult::FfmpegFailed;
  }
  return ConvertResult::Ok;
}

ConvertResult stream_audio(const std::string& input, const ConvertOptions& opts) {
  if (!ensure_required()) {
    std::fprintf(stderr, "stream-audio: ffmpeg not available; bootstrap failed\n");
    return ConvertResult::BootstrapFailed;
  }
  std::error_code ec;
  if (!fs::exists(path_from_utf8(input), ec)) {
    std::fprintf(stderr, "stream-audio: input not found: %s\n", input.c_str());
    return ConvertResult::InputNotFound;
  }

  // Raw binary PCM on stdout (no CRT newline translation).
#ifdef _WIN32
  _setmode(_fileno(stdout), _O_BINARY);
#endif

  double start = 0.0;
  if (!opts.start.empty()) { try { start = std::stod(opts.start); } catch (...) {} }
  if (start < 0.0) start = 0.0;
  const double dur = probe_duration_seconds(input);

  // Force a fixed interleaved float32 layout so the daemon needs no probing or
  // resampling logic of its own: 48 kHz stereo f32le. ffmpeg resamples/downmixes
  // to match.
  const int sr = 48000;
  const int ch = 2;

  std::vector<std::string> argv = { ffmpeg_path(), "-nostdin",
    "-loglevel", "error", "-nostats" };
  if (start > 0.0) { argv.push_back("-ss"); argv.push_back(std::to_string(start)); }
  argv.push_back("-i");  argv.push_back(input);
  argv.push_back("-vn");                                  // audio only
  argv.push_back("-ar"); argv.push_back(std::to_string(sr));
  argv.push_back("-ac"); argv.push_back(std::to_string(ch));
  argv.push_back("-f");  argv.push_back("f32le");         // interleaved float32
  argv.push_back("pipe:1");

  // We force the format, so the layout is known up front — announce it before
  // any samples so the consumer can size its ring buffer / track position.
  std::fprintf(stderr, "WAVDESK_APCM sr=%d ch=%d fmt=f32le dur=%.3f\n", sr, ch, dur);
  std::fflush(stderr);

  bool any = false;
  auto on_out = [&](const char* data, std::size_t len) -> bool {
    any = true;
    size_t wrote = std::fwrite(data, 1, len, stdout);
    std::fflush(stdout);
    return wrote == len;
  };
  auto on_err = [&](const std::string& line) {
    std::fprintf(stderr, "%s\n", line.c_str());
  };

  int rc = run_subprocess_streaming(argv, on_out, on_err);
  std::fflush(stdout);

  if (was_cancelled()) return ConvertResult::Cancelled;
  // No samples + non-zero exit = real failure (e.g. the file has no audio
  // track). The consumer treats that as "video plays muted".
  if (rc != 0 && !any) {
    std::fprintf(stderr, "stream-audio: ffmpeg failed (exit %d): %s\n",
                 rc, last_subprocess_error().c_str());
    return ConvertResult::FfmpegFailed;
  }
  return ConvertResult::Ok;
}

ConvertResult audio_peaks(const std::string& input, int bins) {
  if (bins < 1) bins = 1;
  if (!ensure_required()) {
    std::fprintf(stderr, "audio-peaks: ffmpeg not available; bootstrap failed\n");
    return ConvertResult::BootstrapFailed;
  }
  std::error_code ec;
  if (!fs::exists(path_from_utf8(input), ec)) {
    std::fprintf(stderr, "audio-peaks: input not found: %s\n", input.c_str());
    return ConvertResult::InputNotFound;
  }

  const double dur = probe_duration_seconds(input);

  // Decode the whole audio track to mono float32 at a low rate — plenty to draw
  // a scrubber waveform, and fast / small to buffer (~8 kHz mono = ~8 MB for a
  // 4-min track). stdout (the PCM) is captured by the callback, NOT forwarded to
  // lathe's stdout, so the JSON result below has stdout to itself.
  std::vector<std::string> argv = { ffmpeg_path(), "-nostdin",
    "-loglevel", "error", "-nostats",
    "-i", input, "-vn", "-ac", "1", "-ar", "8000", "-f", "f32le", "pipe:1" };

  std::vector<float> samples;
  std::string leftover;  // partial-float bytes carried across chunk boundaries
  auto on_out = [&](const char* data, std::size_t len) -> bool {
    leftover.append(data, len);
    size_t n = leftover.size() / sizeof(float);
    if (n > 0) {
      size_t old = samples.size();
      samples.resize(old + n);
      std::memcpy(samples.data() + old, leftover.data(), n * sizeof(float));
      leftover.erase(0, n * sizeof(float));
    }
    return true;
  };
  auto on_err = [&](const std::string& line) {
    std::fprintf(stderr, "%s\n", line.c_str());
  };

  int rc = run_subprocess_streaming(argv, on_out, on_err);
  if (was_cancelled()) return ConvertResult::Cancelled;
  if (rc != 0 && samples.empty()) {
    std::fprintf(stderr, "audio-peaks: ffmpeg failed (exit %d): %s\n",
                 rc, last_subprocess_error().c_str());
    return ConvertResult::FfmpegFailed;
  }

  // Max-abs amplitude per bin across the track.
  std::vector<float> peaks(static_cast<size_t>(bins), 0.0f);
  const size_t total = samples.size();
  for (size_t i = 0; i < total; ++i) {
    size_t b = static_cast<size_t>((static_cast<double>(i) / static_cast<double>(total)) * bins);
    if (b >= static_cast<size_t>(bins)) b = static_cast<size_t>(bins) - 1;
    float a = std::fabs(samples[i]);
    if (a > peaks[b]) peaks[b] = a;
  }

  std::string out = "{\"bins\":" + std::to_string(bins) +
                    ",\"dur\":" + std::to_string(dur) + ",\"peaks\":[";
  char buf[16];
  for (int i = 0; i < bins; ++i) {
    if (i) out.push_back(',');
    std::snprintf(buf, sizeof(buf), "%.4f", peaks[static_cast<size_t>(i)]);
    out += buf;
  }
  out += "]}";
  std::fputs(out.c_str(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
  return ConvertResult::Ok;
}

}
