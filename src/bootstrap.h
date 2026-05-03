#pragma once

namespace lathe {

// Returns true if ffmpeg.exe is present next to the wrapper executable.
bool ffmpeg_present();

// Best-effort: ensures ffmpeg.exe is on disk next to the wrapper. If
// already present, returns true immediately. Otherwise downloads from
// the official BtbN GPL build and extracts the archive. Emits NDJSON
// `bootstrap` progress events on stdout for the consumer UI.
bool ensure_ffmpeg();

// Convenience: makes sure every binary lathe depends on is in place.
// Currently just ffmpeg.exe — kept as its own helper so the convert
// flow has a single call site even if more deps land later.
bool ensure_required();

}
