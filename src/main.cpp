#include "bootstrap.h"
#include "convert.h"
#include "decode.h"
#include "libav.h"
#include "paths.h"
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
    "lathe " "0.2.0" " - any-to-any media converter (ffmpeg wrapper)\n"
    "\n"
    "Usage:\n"
    "  lathe convert <input> <output> [options]\n"
    "  lathe extract-frame <input> <output>\n"
    "  lathe extract-audio <input> <output>\n"
    "  lathe stream-frames <input> [--height=<px>] [--fps=<n>] [--start=<sec>]\n"
    "  lathe stream-audio <input> [--start=<sec>]\n"
    "  lathe decode-server <input> [--height=<px>] [--start=<sec>] [--audio]\n"
    "  lathe audio-peaks <input> [--bins=<n>]\n"
    "  lathe bootstrap\n"
    "  lathe --version\n"
    "  lathe --help\n"
    "\n"
    "Convert options (verbose; all optional):\n"
    "  --sample-rate=<hz>          44100 / 48000 / 96000 / 192000\n"
    "  --bit-depth=<n>             16 / 24 / 32 / f32 (PCM-style outputs)\n"
    "  --bitrate=<rate>            192k / 256k / 320k (lossy outputs)\n"
    "  --vbr-quality=<n>           MP3 LAME -q:a 0..9 (0 = best VBR)\n"
    "  --compression-level=<n>     FLAC compression 0..12 (5 = default)\n"
    "  --quality=<n>               image/video quality 0..100 (higher = better)\n"
    "  --duration=<sec>            cap output to first <sec> seconds (ffmpeg -t)\n"
    "  --max-height=<px>           downscale: cap output height, keep aspect\n"
    "  --preset=<name>             x264/x265 preset (ultrafast..veryslow)\n"
    "  --fps=<n>                   GIF target: frame rate (default 15)\n"
    "  --colors=<n>                GIF target: palette size 2..256 (default 256)\n"
    "  --copy                      remux only: copy streams, no re-encode\n"
    "\n"
    "On Windows, paths and filenames are UTF-8 (non-ASCII characters\n"
    "are preserved end-to-end). ffmpeg.exe is resolved from the\n"
    "LATHE_FFMPEG env var, then next to the executable (portable\n"
    "override), then the Vacant Systems shared bin — and downloaded\n"
    "into the shared bin on first run when missing.\n"
    "Run `lathe bootstrap` to pre-fetch without doing a conversion.\n"
    "\n"
    "Progress is emitted as newline-delimited JSON on stdout, one event\n"
    "per line: bootstrap / start / progress / done / cancelled / error.\n"
  );
  return 0;
}

// Tiny --key=value parser. Supports --key=value and bare flags. Unknown
// flags terminate parsing with an error to surface typos early.
bool parse_kv(const std::string& a, const std::string& key, std::string* out) {
  if (a.rfind("--" + key + "=", 0) == 0) {
    *out = a.substr(2 + key.size() + 1);
    return true;
  }
  return false;
}

