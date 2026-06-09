#pragma once

#include <string>

namespace lathe {

enum class ConvertResult {
  Ok,
  InputNotFound,
  FfmpegFailed,
  Cancelled,
  BootstrapFailed,
};

// Per-conversion knobs. All strings; empty = "let ffmpeg pick the
// default for the chosen output format". The wrapper maps these onto
// the right ffmpeg flags based on the output extension (e.g. bit_depth
// "24" + .wav → -c:a pcm_s24le; same depth + .flac → -sample_fmt s32).
struct ConvertOptions {
  std::string sample_rate;       // e.g. "44100", "48000", "96000"
  std::string bit_depth;         // "16", "24", "32", "f32"
  std::string bitrate;           // e.g. "192k", "320k" (lossy formats)
  std::string vbr_quality;       // MP3 LAME -q:a 0..9 (0 = best VBR)
  std::string compression_level; // FLAC -compression_level 0..12
  std::string quality;           // image/video quality 0..100 (higher =
                                 // better). Mapped per output codec:
                                 // JPEG -q:v, WebP -quality, H.264/VP9 -crf.
  std::string duration;          // output duration limit in seconds (ffmpeg
                                 // -t). Empty = full file. Used by WAVdesk's
                                 // preview transcode to decode only the first
                                 // few seconds of a long lossy file.
  std::string max_height;        // downscale cap: limit output HEIGHT to this
                                 // many px, keep aspect (even width), never
                                 // upscale. Empty = source resolution. Lets
                                 // WAVdesk's video preview transcode a huge/4K
                                 // source cheaply instead of at full res.
  std::string preset;            // libx264/x265 -preset (ultrafast..veryslow).
                                 // Empty = encoder default (medium). WAVdesk's
                                 // preview passes "veryfast" for a fast encode.
  std::string fps;               // stream-frames: output frame cadence (ffmpeg
                                 // -r), forcing a known constant rate so the
                                 // frame consumer can pace. Empty = default.
  std::string start;             // stream-frames: start offset in seconds
                                 // (ffmpeg -ss before -i). The consumer seeks by
                                 // restarting the stream at a new offset. Empty
                                 // / 0 = from the beginning.
};

ConvertResult convert(const std::string& input,
                      const std::string& output,
                      const ConvertOptions& opts);

// Pull a single still frame (frame_only = true) or the audio track
// (frame_only = false) out of a video into `output`. Same ffmpeg-driven
// flow as convert() with the same NDJSON progress events; the output
// container is inferred from the output extension (e.g. .png / .wav).
ConvertResult extract(const std::string& input,
                      const std::string& output,
                      bool frame_only);

// Decode `input` to a live raw RGBA video-frame stream on stdout (no audio),
// for direct preview playback without a transcode-to-file. stdout carries ONLY
// concatenated rawvideo frames (binary, no framing markers); the negotiated
// geometry is announced ONCE on stderr as "WAVDESK_GEOM w=<W> h=<H> fps=<FPS>
// pix_fmt=rgba" before the frame bytes are usable, so the consumer knows the
// per-frame byte size. opts.max_height caps the height (keep aspect); opts.fps
// sets the cadence. Streams until end-of-file, cancel, or the consumer closing
// the pipe (a normal end of playback, not an error).
ConvertResult stream_frames(const std::string& input, const ConvertOptions& opts);

// Decode `input`'s audio track to a live raw PCM stream on stdout (no video),
// for synced native preview playback. stdout carries ONLY interleaved float32
// samples (48 kHz stereo, forced); the layout is announced once on stderr as
// "WAVDESK_APCM sr=<n> ch=<n> fmt=f32le dur=<sec>" before samples flow.
// opts.start seeks via -ss (consumer restarts the stream to seek). Streams until
// EOF, cancel, or the consumer closing the pipe.
ConvertResult stream_audio(const std::string& input, const ConvertOptions& opts);

}
