#include "decode.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
// std::thread is unusable here: MSVC's <thread> pulls in the CRT <process.h>,
// which collides with lathe's own process.h on the include path. Use Win32
// threads + Sleep instead.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
static void sleep_ms(int ms) { Sleep(static_cast<DWORD>(ms)); }
#else
#include <chrono>
#include <thread>
static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
#endif

#ifdef LATHE_HAVE_LIBAV
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace lathe {

int decode_probe(const std::string& input, int height, double seek_sec, int nframes) {
#ifdef _WIN32
  _setmode(_fileno(stdout), _O_BINARY);  // raw RGBA on stdout
#endif
  if (height <= 0) height = 720;
  if (nframes <= 0) nframes = 1;

  AVFormatContext* fmt = nullptr;
  if (avformat_open_input(&fmt, input.c_str(), nullptr, nullptr) < 0) {
    std::fprintf(stderr, "decode: open failed: %s\n", input.c_str());
    return 1;
  }
  if (avformat_find_stream_info(fmt, nullptr) < 0) {
    std::fprintf(stderr, "decode: no stream info\n");
    avformat_close_input(&fmt);
    return 1;
  }
  const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (vs < 0) {
    std::fprintf(stderr, "decode: no video stream\n");
    avformat_close_input(&fmt);
    return 1;
  }
  AVStream* st = fmt->streams[vs];
  const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
  AVCodecContext* dec = codec ? avcodec_alloc_context3(codec) : nullptr;
  if (!dec || avcodec_parameters_to_context(dec, st->codecpar) < 0 ||
      avcodec_open2(dec, codec, nullptr) < 0) {
    std::fprintf(stderr, "decode: codec open failed\n");
    if (dec) avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return 1;
  }

  const int cw = dec->width, ch = dec->height;
  int oH = (height > ch) ? ch : height;  // no upscale
  int oW = static_cast<int>(std::lround(static_cast<double>(cw) * oH / ch));
  oW &= ~1; oH &= ~1;
  if (oW < 2) oW = 2;
  if (oH < 2) oH = 2;
  const double dur = (fmt->duration > 0) ? static_cast<double>(fmt->duration) / AV_TIME_BASE : 0.0;
  const double fps = st->avg_frame_rate.num > 0 ? av_q2d(st->avg_frame_rate) : 30.0;

  SwsContext* sws = sws_getContext(cw, ch, dec->pix_fmt, oW, oH, AV_PIX_FMT_RGBA,
                                   SWS_BILINEAR, nullptr, nullptr, nullptr);
  AVFrame* frame = av_frame_alloc();
  AVFrame* rgba = av_frame_alloc();
  rgba->format = AV_PIX_FMT_RGBA;
  rgba->width = oW;
  rgba->height = oH;
  av_frame_get_buffer(rgba, 0);
  AVPacket* pkt = av_packet_alloc();

  // The payoff: jump to seek_sec IN-PROCESS — seek to the prior keyframe, then
  // decode-forward to the target frame. No re-spawn.
  if (seek_sec > 0.0) {
    const int64_t ts = static_cast<int64_t>(seek_sec / av_q2d(st->time_base));
    av_seek_frame(fmt, vs, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec);
  }

  std::fprintf(stderr, "WAVDESK_GEOM w=%d h=%d fps=%d dur=%.3f pix_fmt=rgba\n",
               oW, oH, static_cast<int>(std::lround(fps)), dur);
  std::fflush(stderr);

  int out = 0;
  while (out < nframes && av_read_frame(fmt, pkt) >= 0) {
    if (pkt->stream_index == vs && avcodec_send_packet(dec, pkt) >= 0) {
      while (out < nframes && avcodec_receive_frame(dec, frame) >= 0) {
        // Frame-accurate seek: drop frames before the target (keyframe..target).
        const double ft = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                              ? frame->best_effort_timestamp * av_q2d(st->time_base)
                              : 0.0;
        if (seek_sec > 0.0 && ft < seek_sec - 0.001) continue;
        sws_scale(sws, frame->data, frame->linesize, 0, ch, rgba->data, rgba->linesize);
        for (int y = 0; y < oH; y++) {
          std::fwrite(rgba->data[0] + static_cast<size_t>(y) * rgba->linesize[0], 1,
                      static_cast<size_t>(oW) * 4, stdout);
        }
        out++;
      }
    }
    av_packet_unref(pkt);
  }
  std::fflush(stdout);

  av_packet_free(&pkt);
  av_frame_free(&rgba);
  av_frame_free(&frame);
  sws_freeContext(sws);
  avcodec_free_context(&dec);
  avformat_close_input(&fmt);
  return out > 0 ? 0 : 1;
}

// --- persistent decode-server ---------------------------------------------
//
// Output protocol (stdout, both video and audio mode): a stream of chunks,
//   [8-byte LE u64 pts in MICROSECONDS][4-byte LE u32 payload length][payload]
// Video payload = one scaled RGBA frame (w*h*4 bytes, constant per stream).
// Audio payload = interleaved f32le PCM (variable length).
// Two payload-less marker forms:
//   len == 0          SEEK MARKER — everything before it predates the seek;
//                     consumers FLUSH their buffers (PTS alone can't split
//                     stale from fresh on a backward seek).
//   len == 0xFFFFFFFF WRAP MARKER — a gapless loop wrapped to pts; pure
//                     continuity, consumers REBASE position bookkeeping but
//                     must NOT flush (the buffered tail still plays).
namespace {

constexpr uint32_t kWrapMarkerLen = 0xFFFFFFFFu;

double cmd_num(const std::string& s, const char* key, double def) {
  const std::string k = std::string("\"") + key + "\"";
  auto p = s.find(k);
  if (p == std::string::npos) return def;
  p = s.find(':', p + k.size());
  if (p == std::string::npos) return def;
  ++p;
  while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
  try { return std::stod(s.substr(p)); } catch (...) { return def; }
}

bool cmd_is(const std::string& s, const char* op) {
  return s.find(std::string("\"") + op + "\"") != std::string::npos;
}

struct CtlCtx {
  std::atomic<bool>* wantClose;
  std::atomic<bool>* paused;
  std::atomic<double>* seekReq;
  std::atomic<int>* dirReq;        // playback direction riding the seek op (1 / -1)
  // Number of seek COMMANDS received but not yet consumed by the main loop.
  // seekReq alone coalesces (two commands landing within one servicing window
  // collapse to the last value), but consumers count one SEEK MARKER per
  // command they sent — a deficit leaves their stale-frame gate stuck open
  // forever (the audio daemon then discards PCM permanently: dead sound).
  // The main loop services the LAST target but emits one marker per command.
  std::atomic<int>* seekCount;
  std::atomic<double>* loopIn;     // gapless loop region; loopOut <= loopIn = off
  std::atomic<double>* loopOut;
  std::atomic<bool>* tonemap;      // HDR→SDR tone-mapping (video server only)
};

uint64_t now_ms() {
#ifdef _WIN32
  return GetTickCount64();
#else
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

// avformat interrupt hook: a pending seek (or close) aborts the blocking call
// in flight — most importantly av_seek_frame's linear cluster scan on a
// cue-less/cue-sparse matroska, which otherwise pins the main loop for seconds
// while newer seek commands (and their markers) queue behind it. Rapid seeks
// then coalesce to the LATEST target at decode level: the superseded scan
// aborts in milliseconds, the loop re-enters, emits one marker per received
// command, and services only the newest target.
struct InterruptCtx {
  std::atomic<bool>* wantClose;
  std::atomic<int>* seekCount;
};

int interrupt_cb(void* opaque) {
  auto* c = static_cast<InterruptCtx*>(opaque);
  return (c->wantClose->load(std::memory_order_relaxed) ||
          c->seekCount->load(std::memory_order_relaxed) > 0)
             ? 1
             : 0;
}

// An interrupt-aborted read LATCHES AVERROR_EXIT into the AVIOContext — every
// later read then fails instantly (mov never recovers; matroska mostly
// resyncs). Clear the latch before servicing the seek that caused the abort.
void clear_io_latch(AVFormatContext* fmt) {
  if (fmt && fmt->pb) {
    fmt->pb->eof_reached = 0;
    fmt->pb->error = 0;
  }
}

// Reads JSON-line commands on stdin and drives the atomics. Runs on its own
// thread; blocks on stdin and exits when the stream closes.
void ctl_loop(CtlCtx* c) {
  std::string line;
  int ch;
  while ((ch = std::getchar()) != EOF) {
    if (ch == '\n' || ch == '\r') {
      if (!line.empty()) {
        if (cmd_is(line, "close")) { c->wantClose->store(true); break; }
        else if (cmd_is(line, "seek")) {
          // Direction and target land BEFORE the count so the main loop, on
          // seeing count > 0, always reads values at least as new as the
          // commands it consumed.
          c->dirReq->store(cmd_num(line, "dir", 1.0) < 0 ? -1 : 1);
          c->seekReq->store(cmd_num(line, "sec", 0.0));
          c->seekCount->fetch_add(1);
        }
        else if (cmd_is(line, "loop")) {
          // In lands before out: a torn read sees out<=in (= loop off) rather
          // than a bogus region.
          c->loopIn->store(cmd_num(line, "in", 0.0));
          c->loopOut->store(cmd_num(line, "out", 0.0));
        }
        else if (cmd_is(line, "tonemap")) c->tonemap->store(cmd_num(line, "on", 1.0) != 0.0);
        else if (cmd_is(line, "pause")) c->paused->store(true);
        else if (cmd_is(line, "play")) c->paused->store(false);
        line.clear();
      }
    } else {
      line.push_back(static_cast<char>(ch));
    }
  }
  c->wantClose->store(true);  // stdin EOF
}

#ifdef _WIN32
DWORD WINAPI ctl_thread_proc(LPVOID p) { ctl_loop(static_cast<CtlCtx*>(p)); return 0; }
#endif

bool write_chunk_header(double pts_sec, uint32_t payload_bytes) {
  const uint64_t pts_us = pts_sec > 0.0 ? static_cast<uint64_t>(pts_sec * 1e6) : 0;
  uint8_t hdr[12];
  std::memcpy(hdr, &pts_us, 8);                 // x86/x64: already LE
  std::memcpy(hdr + 8, &payload_bytes, 4);
  return std::fwrite(hdr, 1, sizeof(hdr), stdout) == sizeof(hdr);
}

// Prefer D3D11 surfaces when the decoder offers them; anything else falls
// back to the first (software) format and decode proceeds on the CPU.
enum AVPixelFormat hw_get_format(AVCodecContext*, const enum AVPixelFormat* fmts) {
  for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
    if (*p == AV_PIX_FMT_D3D11) return *p;
#ifdef __APPLE__
    // macOS VideoToolbox surface — mirrors the D3D11 pick above. Anything the
    // decoder doesn't offer as VT falls through to fmts[0] (software), so a
    // codec VideoToolbox can't hw-decode stays on the CPU path.
    if (*p == AV_PIX_FMT_VIDEOTOOLBOX) return *p;
#endif
  }
  return fmts[0];
}

// HDR→SDR tone-mapping LUT: 16-bit nonlinear code value → 8-bit display gamma.
// Per-channel after the (BT.2020) YUV→RGB conversion at 16-bit — primaries
// stay 2020 (slight saturation shift on extreme colors), which is the right
// cost/quality trade for a preview toggle. PQ: ST.2084 EOTF → reference-white
// 203 nits → extended-Reinhard toward a 1000-nit peak → 2.2 gamma. HLG:
// inverse OETF → 1.2 system gamma → same.
std::vector<uint8_t> build_tonemap_lut(int hdrKind /* 1=pq 2=hlg */) {
  std::vector<uint8_t> lut(65536);
  const double Lw = 1000.0 / 203.0;  // peak in reference-white units
  for (int i = 0; i < 65536; i++) {
    const double e = i / 65535.0;
    double L;  // linear light, 1.0 = reference white (203 nits)
    if (hdrKind == 1) {
      const double m1 = 0.1593017578125, m2 = 78.84375;
      const double c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
      const double p = std::pow(e, 1.0 / m2);
      const double num = p - c1 > 0.0 ? p - c1 : 0.0;
      const double Y = std::pow(num / (c2 - c3 * p), 1.0 / m1);  // 0..1 = 0..10000 nits
      L = Y * 10000.0 / 203.0;
    } else {
      const double a = 0.17883277, b = 0.28466892, c = 0.55991073;
      const double scene = e <= 0.5 ? (e * e) / 3.0 : (std::exp((e - c) / a) + b) / 12.0;
      const double display = std::pow(scene, 1.2);  // HLG system gamma
      L = display * 1000.0 / 203.0;
    }
    double t = L * (1.0 + L / (Lw * Lw)) / (1.0 + L);  // extended Reinhard
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    lut[i] = static_cast<uint8_t>(std::lround(std::pow(t, 1.0 / 2.2) * 255.0));
  }
  return lut;
}

// Match the YUV→RGB matrix to the content: the tagged colorspace when present,
// else the HD/SD heuristic (BT.709 at ≥720p). swscale otherwise defaults to
// BT.601 coefficients, which visibly shifts hues on HD content — Chromium got
// this right, so the native path must too.
void set_sws_colorspace(SwsContext* sws, const AVCodecContext* dec, int srcH) {
  int spc;
  switch (dec->colorspace) {
    case AVCOL_SPC_BT709:      spc = SWS_CS_ITU709; break;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:  spc = SWS_CS_ITU601; break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:  spc = SWS_CS_BT2020; break;
    default:                   spc = srcH >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601; break;
  }
  const int srcRange = dec->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
  sws_setColorspaceDetails(sws, sws_getCoefficients(spc), srcRange,
                           sws_getCoefficients(SWS_CS_DEFAULT), 1 /* full-range RGB out */,
                           0, 1 << 16, 1 << 16);
}

// Clockwise display rotation (0/90/180/270) from the stream's display-matrix
// side data. av_display_rotation_get returns counterclockwise degrees; the
// rotation to APPLY for display is its negation.
int stream_rotation(const AVStream* st) {
  for (int i = 0; i < st->codecpar->nb_coded_side_data; i++) {
    const AVPacketSideData* sd = &st->codecpar->coded_side_data[i];
    if (sd->type == AV_PKT_DATA_DISPLAYMATRIX && sd->size >= 9 * 4) {
      const double theta = av_display_rotation_get(reinterpret_cast<const int32_t*>(sd->data));
      if (!std::isnan(theta)) {
        int rot = static_cast<int>(std::lround(-theta / 90.0)) * 90;
        rot %= 360;
        if (rot < 0) rot += 360;
        return rot;
      }
    }
  }
  return 0;
}

// Rotate a (sw x sh) RGBA image with row stride srcStride into a tight dst
// buffer. 90/270 produce (sh x sw); 180 produces (sw x sh).
void rotate_rgba(const uint8_t* src, int srcStride, int sw, int sh,
                 uint8_t* dst, int rot) {
  if (rot == 180) {
    for (int y = 0; y < sh; y++) {
      const uint32_t* row = reinterpret_cast<const uint32_t*>(src + static_cast<size_t>(sh - 1 - y) * srcStride);
      uint32_t* out = reinterpret_cast<uint32_t*>(dst) + static_cast<size_t>(y) * sw;
      for (int x = 0; x < sw; x++) out[x] = row[sw - 1 - x];
    }
  } else if (rot == 90) {
    const int dw = sh, dh = sw;
    for (int y = 0; y < dh; y++) {
      uint32_t* out = reinterpret_cast<uint32_t*>(dst) + static_cast<size_t>(y) * dw;
      for (int x = 0; x < dw; x++) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(src + static_cast<size_t>(dw - 1 - x) * srcStride);
        out[x] = row[y];
      }
    }
  } else if (rot == 270) {
    const int dw = sh, dh = sw;
    for (int y = 0; y < dh; y++) {
      uint32_t* out = reinterpret_cast<uint32_t*>(dst) + static_cast<size_t>(y) * dw;
      for (int x = 0; x < dw; x++) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(src + static_cast<size_t>(x) * srcStride);
        out[x] = row[dh - 1 - y];
      }
    }
  }
}

}  // namespace

