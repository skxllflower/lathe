#pragma once

#include <string>

namespace lathe {

// %LOCALAPPDATA%\Vacant Systems\Shared\bin (created on demand) — the
// vendor-shared home for runtime-fetched tool binaries; ffmpeg.exe lives
// here once for every Vacant Systems app (Latch resolves the same dir).
// macOS: ~/Library/Application Support/Vacant Systems/Shared/bin
// Linux:  $XDG_DATA_HOME/vacant-systems/shared/bin (~/.local/share fallback)
std::string shared_bin_dir();

// The ffmpeg binary to use. Resolution order:
//   1. LATHE_FFMPEG env var (explicit override)
//   2. ffmpeg.exe next to lathe.exe (portable override)
//   3. Shared\bin (the normal home; bootstrap downloads here)
// Always returns a path — when nothing exists yet it returns the Shared\bin
// target so bootstrap and error messages agree on where it belongs.
std::string resolved_ffmpeg();

// True when resolved_ffmpeg() points at an existing file.
bool ffmpeg_exists();

// One-time tidy-up: pre-vendor-folder bootstraps left ffmpeg.exe next to
// the executable. Move it into Shared\bin when Shared has none. Gated on
// the Shared copy being absent, so a deliberately-placed portable copy is
// never stolen later (portable copies still win via the resolution order).
void migrate_legacy_binaries();

// Write <stem>.json next to a managed binary recording where it came from,
// when, its size, and its self-reported version (best effort) — the basis
// for future update checks, which today's exists()-only logic can't do.
void write_binary_manifest(const std::string& binary_path_utf8,
                           const std::string& source,
                           const std::string& version_flag);

}
