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

// Persistent decoder: opens `input` once, streams chunks on stdout — each is
// [8-byte LE u64 PTS micros][4-byte LE u32 payload bytes][payload], where a
// zero-length chunk is a SEEK MARKER (consumers flush buffers at it). Video
// payload = one display-rotated scaled RGBA frame; emits WAVDESK_GEOM on
// stderr. Reads JSON-line commands on stdin: {"op":"seek","sec":<t>} (frame-
// accurate IN-PROCESS av_seek_frame, no re-spawn; delivers one frame even
// while paused so a paused scrub updates the picture), {"op":"pause"}/
// {"op":"play"}, {"op":"close"}. Backpressure paces the decode. The control
// protocol the frame-server / engine drive instead of spawning per seek.
int decode_server(const std::string& input, int height, double start_sec);

// Audio sibling (`decode-server --audio`): same control protocol and chunk
// framing, payload = interleaved 48 kHz stereo f32le PCM; WAVDESK_APCM on
// stderr (matches stream-audio so the audio daemon's parser is reused).
int decode_server_audio(const std::string& input, double start_sec);

}