int decode_server(const std::string& input, int height, double start_sec, bool no_hwaccel) {
#ifdef _WIN32
  _setmode(_fileno(stdout), _O_BINARY);  // raw frame bytes on stdout
#endif
  if (height <= 0) height = 720;

  AVFormatContext* fmt = nullptr;
  if (avformat_open_input(&fmt, input.c_str(), nullptr, nullptr) < 0 ||
      avformat_find_stream_info(fmt, nullptr) < 0) {
    std::fprintf(stderr, "decode-server: open failed: %s\n", input.c_str());
    if (fmt) avformat_close_input(&fmt);
    return 1;
  }
  int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (vs < 0) { std::fprintf(stderr, "decode-server: no video stream\n"); avformat_close_input(&fmt); return 1; }
  AVStream* st = fmt->streams[vs];
  const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
  AVCodecContext* dec = codec ? avcodec_alloc_context3(codec) : nullptr;
  AVBufferRef* hwdev = nullptr;
  bool dec_ok = dec && avcodec_parameters_to_context(dec, st->codecpar) >= 0;
  if (dec_ok) {
    // Multithreaded decode. The default is ONE thread, which made 4K H.264
    // barely-realtime and turned seek (keyframe + decode-forward to target)
    // into a multi-second stall on long-GOP files.
    dec->thread_count = 0;  // auto (core count)
    dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    dec->flags2 |= AV_CODEC_FLAG2_FAST;
#if defined(__APPLE__)
    // macOS VideoToolbox hardware decode — mirrors the Windows d3d11va path
    // below (av_hwdevice_ctx_create + get_format + per-frame transfer in
    // render_frame). Intel UHD 630 hw-decodes H.264/HEVC fine; codecs it can't
    // (AV1, VP9) are NOT attached here so avcodec_open2 opens the software
    // (dav1d) decoder instead — a prior VT-on-AV1 attempt logged 148 hw errors
    // and 0 frames, so gate the device by codec rather than let it attach and
    // starve. Any per-frame hw fault still drops to software via reopen_software.
    const bool hw_ok_codec = (codec->id == AV_CODEC_ID_H264 ||
                              codec->id == AV_CODEC_ID_HEVC);
    if (!no_hwaccel && hw_ok_codec &&
        av_hwdevice_ctx_create(&hwdev, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0) >= 0) {
      dec->hw_device_ctx = av_buffer_ref(hwdev);
      dec->get_format = hw_get_format;
    }
#else
    // Hardware decode (d3d11va) when the GPU offers it — the laptop-battery
    // path <video> used to provide. get_format picks D3D11 surfaces only when
    // the decoder lists them, so unsupported codecs/GPUs silently stay on the
    // (still multithreaded) software path; frames transfer to system memory
    // per frame in render_frame. `no_hwaccel` (set by the frame-server on a
    // respawn after a prior hw fault on this file) skips d3d11va outright.
    if (!no_hwaccel &&
        av_hwdevice_ctx_create(&hwdev, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) >= 0) {
      dec->hw_device_ctx = av_buffer_ref(hwdev);
      dec->get_format = hw_get_format;
    }
#endif
    dec_ok = avcodec_open2(dec, codec, nullptr) >= 0;
    // Opening the codec WITH hw accel can fail on a flaky GPU/driver even when
    // device creation succeeded — drop hw and open software rather than dying.
    if (!dec_ok && hwdev) {
      std::fprintf(stderr, "decode-server: hw codec open failed, retrying software\n");
      std::fflush(stderr);
      av_buffer_unref(&hwdev);
      hwdev = nullptr;
      avcodec_free_context(&dec);
      dec = avcodec_alloc_context3(codec);
      dec_ok = dec && avcodec_parameters_to_context(dec, st->codecpar) >= 0;
      if (dec_ok) {
        dec->thread_count = 0;
        dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        dec->flags2 |= AV_CODEC_FLAG2_FAST;
        dec_ok = avcodec_open2(dec, codec, nullptr) >= 0;
      }
    }
  }
  if (!dec_ok) {
    std::fprintf(stderr, "decode-server: codec open failed\n");
    if (dec) avcodec_free_context(&dec);
    if (hwdev) av_buffer_unref(&hwdev);
    avformat_close_input(&fmt);
    return 1;
  }
  bool hwActive = (hwdev != nullptr);  // hw device attached to `dec` right now

  // Output geometry is the DISPLAYED orientation: a 90/270 display-matrix
  // rotation swaps the source dims before the height cap is applied, and each
  // scaled frame is rotated into place before it's written.
  const int srcW = dec->width, srcH = dec->height;
  const int rot = stream_rotation(st);
  const bool swapDims = (rot == 90 || rot == 270);
  const int dispW = swapDims ? srcH : srcW;
  const int dispH = swapDims ? srcW : srcH;
  int oH = (height > dispH) ? dispH : height;
  int oW = static_cast<int>(std::lround(static_cast<double>(dispW) * oH / dispH));
  oW &= ~1; oH &= ~1;
  if (oW < 2) oW = 2;
  if (oH < 2) oH = 2;
  const int pW = swapDims ? oH : oW;  // pre-rotation scale target
  const int pH = swapDims ? oW : oH;
  const double dur = (fmt->duration > 0) ? static_cast<double>(fmt->duration) / AV_TIME_BASE : 0.0;
  const double fps = st->avg_frame_rate.num > 0 ? av_q2d(st->avg_frame_rate) : 30.0;

  // HDR detection (PQ / HLG transfer): reported in the geometry so the GUI can
  // offer the tone-map toggle; tone-mapping defaults ON for HDR sources (the
  // raw transfer curve looks washed-out flat on an SDR canvas).
  const int hdrKind = dec->color_trc == AVCOL_TRC_SMPTE2084     ? 1
                      : dec->color_trc == AVCOL_TRC_ARIB_STD_B67 ? 2
                                                                  : 0;

  // The scaler is built lazily off the FIRST decoded frame's actual format:
  // with d3d11va the frames arrive as NV12 after the GPU→CPU transfer, not the
  // container-declared pix_fmt.
  SwsContext* sws = nullptr;
  int swsFmt = AV_PIX_FMT_NONE;
  bool swsTm = false;
  AVFrame* frame = av_frame_alloc();
  AVFrame* hwsw = av_frame_alloc();   // GPU frame downloaded to system memory
  AVFrame* rgba = av_frame_alloc();
  rgba->format = AV_PIX_FMT_RGBA; rgba->width = pW; rgba->height = pH;
  av_frame_get_buffer(rgba, 0);
  AVFrame* rgba64 = nullptr;          // 16-bit staging for the tone-map path
  std::vector<uint8_t> tmLut;
  AVPacket* pkt = av_packet_alloc();
  const uint32_t frameBytes = static_cast<uint32_t>(oW) * oH * 4;
  std::vector<uint8_t> rotBuf(rot != 0 ? frameBytes : 0);

  std::fprintf(stderr, "WAVDESK_GEOM w=%d h=%d fps=%d dur=%.3f pix_fmt=rgba hdr=%s\n",
               oW, oH, static_cast<int>(std::lround(fps)), dur,
               hdrKind == 1 ? "pq" : hdrKind == 2 ? "hlg" : "0");
  std::fflush(stderr);

  // Control thread: JSON-line commands on stdin (seek/pause/play/close). Detached
  // — it blocks on stdin; the process exits when the main loop ends.
  std::atomic<bool> wantClose{false};
  std::atomic<bool> paused{false};
  std::atomic<double> seekReq{-1.0};
  std::atomic<int> dirReq{1};
  std::atomic<int> seekCount{0};
  std::atomic<double> loopIn{0.0};
  std::atomic<double> loopOut{0.0};
  std::atomic<bool> tonemap{hdrKind != 0};  // default ON for HDR sources
  CtlCtx ctlCtx{ &wantClose, &paused, &seekReq, &dirReq, &seekCount, &loopIn, &loopOut, &tonemap };
  InterruptCtx intrCtx{ &wantClose, &seekCount };
  fmt->interrupt_callback.callback = interrupt_cb;
  fmt->interrupt_callback.opaque = &intrCtx;
#ifdef _WIN32
  HANDLE ctlThread = CreateThread(nullptr, 0, ctl_thread_proc, &ctlCtx, 0, nullptr);
#else
  std::thread ctlThread(ctl_loop, &ctlCtx);
#endif

  double dropUntil = -1.0;  // frame-accurate seek: drop frames before this time
  uint64_t seekT0 = 0;      // seek-servicing timer (0 = no measurement pending)
  double seekTarget = 0.0;
  bool eof = false;
  bool drained = false;     // EOF flush packet sent (reset by seek)
  bool oneShot = false;     // chase the exact target frame after a seek even
                            // while paused, so a paused scrub still lands
  bool preview = false;     // surface the first decoded frame during catch-up
  bool wrapReq = false;     // gapless loop: jump back to the in-point next
  int dirState = 1;         // 1 = forward, -1 = reverse (backward-GOP chunks)
  double revHead = 0.0;     // reverse playback position (descending)
  bool hwFellBack = false;  // software re-fallback already done (at most once)
  bool hwFaultReq = false;  // a fatal hw decode error requested the fallback
  int  hwFaultErr = 0;      // the triggering AVERROR (for the stderr log line)
  double curPos = start_sec > 0.0 ? start_sec : 0.0;  // last emitted frame time

  // Scale (+ rotate, + tone-map) the decoded frame into `out` (frameBytes).
  // Downloads a GPU (d3d11va) frame to system memory first, and (re)builds the
  // scaler off the frame's true format and the tone-map state. With tone-
  // mapping the scale lands in 16-bit RGBA and a per-channel LUT folds the
  // PQ/HLG transfer down to display gamma. False = skip this frame.
  bool hwAnnounced = false;
  auto render_frame = [&](uint8_t* out) -> bool {
    AVFrame* src = frame;
    if (frame->format == AV_PIX_FMT_D3D11) {
      if (!hwAnnounced) {
        hwAnnounced = true;
        std::fprintf(stderr, "decode-server: d3d11va hardware decode active\n");
        std::fflush(stderr);
      }
      av_frame_unref(hwsw);
      if (av_hwframe_transfer_data(hwsw, frame, 0) < 0) return false;
      src = hwsw;
    }
#ifdef __APPLE__
    else if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
      if (!hwAnnounced) {
        hwAnnounced = true;
        std::fprintf(stderr, "decode: %s via videotoolbox\n", avcodec_get_name(dec->codec_id));
        std::fflush(stderr);
      }
      av_frame_unref(hwsw);
      // A VideoToolbox GPU->CPU transfer failure is per-frame, not fatal: ask the
      // main loop to reopen this file in software (reopen_software) and resume,
      // rather than dropping the picture. Mirrors the receive_frame hw-fault path.
      if (av_hwframe_transfer_data(hwsw, frame, 0) < 0) {
        if (hwActive && !hwFellBack) { hwFaultReq = true; hwFaultErr = AVERROR(EIO); }
        return false;
      }
      src = hwsw;
    } else if (!hwAnnounced) {
      // Software decode path (hw never attached, e.g. AV1 via dav1d, or after a
      // fallback): announce once so the frame-server / owner sees which path ran.
      hwAnnounced = true;
      std::fprintf(stderr, "decode: %s via software\n", avcodec_get_name(dec->codec_id));
      std::fflush(stderr);
    }
#endif
    const bool tm = hdrKind != 0 && tonemap.load();
    if (!sws || swsFmt != src->format || swsTm != tm) {
      if (sws) sws_freeContext(sws);
      sws = sws_getContext(srcW, srcH, static_cast<enum AVPixelFormat>(src->format),
                           pW, pH, tm ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGBA,
                           SWS_BILINEAR, nullptr, nullptr, nullptr);
      if (!sws) return false;
      set_sws_colorspace(sws, dec, srcH);
      swsFmt = src->format;
      swsTm = tm;
    }
    if (tm) {
      if (!rgba64) {
        rgba64 = av_frame_alloc();
        rgba64->format = AV_PIX_FMT_RGBA64LE; rgba64->width = pW; rgba64->height = pH;
        if (av_frame_get_buffer(rgba64, 0) < 0) { av_frame_free(&rgba64); return false; }
      }
      if (tmLut.empty()) tmLut = build_tonemap_lut(hdrKind);
      sws_scale(sws, src->data, src->linesize, 0, srcH, rgba64->data, rgba64->linesize);
      for (int y = 0; y < pH; y++) {
        const uint16_t* sp = reinterpret_cast<const uint16_t*>(
            rgba64->data[0] + static_cast<size_t>(y) * rgba64->linesize[0]);
        uint8_t* dp = rgba->data[0] + static_cast<size_t>(y) * rgba->linesize[0];
        for (int x = 0; x < pW; x++) {
          dp[0] = tmLut[sp[0]];
          dp[1] = tmLut[sp[1]];
          dp[2] = tmLut[sp[2]];
          dp[3] = 255;
          sp += 4; dp += 4;
        }
      }
    } else {
      sws_scale(sws, src->data, src->linesize, 0, srcH, rgba->data, rgba->linesize);
    }
    if (rot != 0) {
      rotate_rgba(rgba->data[0], rgba->linesize[0], pW, pH, out, rot);
    } else {
      for (int y = 0; y < oH; y++) {
        std::memcpy(out + static_cast<size_t>(y) * oW * 4,
                    rgba->data[0] + static_cast<size_t>(y) * rgba->linesize[0],
                    static_cast<size_t>(oW) * 4);
      }
    }
    return true;
  };
  std::vector<uint8_t> scratch(frameBytes);
  auto write_frame = [&](double ft, const uint8_t* data) -> bool {
    if (seekT0) {
      // First frame after a seek: how long the servicing (keyframe seek +
      // decode-forward) actually took. Gated — only slow seeks are worth a line.
      const uint64_t dt = now_ms() - seekT0;
      seekT0 = 0;
      if (dt > 400) {
        std::fprintf(stderr, "decode-server: video seek to %.2fs served first frame after %llu ms\n",
                     seekTarget, static_cast<unsigned long long>(dt));
        std::fflush(stderr);
      }
    }
    if (!write_chunk_header(ft, frameBytes)) return false;
    if (std::fwrite(data, 1, frameBytes, stdout) != frameBytes) return false;
    std::fflush(stdout);  // flush each frame; backpressure paces the decode
    return true;
  };

  // Small-loop frame cache: every wrap re-seeks to the keyframe before
  // the in-point and drop-decodes up to it — for a tiny region that
  // costs more than the region lasts and the stream starves (frozen
  // video each cycle). Cache one full rendered cycle and replay it at
  // wraps; any seek or bounds change invalidates and decode resumes.
  constexpr double kLoopCacheMaxSec   = 2.0;
  constexpr size_t kLoopCacheMaxBytes = 128ull * 1024 * 1024;
  std::vector<std::pair<double, std::vector<uint8_t>>> loopCache;
  bool   loopCacheOn    = false;  // capturing this cycle
  bool   loopCacheReady = false;  // full cycle stored — replay at wraps
  double loopCacheIn = -1.0, loopCacheOut = -1.0;
  auto loop_cache_reset = [&]() {
    loopCache.clear();
    loopCacheOn = false;
    loopCacheReady = false;
    loopCacheIn = -1.0;
    loopCacheOut = -1.0;
  };

  auto emit_frame = [&](double ft) -> bool {
    if (!render_frame(scratch.data())) return true;  // bad frame — skip, keep streaming
    curPos = ft;  // resume point if a hw fault forces a software reopen
    if (loopCacheOn) {
      if (loopIn.load() != loopCacheIn || loopOut.load() != loopCacheOut) {
        loop_cache_reset();  // bounds moved mid-capture
      } else if ((loopCache.size() + 1) * frameBytes > kLoopCacheMaxBytes) {
        loop_cache_reset();  // region too heavy at this resolution
      } else if (ft >= loopCacheIn - 0.25) {
        loopCache.emplace_back(ft, std::vector<uint8_t>(scratch.data(), scratch.data() + frameBytes));
      }
    }
    return write_frame(ft, scratch.data());
  };

  // Emit everything the decoder has ready. Shared by the normal per-packet
  // path and the EOF drain. A fatal receive_frame error while hw is active
  // (the catchable class of d3d11va fault) requests the software fallback.
  auto pump_decoded = [&]() {
    int rr;
    while ((rr = avcodec_receive_frame(dec, frame)) >= 0) {
      const double ft = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                            ? frame->best_effort_timestamp * av_q2d(st->time_base)
                            : 0.0;
      if (dropUntil >= 0.0 && ft < dropUntil - 0.001) {
        // Long-GOP catch-up takes a beat even multithreaded: ship the FIRST
        // decoded (keyframe-adjacent) frame instantly so a scrub shows
        // nearby content while the exact target frame is chased down. It
        // doesn't clear oneShot — the chase continues through a pause.
        if (preview) {
          preview = false;
          if (!emit_frame(ft)) { wantClose.store(true); break; }
        }
        continue;
      }
      dropUntil = -1.0;
      preview = false;
      if (!emit_frame(ft)) { wantClose.store(true); break; }
      oneShot = false;
      // Gapless loop: this frame reached the out-point — wrap to the in-point
      // (handled in the outer loop; the emitted stream stays continuous).
      const double lIn = loopIn.load(), lOut = loopOut.load();
      if (dirState > 0 && lOut > lIn + 0.01 &&
          ft >= lOut - 0.5 / (fps > 1.0 ? fps : 30.0)) {
        wrapReq = true;
        break;
      }
      // Stay responsive mid-packet. Don't break on pause here: abandoning a
      // half-drained packet makes the next send_packet EAGAIN-fail and drops
      // a frame on resume; the outer loop gates further packets instead.
      if (wantClose.load() || seekCount.load() > 0) break;
    }
    if (hwActive && !hwFellBack && rr < 0 && rr != AVERROR(EAGAIN) && rr != AVERROR_EOF) {
      hwFaultReq = true; hwFaultErr = rr;
    }
  };

  // Reverse playback: video only decodes forward, so play backward by GOP-ish
  // chunks — seek to the keyframe before revHead, decode forward keeping a
  // memory-capped ring of rendered frames below revHead, then emit the ring
  // back-to-front (descending pts). revHead advances down to the lowest
  // emitted frame; when that's the keyframe itself, the next chunk's -eps seek
  // lands in the PREVIOUS GOP. Long GOPs that overflow the ring re-decode
  // their prefix per chunk (bounded O(GOP²/chunk), cheap with threading).
  auto reverse_chunk = [&]() {
    const double chunkEnd = revHead;
    const double seekT = chunkEnd - 0.0005 > 0.0 ? chunkEnd - 0.0005 : 0.0;
    clear_io_latch(fmt);
    av_seek_frame(fmt, vs, static_cast<int64_t>(seekT / av_q2d(st->time_base)),
                  AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec);
    const size_t maxKeep = std::max<size_t>(8, (192ull << 20) / frameBytes);
    std::deque<std::pair<double, std::vector<uint8_t>>> kept;
    bool past = false, drainSent = false;
    while (!past) {
      if (wantClose.load() || seekCount.load() > 0) return;  // preempted
      const int rr = drainSent ? -1 : av_read_frame(fmt, pkt);
      if (rr < 0) {
        if (drainSent) break;
        drainSent = true;
        avcodec_send_packet(dec, nullptr);  // drain the thread pipeline
      } else if (pkt->stream_index != vs || avcodec_send_packet(dec, pkt) < 0) {
        av_packet_unref(pkt);
        continue;
      }
      int rf;
      while ((rf = avcodec_receive_frame(dec, frame)) >= 0) {
        const double ft = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                              ? frame->best_effort_timestamp * av_q2d(st->time_base)
                              : 0.0;
        if (ft >= chunkEnd - 0.0005) { past = true; continue; }
        std::vector<uint8_t> buf(frameBytes);
        if (!render_frame(buf.data())) continue;
        kept.emplace_back(ft, std::move(buf));
        if (kept.size() > maxKeep) kept.pop_front();
      }
      if (rr >= 0) av_packet_unref(pkt);
      if (hwActive && !hwFellBack && rf < 0 && rf != AVERROR(EAGAIN) && rf != AVERROR_EOF) {
        hwFaultReq = true; hwFaultErr = rf;  // main loop reopens software
        return;
      }
    }
    if (kept.empty()) { revHead = 0.0; return; }  // nothing earlier — at the start
    for (auto it = kept.rbegin(); it != kept.rend(); ++it) {
      if (wantClose.load() || seekCount.load() > 0 || paused.load()) return;
      if (!write_frame(it->first, it->second.data())) { wantClose.store(true); return; }
      revHead = it->first;  // progressive: a mid-chunk preemption stays consistent
    }
  };

  // Tear down the (hardware) decoder + input and reopen the SAME file fully in
  // software (no hw_device_ctx). Geometry constants (srcW/srcH/oW/oH/frameBytes)
  // are unchanged — same file, same scale target — so the emitted frame shape
  // stays identical and the consumer's protocol is uninterrupted. The lazily
  // (re)built sws in render_frame adapts to the new (software) pixel format.
  auto reopen_software = [&]() -> bool {
    if (sws) { sws_freeContext(sws); sws = nullptr; }
    swsFmt = AV_PIX_FMT_NONE;
    avcodec_free_context(&dec);           // also drops the hw_device_ctx ref
    if (hwdev) av_buffer_unref(&hwdev);
    hwdev = nullptr;
    avformat_close_input(&fmt);
    fmt = nullptr;
    if (avformat_open_input(&fmt, input.c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(fmt, nullptr) < 0) {
      return false;
    }
    fmt->interrupt_callback.callback = interrupt_cb;
    fmt->interrupt_callback.opaque = &intrCtx;
    vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) return false;
    st = fmt->streams[vs];
    const AVCodec* c2 = avcodec_find_decoder(st->codecpar->codec_id);
    dec = c2 ? avcodec_alloc_context3(c2) : nullptr;
    if (!dec || avcodec_parameters_to_context(dec, st->codecpar) < 0) return false;
    dec->thread_count = 0;
    dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    dec->flags2 |= AV_CODEC_FLAG2_FAST;
    return avcodec_open2(dec, c2, nullptr) >= 0;
  };

  if (start_sec > 0.0) {
    const int64_t ts = static_cast<int64_t>(start_sec / av_q2d(st->time_base));
    av_seek_frame(fmt, vs, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec);
    dropUntil = start_sec;  // stream BEGINS here — no marker
    preview = true;
  }
  while (!wantClose.load()) {
    // A fatal hardware-decode error surfaced (see the send/receive sites): drop
    // to software ONCE and resume at the current position on the same stdout
    // protocol, rather than letting the process die and blank the picture.
    if (hwFaultReq && !hwFellBack) {
      hwFaultReq = false;
      char eb[128] = {0};
      av_strerror(hwFaultErr, eb, sizeof(eb));
      std::fprintf(stderr, "decode-server: hw decode failed (%s), retrying software\n", eb);
      std::fflush(stderr);
      if (reopen_software()) {
        hwFellBack = true;
        hwActive = false;
        const double pos = curPos > 0.0 ? curPos : 0.0;
        if (dirState < 0) {
          revHead = pos;
        } else {
          const int64_t ts = static_cast<int64_t>(pos / av_q2d(st->time_base));
          av_seek_frame(fmt, vs, ts, AVSEEK_FLAG_BACKWARD);
          avcodec_flush_buffers(dec);
          dropUntil = pos > 0.0 ? pos : -1.0;
          preview = true;
        }
        eof = false;
        drained = false;
        wrapReq = false;
        loop_cache_reset();
        continue;
      }
      std::fprintf(stderr, "decode-server: software reopen failed\n");
      std::fflush(stderr);
      break;  // both hw and software failed — exit as before
    }
    if (seekCount.load() > 0) {
      // ONE marker per received command (see CtlCtx::seekCount); the count is
      // consumed FIRST so seekReq/dirReq are at least as new as the commands
      // counted. Markers go out BEFORE the (possibly slow) av_seek_frame —
      // everything already written predates the seek either way, and consumers
      // un-gate at the marker instead of sitting suppressed through a long
      // index scan.
      const int nSeeks = seekCount.exchange(0);
      const double sk = seekReq.load();
      dirState = dirReq.load();
      bool markerFail = false;
      for (int i = 0; i < nSeeks && !markerFail; ++i) markerFail = !write_chunk_header(sk, 0);
      if (markerFail) break;  // seek marker(s)
      std::fflush(stdout);
      seekT0 = now_ms();
      seekTarget = sk;
      if (dirState < 0) {
        revHead = sk;
        dropUntil = -1.0;
        oneShot = false;
        preview = false;
        eof = false;
      } else {
        clear_io_latch(fmt);
        const int64_t ts = static_cast<int64_t>(sk / av_q2d(st->time_base));
        av_seek_frame(fmt, vs, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(dec);
        dropUntil = sk;
        eof = false;
        drained = false;  // flush_buffers re-arms a drained decoder
        oneShot = true;
        preview = true;
      }
      loop_cache_reset();  // a seek invalidates the cached cycle
    }
    if (dirState < 0) {
      if (paused.load() || revHead <= 0.0005) {
        sleep_ms(8);  // reverse idles at pause / once it hits the start
        continue;
      }
      reverse_chunk();
      continue;
    }
    if ((paused.load() && !oneShot) || eof) {
      sleep_ms(8);
      continue;
    }
    const int readRc = av_read_frame(fmt, pkt);
    // An interrupt-aborted read (incoming seek/close preempted it) is not EOF:
    // loop back so the seek is serviced instead of draining the decoder.
    if (readRc < 0 && (wantClose.load() || seekCount.load() > 0)) continue;
    if (readRc < 0) {
      // End of stream: DRAIN the decoder before idling. Frame-threaded decode
      // buffers ~thread_count frames internally — without the flush packet, a
      // video shorter than the pipeline emits NOTHING at all, and every file
      // silently loses its tail.
      if (!drained) {
        drained = true;
        avcodec_send_packet(dec, nullptr);
        pump_decoded();
      }
      // A loop region whose out-point sits at/past the end wraps from EOF.
      if (!wrapReq && dirState > 0 && loopOut.load() > loopIn.load() + 0.01) {
        wrapReq = true;
      }
      if (!wrapReq) {
        eof = true;
        continue;
      }
    } else {
      if (pkt->stream_index == vs) {
        const int sr = avcodec_send_packet(dec, pkt);
        if (sr >= 0) {
          pump_decoded();
        } else if (hwActive && !hwFellBack && sr != AVERROR(EAGAIN) && sr != AVERROR_EOF) {
          hwFaultReq = true; hwFaultErr = sr;  // main loop reopens software
        }
      }
      av_packet_unref(pkt);
    }
    if (wrapReq) {
      // Gapless loop wrap: jump back to the in-point IN-PROCESS with no
      // marker — the emitted stream stays continuous (frames carry their
      // pts) while the consumer plays out its buffered tail.
      wrapReq = false;
      const double lIn = loopIn.load();
      const double lOut = loopOut.load();
      // The loop can be CLEARED between the wrap request and servicing it (a
      // live region-drag disarms mid-flight): abandon the wrap — the loads
      // above read the cleared 0/0 and the seek below would yank the stream
      // to the file start. An EOF-origin wrap re-derives eof next iteration.
      if (!(lOut > lIn + 0.01)) continue;
      // A capture that survived a full cycle is complete.
      if (loopCacheOn && !loopCache.empty() && lIn == loopCacheIn && lOut == loopCacheOut) {
        loopCacheOn = false;
        loopCacheReady = true;
      }
      // Replay path: serve whole cycles from the cache — no GOP re-decode.
      // Stays responsive: pause idles between frames, and any close/seek/
      // bounds change bails back to the normal decode path.
      if (loopCacheReady && lIn == loopCacheIn && lOut == loopCacheOut) {
        bool completed = true;
        for (const auto& f : loopCache) {
          while (paused.load() && !wantClose.load() && seekCount.load() == 0) sleep_ms(8);
          if (wantClose.load() || seekCount.load() > 0 ||
              loopIn.load() != loopCacheIn || loopOut.load() != loopCacheOut) {
            completed = false;
            break;
          }
          if (!write_frame(f.first, f.second.data())) {
            wantClose.store(true);
            completed = false;
            break;
          }
        }
        if (completed) wrapReq = true;  // next cycle replays again
        continue;
      }
      // First wrap of a small region: arm the capture for the coming cycle.
      if (!loopCacheReady && lOut > lIn + 0.01 && (lOut - lIn) <= kLoopCacheMaxSec) {
        loop_cache_reset();
        loopCacheOn = true;
        loopCacheIn = lIn;
        loopCacheOut = lOut;
      }
      clear_io_latch(fmt);
      av_seek_frame(fmt, vs, static_cast<int64_t>(lIn / av_q2d(st->time_base)),
                    AVSEEK_FLAG_BACKWARD);
      avcodec_flush_buffers(dec);
      dropUntil = lIn;
      drained = false;
      eof = false;
    }
  }

#ifdef _WIN32
  if (ctlThread) CloseHandle(ctlThread);  // blocked on stdin; dies with the process
#else
  ctlThread.detach();
#endif
  av_packet_free(&pkt);
  av_frame_free(&rgba);
  if (rgba64) av_frame_free(&rgba64);
  av_frame_free(&hwsw);
  av_frame_free(&frame);
  if (sws) sws_freeContext(sws);
  avcodec_free_context(&dec);
  if (hwdev) av_buffer_unref(&hwdev);
  avformat_close_input(&fmt);
  return 0;
}

