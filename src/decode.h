#pragma once
#include <string>

namespace lathe {

// Phase-3 spike: open `input` via libav, seek IN-PROCESS to `seek_sec`
// (av_seek_frame to the prior keyframe + decode-forward to the exact frame),
// decode + scale `nframes` RGBA frames (capped to `height`, no upscale) to
// stdout, and emit one WAVDESK_GEOM line on stderr. Proves the linked-decoder
// path — frame-accurate seek with NO process re-spawn — before the persistent
// decode-server is built on it. Rotation is not applied yet (TODO).
int decode_probe(const std::string& input, int height, double seek_sec, int nframes);

}