int run_cli(const std::vector<std::string>& args) {
  if (args.size() < 2) return print_help();

  const std::string& cmd = args[1];

  if (cmd == "--help" || cmd == "-h") return print_help();
  if (cmd == "--version" || cmd == "-v") {
    std::puts("lathe 0.2.0");
    return 0;
  }
  if (cmd == "libav-version") {
    std::fputs(lathe::libav_versions().c_str(), stdout);
    return 0;
  }
  if (cmd == "decode-probe") {
    if (args.size() < 3) {
      std::fputs("error: decode-probe requires <input>\n", stderr);
      return 2;
    }
    int height = 720, frames = 1;
    double seek = 0.0;
    for (size_t i = 3; i < args.size(); ++i) {
      std::string v;
      if      (parse_kv(args[i], "height", &v)) { try { height = std::stoi(v); } catch (...) {} }
      else if (parse_kv(args[i], "seek",   &v)) { try { seek   = std::stod(v); } catch (...) {} }
      else if (parse_kv(args[i], "frames", &v)) { try { frames = std::stoi(v); } catch (...) {} }
      else { std::fprintf(stderr, "error: unknown argument '%s'\n", args[i].c_str()); return 2; }
    }
    return lathe::decode_probe(args[2], height, seek, frames);
  }
  if (cmd == "decode-server") {
    if (args.size() < 3) {
      std::fputs("error: decode-server requires <input>\n", stderr);
      return 2;
    }
    int height = 720;
    double start = 0.0;
    bool audio = false;
    for (size_t i = 3; i < args.size(); ++i) {
      std::string v;
      if      (parse_kv(args[i], "height", &v)) { try { height = std::stoi(v); } catch (...) {} }
      else if (parse_kv(args[i], "start",  &v)) { try { start  = std::stod(v); } catch (...) {} }
      else if (args[i] == "--audio") { audio = true; }
      else { std::fprintf(stderr, "error: unknown argument '%s'\n", args[i].c_str()); return 2; }
    }
    return audio ? lathe::decode_server_audio(args[2], start)
                 : lathe::decode_server(args[2], height, start);
  }

  // Pre-vendor-folder bootstraps left ffmpeg.exe next to the executable;
  // adopt it into the shared bin once so it isn't re-downloaded. After the
  // trivial --help/--version returns so a pure help query never moves files.
  lathe::migrate_legacy_binaries();

  if (cmd == "bootstrap") {
    return lathe::ensure_required() ? 0 : 1;
  }

  if (cmd == "convert") {
    if (args.size() < 4) {
      std::fputs("error: convert requires <input> <output>\n", stderr);
      return 2;
    }
    lathe::ConvertOptions opts;
    for (size_t i = 4; i < args.size(); ++i) {
      const std::string& a = args[i];
      if      (parse_kv(a, "sample-rate",       &opts.sample_rate))       continue;
      else if (parse_kv(a, "bit-depth",         &opts.bit_depth))         continue;
      else if (parse_kv(a, "bitrate",           &opts.bitrate))           continue;
      else if (parse_kv(a, "vbr-quality",       &opts.vbr_quality))       continue;
      else if (parse_kv(a, "compression-level", &opts.compression_level)) continue;
      else if (parse_kv(a, "quality",           &opts.quality))           continue;
      else if (parse_kv(a, "duration",          &opts.duration))          continue;
      else if (parse_kv(a, "max-height",        &opts.max_height))        continue;
      else if (parse_kv(a, "preset",            &opts.preset))            continue;
      else if (parse_kv(a, "fps",               &opts.fps))               continue;
      else if (parse_kv(a, "colors",            &opts.colors))            continue;
      else if (a == "--copy") { opts.copy_streams = true; continue; }
      std::fprintf(stderr, "error: unknown argument '%s'\n", a.c_str());
      return 2;
    }
    auto r = lathe::convert(args[2], args[3], opts);
    switch (r) {
      case lathe::ConvertResult::Ok:               return 0;
      case lathe::ConvertResult::Cancelled:        return 130;
      case lathe::ConvertResult::InputNotFound:    return 1;
      case lathe::ConvertResult::FfmpegFailed:     return 1;
      case lathe::ConvertResult::BootstrapFailed:  return 1;
    }
    return 1;
  }

  if (cmd == "extract-frame" || cmd == "extract-audio") {
    if (args.size() < 4) {
      std::fputs("error: extract requires <input> <output>\n", stderr);
      return 2;
    }
    bool frame_only = (cmd == "extract-frame");
    auto r = lathe::extract(args[2], args[3], frame_only);
    switch (r) {
      case lathe::ConvertResult::Ok:               return 0;
      case lathe::ConvertResult::Cancelled:        return 130;
      case lathe::ConvertResult::InputNotFound:    return 1;
      case lathe::ConvertResult::FfmpegFailed:     return 1;
      case lathe::ConvertResult::BootstrapFailed:  return 1;
    }
    return 1;
  }

  if (cmd == "stream-frames") {
    if (args.size() < 3) {
      std::fputs("error: stream-frames requires <input>\n", stderr);
      return 2;
    }
    lathe::ConvertOptions opts;
    for (size_t i = 3; i < args.size(); ++i) {
      const std::string& a = args[i];
      if      (parse_kv(a, "max-height", &opts.max_height)) continue;
      else if (parse_kv(a, "height",     &opts.max_height)) continue;  // alias
      else if (parse_kv(a, "fps",        &opts.fps))        continue;
      else if (parse_kv(a, "start",      &opts.start))      continue;
      std::fprintf(stderr, "error: unknown argument '%s'\n", a.c_str());
      return 2;
    }
    auto r = lathe::stream_frames(args[2], opts);
    switch (r) {
      case lathe::ConvertResult::Ok:               return 0;
      case lathe::ConvertResult::Cancelled:        return 130;
      case lathe::ConvertResult::InputNotFound:    return 1;
      case lathe::ConvertResult::FfmpegFailed:     return 1;
      case lathe::ConvertResult::BootstrapFailed:  return 1;
    }
    return 1;
  }

  if (cmd == "stream-audio") {
    if (args.size() < 3) {
      std::fputs("error: stream-audio requires <input>\n", stderr);
      return 2;
    }
    lathe::ConvertOptions opts;
    for (size_t i = 3; i < args.size(); ++i) {
      const std::string& a = args[i];
      if (parse_kv(a, "start", &opts.start)) continue;
      std::fprintf(stderr, "error: unknown argument '%s'\n", a.c_str());
      return 2;
    }
    auto r = lathe::stream_audio(args[2], opts);
    switch (r) {
      case lathe::ConvertResult::Ok:               return 0;
      case lathe::ConvertResult::Cancelled:        return 130;
      case lathe::ConvertResult::InputNotFound:    return 1;
      case lathe::ConvertResult::FfmpegFailed:     return 1;
      case lathe::ConvertResult::BootstrapFailed:  return 1;
    }
    return 1;
  }

  if (cmd == "audio-peaks") {
    if (args.size() < 3) {
      std::fputs("error: audio-peaks requires <input>\n", stderr);
      return 2;
    }
    int bins = 2000;
    for (size_t i = 3; i < args.size(); ++i) {
      std::string v;
      if (parse_kv(args[i], "bins", &v)) { try { bins = std::stoi(v); } catch (...) {} continue; }
      std::fprintf(stderr, "error: unknown argument '%s'\n", args[i].c_str());
      return 2;
    }
    auto r = lathe::audio_peaks(args[2], bins);
    switch (r) {
      case lathe::ConvertResult::Ok:               return 0;
      case lathe::ConvertResult::Cancelled:        return 130;
      case lathe::ConvertResult::InputNotFound:    return 1;
      case lathe::ConvertResult::FfmpegFailed:     return 1;
      case lathe::ConvertResult::BootstrapFailed:  return 1;
    }
    return 1;
  }

  std::fprintf(stderr, "error: unknown command '%s'\n", cmd.c_str());
  return 2;
}

}

int main(int /*argc*/, char** /*argv*/) {
#ifdef _WIN32
  int wargc = 0;
  LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  if (!wargv) return 1;
  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(wargc));
  for (int i = 0; i < wargc; ++i) {
    args.push_back(lathe::utf16_to_utf8(wargv[i]));
  }
  LocalFree(wargv);

  SetConsoleOutputCP(CP_UTF8);
  return run_cli(args);
#else
  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
  return run_cli(args);
#endif
}