// --- persistent AUDIO decode-server ----------------------------------------
//
// Same control protocol and chunk framing as the video server, but decodes the
// audio stream and emits interleaved 48 kHz stereo f32le PCM payloads. The
// WAVDESK_APCM stderr line matches `stream-audio`'s so the audio daemon's
// header parser is reused. Chunk PTS = the content time of the chunk's first
// sample; the daemon rebases its position cursor on the first chunk after a
// seek marker, which makes A/V position exact without sample trimming here.
int decode_server_audio(const std::string& input, double start_sec) {
#ifdef _WIN32
  _setmode(_fileno(stdout), _O_BINARY);  // raw PCM chunks on stdout
#endif

  AVFormatContext* fmt = nullptr;
  if (avformat_open_input(&fmt, input.c_str(), nullptr, nullptr) < 0 ||
      avformat_find_stream_info(fmt, nullptr) < 0) {
    std::fprintf(stderr, "decode-server: open failed: %s\n", input.c_str());
    if (fmt) avformat_close_input(&fmt);
    return 1;
  }
  const int as = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (as < 0) { std::fprintf(stderr, "decode-server: no audio stream\n"); avformat_close_input(&fmt); return 1; }
  AVStream* st = fmt->streams[as];
  const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
  AVCodecContext* dec = codec ? avcodec_alloc_context3(codec) : nullptr;
  if (!dec || avcodec_parameters_to_context(dec, st->codecpar) < 0 ||
      avcodec_open2(dec, codec, nullptr) < 0) {
    std::fprintf(stderr, "decode-server: audio codec open failed\n");
    if (dec) avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return 1;
  }
  if (dec->ch_layout.nb_channels == 0) av_channel_layout_default(&dec->ch_layout, 2);

  // Fixed output layout (matches stream-audio): 48 kHz stereo interleaved f32.
  const int outSr = 48000, outCh = 2;
  AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
  SwrContext* swr = nullptr;
  auto swr_make = [&]() -> bool {
    if (swr) swr_free(&swr);
    if (swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, outSr,
                            &dec->ch_layout, dec->sample_fmt, dec->sample_rate,
                            0, nullptr) < 0) return false;
    return swr_init(swr) >= 0;
  };
  if (dec->sample_rate <= 0 || !swr_make()) {
    std::fprintf(stderr, "decode-server: swresample init failed\n");
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return 1;
  }

  const double dur = (fmt->duration > 0) ? static_cast<double>(fmt->duration) / AV_TIME_BASE : 0.0;
  std::fprintf(stderr, "WAVDESK_APCM sr=%d ch=%d fmt=f32le dur=%.3f\n", outSr, outCh, dur);
  std::fflush(stderr);

  AVFrame* frame = av_frame_alloc();
  AVPacket* pkt = av_packet_alloc();
  std::vector<float> outBuf;

  std::atomic<bool> wantClose{false};
  std::atomic<bool> paused{false};
  std::atomic<double> seekReq{-1.0};
  std::atomic<int> dirReq{1};
  std::atomic<int> seekCount{0};
  std::atomic<double> loopIn{0.0};
  std::atomic<double> loopOut{0.0};
  std::atomic<bool> tonemapUnused{false};  // video-only op; absorbed here
  CtlCtx ctlCtx{ &wantClose, &paused, &seekReq, &dirReq, &seekCount, &loopIn, &loopOut, &tonemapUnused };
  InterruptCtx intrCtx{ &wantClose, &seekCount };
  fmt->interrupt_callback.callback = interrupt_cb;
  fmt->interrupt_callback.opaque = &intrCtx;
#ifdef _WIN32
  HANDLE ctlThread = CreateThread(nullptr, 0, ctl_thread_proc, &ctlCtx, 0, nullptr);
#else
  std::thread ctlThread(ctl_loop, &ctlCtx);
#endif

  // Seek routing. Matroska cues typically index ONLY the video track: seeking
  // on the audio stream index of such a file falls off the index and degrades
  // to a linear cluster scan from the segment start — the measured multi-
  // second (up to ~12s in the field) far-seek stall. Seek on the real video
  // stream instead when one exists (attached cover art doesn't count); the
  // demuxer repositions every stream at that cluster and dropUntil trims the
  // keyframe-early audio. Falls back to the audio index if that seek fails.
  int seekStream = as;
  {
    const int v = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (v >= 0 && !(fmt->streams[v]->disposition & AV_DISPOSITION_ATTACHED_PIC)) seekStream = v;
  }
  auto stream_seek = [&](double t) -> int {
    clear_io_latch(fmt);
    AVStream* ss = fmt->streams[seekStream];
    int rc = av_seek_frame(fmt, seekStream,
                           static_cast<int64_t>(t / av_q2d(ss->time_base)),
                           AVSEEK_FLAG_BACKWARD);
    if (rc < 0 && seekStream != as && !wantClose.load() && seekCount.load() == 0) {
      rc = av_seek_frame(fmt, as, static_cast<int64_t>(t / av_q2d(st->time_base)),
                         AVSEEK_FLAG_BACKWARD);
    }
    return rc;
  };

  double dropUntil = -1.0;
  uint64_t seekT0 = 0;      // seek-servicing timer (0 = no measurement pending)
  double seekTarget = 0.0;
  bool eof = false;
  bool drained = false;     // EOF flush packet sent (reset by seek)
  bool wrapReq = false;     // gapless loop: jump back to the in-point next
  double trimUntil = -1.0;  // sample-trim the straddling frame after a wrap
  int dirState = 1;         // 1 = forward, -1 = reverse (sample-reversed chunks)
  double revHead = 0.0;     // reverse playback position (descending)
  // First PCM after a seek: log slow servicing (a cue-less long file can cost
  // seconds in av_seek_frame's index scan — invisible in the field otherwise).
  auto note_seek_served = [&]() {
    if (!seekT0) return;
    const uint64_t dt = now_ms() - seekT0;
    seekT0 = 0;
    if (dt > 400) {
      std::fprintf(stderr, "decode-server: audio seek to %.2fs served first PCM after %llu ms\n",
                   seekTarget, static_cast<unsigned long long>(dt));
      std::fflush(stderr);
    }
  };
  auto do_seek = [&](double t) {
    stream_seek(t);
    avcodec_flush_buffers(dec);
    swr_make();  // drop resampler history from the old position
    dropUntil = t;
    eof = false;
    drained = false;
    wrapReq = false;
    trimUntil = -1.0;
  };

  // Reverse audio ("the quirk"): decode a short window ENDING at revHead
  // forward, window it precisely at 48 kHz, reverse the frame order, and emit
  // it as ONE chunk whose pts = the window END (the first emitted sample's
  // content time — the daemon's position then descends from there). Repeat
  // into earlier windows. Audio decode is so fast the re-seek per window is
  // negligible.
  const double REV_WIN_SEC = 0.8;
  auto reverse_chunk_audio = [&](std::vector<float>& acc, std::vector<float>& rev) {
    const double chunkEnd = revHead;
    const double chunkStart = chunkEnd - REV_WIN_SEC > 0.0 ? chunkEnd - REV_WIN_SEC : 0.0;
    stream_seek(chunkStart);
    avcodec_flush_buffers(dec);
    swr_make();
    acc.clear();
    double accStart = -1.0;
    bool past = false, drainSent = false;
    while (!past) {
      if (wantClose.load() || seekCount.load() > 0) return;  // preempted
      const int rr = drainSent ? -1 : av_read_frame(fmt, pkt);
      if (rr < 0) {
        if (drainSent) break;
        drainSent = true;
        avcodec_send_packet(dec, nullptr);
      } else if (pkt->stream_index != as || avcodec_send_packet(dec, pkt) < 0) {
        av_packet_unref(pkt);
        continue;
      }
      while (avcodec_receive_frame(dec, frame) >= 0) {
        const double ft = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                              ? frame->best_effort_timestamp * av_q2d(st->time_base)
                              : 0.0;
        if (accStart < 0.0) accStart = ft;
        if (ft >= chunkEnd + 0.05) { past = true; break; }
        const int outCap = static_cast<int>(av_rescale_rnd(
            swr_get_delay(swr, dec->sample_rate) + frame->nb_samples, outSr,
            dec->sample_rate, AV_ROUND_UP)) + 64;
        const size_t off = acc.size();
        acc.resize(off + static_cast<size_t>(outCap) * outCh);
        uint8_t* outPlanes[1] = { reinterpret_cast<uint8_t*>(acc.data() + off) };
        const int got = swr_convert(swr, outPlanes, outCap,
                                    const_cast<const uint8_t**>(frame->extended_data),
                                    frame->nb_samples);
        acc.resize(off + (got > 0 ? static_cast<size_t>(got) * outCh : 0));
      }
      if (rr >= 0) av_packet_unref(pkt);
    }
    const int64_t total = static_cast<int64_t>(acc.size()) / outCh;
    if (total == 0 || accStart < 0.0) { revHead = 0.0; return; }
    int64_t skip = static_cast<int64_t>(std::llround((chunkStart - accStart) * outSr));
    int64_t take = static_cast<int64_t>(std::llround((chunkEnd - chunkStart) * outSr));
    if (skip < 0) { take += skip; skip = 0; }
    if (skip > total) skip = total;
    if (take > total - skip) take = total - skip;
    if (take <= 0) {
      revHead = chunkStart > 0.0005 ? chunkStart : 0.0;  // progress regardless
      return;
    }
    rev.resize(static_cast<size_t>(take) * outCh);
    for (int64_t i = 0; i < take; ++i) {
      const float* s = acc.data() + static_cast<size_t>(skip + take - 1 - i) * outCh;
      float* d = rev.data() + static_cast<size_t>(i) * outCh;
      for (int c = 0; c < outCh; ++c) d[c] = s[c];
    }
    note_seek_served();
    const uint32_t bytes = static_cast<uint32_t>(take) * outCh * sizeof(float);
    if (!write_chunk_header(chunkEnd, bytes) ||
        std::fwrite(rev.data(), 1, bytes, stdout) != bytes) {
      wantClose.store(true);
      return;
    }
    std::fflush(stdout);
    revHead = chunkStart;
  };

  auto pump_decoded = [&]() {
    while (avcodec_receive_frame(dec, frame) >= 0) {
      const double ft = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                            ? frame->best_effort_timestamp * av_q2d(st->time_base)
                            : 0.0;
      // Drop whole frames that END before the seek target; the straddling
      // frame is kept intact — its true PTS is what the daemon rebases on.
      if (dropUntil >= 0.0 && dec->sample_rate > 0 &&
          ft + static_cast<double>(frame->nb_samples) / dec->sample_rate < dropUntil - 0.001) {
        continue;
      }
      dropUntil = -1.0;
      const int outCap = static_cast<int>(av_rescale_rnd(
          swr_get_delay(swr, dec->sample_rate) + frame->nb_samples, outSr,
          dec->sample_rate, AV_ROUND_UP)) + 64;
      outBuf.resize(static_cast<size_t>(outCap) * outCh);
      uint8_t* outPlanes[1] = { reinterpret_cast<uint8_t*>(outBuf.data()) };
      const int got = swr_convert(swr, outPlanes, outCap,
                                  const_cast<const uint8_t**>(frame->extended_data),
                                  frame->nb_samples);
      if (got <= 0) continue;

      const float* data = outBuf.data();
      int n = got;
      double pts = ft;
      // Loop wrap landed mid-frame: trim the leading samples so the loop body
      // starts EXACTLY at the in-point (sample-accurate cycle length).
      if (trimUntil >= 0.0) {
        if (pts + static_cast<double>(n) / outSr <= trimUntil + 1e-9) continue;
        if (pts < trimUntil) {
          const int lead = static_cast<int>(std::llround((trimUntil - pts) * outSr));
          if (lead >= n) continue;
          data += static_cast<size_t>(lead) * outCh;
          n -= lead;
          pts = trimUntil;
        }
        trimUntil = -1.0;
      }
      // Gapless loop: trim the chunk AT the out-point, then wrap.
      const double lIn = loopIn.load(), lOut = loopOut.load();
      const bool looping = dirState > 0 && lOut > lIn + 0.01;
      bool wrap = false;
      if (looping && pts >= lOut - 1e-9) {
        n = 0;
        wrap = true;
      } else if (looping && pts + static_cast<double>(n) / outSr > lOut) {
        n = static_cast<int>(std::llround((lOut - pts) * outSr));
        wrap = true;
      }
      if (n > 0) {
        note_seek_served();
        const uint32_t bytes = static_cast<uint32_t>(n) * outCh * sizeof(float);
        if (!write_chunk_header(pts, bytes) ||
            std::fwrite(data, 1, bytes, stdout) != bytes) {
          wantClose.store(true);
          break;
        }
        std::fflush(stdout);  // backpressure paces the decode
      }
      if (wrap) { wrapReq = true; break; }
      if (wantClose.load() || seekCount.load() > 0) break;
    }
  };

  std::vector<float> accBuf, revBuf;  // reverse-window scratch (reused)
  if (start_sec > 0.0) do_seek(start_sec);  // stream BEGINS here — no marker

  while (!wantClose.load()) {
    if (seekCount.load() > 0) {
      // ONE marker per received command (see CtlCtx::seekCount) — the daemon
      // decrements its seek-pending gate per marker, and a deficit mutes the
      // stream permanently. Markers first, even while paused: the daemon
      // flushes its ring buffer and rebases position at the marker, so a
      // paused seek lands instantly (and a slow av_seek_frame below doesn't
      // keep the position cursor suppressed meanwhile).
      const int nSeeks = seekCount.exchange(0);
      const double sk = seekReq.load();
      dirState = dirReq.load();
      bool markerFail = false;
      for (int i = 0; i < nSeeks && !markerFail; ++i) markerFail = !write_chunk_header(sk, 0);
      if (markerFail) break;
      std::fflush(stdout);
      seekT0 = now_ms();
      seekTarget = sk;
      if (dirState < 0) {
        revHead = sk;
        eof = false;
      } else {
        do_seek(sk);
      }
    }
    if (dirState < 0) {
      if (paused.load() || revHead <= 0.0005) {
        sleep_ms(8);  // reverse idles at pause / once it hits the start
        continue;
      }
      reverse_chunk_audio(accBuf, revBuf);
      continue;
    }
    if (paused.load() || eof) {
      sleep_ms(8);
      continue;
    }
    const int readRc = av_read_frame(fmt, pkt);
    // Interrupt-aborted read (incoming seek/close) is not EOF — service it.
    if (readRc < 0 && (wantClose.load() || seekCount.load() > 0)) continue;
    if (readRc < 0) {
      // Drain decoder-buffered tail samples before idling (see video twin).
      if (!drained) {
        drained = true;
        avcodec_send_packet(dec, nullptr);
        pump_decoded();
      }
      // A loop region whose out-point sits at/past the end wraps from EOF.
      if (!wrapReq && dirState > 0 && loopOut.load() > loopIn.load() + 0.01) {
        wrapReq = true;
      }
      if (!wrapReq) {
        eof = true;
        continue;
      }
    } else {
      if (pkt->stream_index == as && avcodec_send_packet(dec, pkt) >= 0) {
        pump_decoded();
      }
      av_packet_unref(pkt);
    }
    if (wrapReq) {
      // Gapless loop wrap: tell the consumer (WRAP marker — rebase, don't
      // flush), then jump to the in-point and sample-trim onto it.
      wrapReq = false;
      const double lIn = loopIn.load();
      // Cleared between the wrap request and servicing it (a live
      // region-drag disarms mid-flight): abandon the wrap — a marker at the
      // cleared 0-bounds would rebase the consumer's position to 0 and
      // re-cue the stream to the file start. EOF re-derives next iteration.
      if (!(loopOut.load() > lIn + 0.01)) continue;
      if (!write_chunk_header(lIn, kWrapMarkerLen)) break;
      std::fflush(stdout);
      do_seek(lIn);
      trimUntil = lIn;
    }
  }

#ifdef _WIN32
  if (ctlThread) CloseHandle(ctlThread);  // blocked on stdin; dies with the process
#else
  ctlThread.detach();
#endif
  av_packet_free(&pkt);
  av_frame_free(&frame);
  swr_free(&swr);
  avcodec_free_context(&dec);
  avformat_close_input(&fmt);
  return 0;
}

}  // namespace lathe
#else
namespace lathe {
int decode_probe(const std::string&, int, double, int) {
  std::fprintf(stderr, "decode-probe: libav not built in (LGPL ffmpeg dev package missing)\n");
  return 1;
}
int decode_server(const std::string&, int, double, bool) {
  std::fprintf(stderr, "decode-server: libav not built in (LGPL ffmpeg dev package missing)\n");
  return 1;
}
int decode_server_audio(const std::string&, double) {
  std::fprintf(stderr, "decode-server: libav not built in (LGPL ffmpeg dev package missing)\n");
  return 1;
}
}  // namespace lathe
#endif
