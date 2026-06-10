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
// A ZERO-length chunk is a SEEK MARKER: everything before it in the stream
// predates the seek, everything after is from the new position. Consumers
// flush their buffers at the marker — that's what makes in-process seek
// glitch-proof even with stale data still in flight in the pipes (PTS alone
// can't distinguish stale from fresh on a backward seek).
namespace {

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
  std::atomic<int>* dirReq;   // playback direction riding the seek op (1 / -1)
};

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
          // Direction lands BEFORE the seek time so the main loop can't see
          // the new seek with the old direction.
          c->dirReq->store(cmd_num(line, "dir", 1.0) < 0 ? -1 : 1);
          c->seekReq->store(cmd_num(line, "sec", 0.0));
        }
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

int decode_server(const std::string& input, int height, double start_sec) {
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
  const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (vs < 0) { std::fprintf(stderr, "decode-server: no video stream\n"); avformat_close_input(&fmt); return 1; }
  AVStream* st = fmt->streams[vs];
  const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
  AVCodecContext* dec = codec ? avcodec_alloc_context3(codec) : nullptr;
  bool dec_ok = dec && avcodec_parameters_to_context(dec, st->codecpar) >= 0;
  if (dec_ok) {
    // Multithreaded decode. The default is ONE thread, which made 4K H.264
    // barely-realtime and turned seek (keyframe + decode-forward to target)
    // into a multi-second stall on long-GOP files.
    dec->thread_count = 0;  // auto (core count)
    dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    dec->flags2 |= AV_CODEC_FLAG2_FAST;
    dec_ok = avcodec_open2(dec, codec, nullptr) >= 0;
  }
  if (!dec_ok) {
    std::fprintf(stderr, "decode-server: codec open failed\n");
    if (dec) avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return 1;
  }

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

  SwsContext* sws = sws_getContext(srcW, srcH, dec->pix_fmt, pW, pH, AV_PIX_FMT_RGBA,
                                   SWS_BILINEAR, nullptr, nullptr, nullptr);
  AVFrame* frame = av_frame_alloc();
  AVFrame* rgba = av_frame_alloc();
  rgba->format = AV_PIX_FMT_RGBA; rgba->width = pW; rgba->height = pH;
  av_frame_get_buffer(rgba, 0);
  AVPacket* pkt = av_packet_alloc();
  const uint32_t frameBytes = static_cast<uint32_t>(oW) * oH * 4;
  std::vector<uint8_t> rotBuf(rot != 0 ? frameBytes : 0);

  std::fprintf(stderr, "WAVDESK_GEOM w=%d h=%d fps=%d dur=%.3f pix_fmt=rgba\n",
               oW, oH, static_cast<int>(std::lround(fps)), dur);
  std::fflush(stderr);

  // Control thread: JSON-line commands on stdin (seek/pause/play/close). Detached
  // — it blocks on stdin; the process exits when the main loop ends.
  std::atomic<bool> wantClose{false};
  std::atomic<bool> paused{false};
  std::atomic<double> seekReq{-1.0};
  std::atomic<int> dirReq{1};
  CtlCtx ctlCtx{ &wantClose, &paused, &seekReq, &dirReq };
#ifdef _WIN32
  HANDLE ctlThread = CreateThread(nullptr, 0, ctl_thread_proc, &ctlCtx, 0, nullptr);
#else
  std::thread ctlThread(ctl_loop, &ctlCtx);
#endif

  double dropUntil = -1.0;  // frame-accurate seek: drop frames before this time
  bool eof = false;
  bool drained = false;     // EOF flush packet sent (reset by seek)
  bool oneShot = false;     // chase the exact target frame after a seek even
                            // while paused, so a paused scrub still lands
  bool preview = false;     // surface the first decoded frame during catch-up
  int dirState = 1;         // 1 = forward, -1 = reverse (backward-GOP chunks)
  double revHead = 0.0;     // reverse playback position (descending)

  // Scale (+ rotate) the decoded frame into `out` (frameBytes).
  auto render_frame = [&](uint8_t* out) {
    sws_scale(sws, frame->data, frame->linesize, 0, srcH, rgba->data, rgba->linesize);
    if (rot != 0) {
      rotate_rgba(rgba->data[0], rgba->linesize[0], pW, pH, out, rot);
    } else {
      for (int y = 0; y < oH; y++) {
        std::memcpy(out + static_cast<size_t>(y) * oW * 4,
                    rgba->data[0] + static_cast<size_t>(y) * rgba->linesize[0],
                    static_cast<size_t>(oW) * 4);
      }
    }
  };
  std::vector<uint8_t> scratch(frameBytes);
  auto write_frame = [&](double ft, const uint8_t* data) -> bool {
    if (!write_chunk_header(ft, frameBytes)) return false;
    if (std::fwrite(data, 1, frameBytes, stdout) != frameBytes) return false;
    std::fflush(stdout);  // flush each frame; backpressure paces the decode
    return true;
  };
  auto emit_frame = [&](double ft) -> bool {
    render_frame(scratch.data());
    return write_frame(ft, scratch.data());
  };

  // Emit everything the decoder has ready. Shared by the normal per-packet
  // path and the EOF drain.
  auto pump_decoded = [&]() {
    while (avcodec_receive_frame(dec, frame) >= 0) {
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
      // Stay responsive mid-packet. Don't break on pause here: abandoning a
      // half-drained packet makes the next send_packet EAGAIN-fail and drops
      // a frame on resume; the outer loop gates further packets instead.
      if (wantClose.load() || seekReq.load() >= 0.0) break;
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
    av_seek_frame(fmt, vs, static_cast<int64_t>(seekT / av_q2d(st->time_base)),
                  AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec);
    const size_t maxKeep = std::max<size_t>(8, (192ull << 20) / frameBytes);
    std::deque<std::pair<double, std::vector<uint8_t>>> kept;
    bool past = false, drainSent = false;
    while (!past) {
      if (wantClose.load() || seekReq.load() >= 0.0) return;  // preempted
      const int rr = drainSent ? -1 : av_read_frame(fmt, pkt);
      if (rr < 0) {
        if (drainSent) break;
        drainSent = true;
        avcodec_send_packet(dec, nullptr);  // drain the thread pipeline
      } else if (pkt->stream_index != vs || avcodec_send_packet(dec, pkt) < 0) {
        av_packet_unref(pkt);
        continue;
      }
      while (avcodec_receive_frame(dec, frame) >= 0) {
        const double ft = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                              ? frame->best_effort_timestamp * av_q2d(st->time_base)
                              : 0.0;
        if (ft >= chunkEnd - 0.0005) { past = true; continue; }
        std::vector<uint8_t> buf(frameBytes);
        render_frame(buf.data());
        kept.emplace_back(ft, std::move(buf));
        if (kept.size() > maxKeep) kept.pop_front();
      }
      if (rr >= 0) av_packet_unref(pkt);
    }
    if (kept.empty()) { revHead = 0.0; return; }  // nothing earlier — at the start
    for (auto it = kept.rbegin(); it != kept.rend(); ++it) {
      if (wantClose.load() || seekReq.load() >= 0.0 || paused.load()) return;
      if (!write_frame(it->first, it->second.data())) { wantClose.store(true); return; }
      revHead = it->first;  // progressive: a mid-chunk preemption stays consistent
    }
  };

  if (start_sec > 0.0) {
    const int64_t ts = static_cast<int64_t>(start_sec / av_q2d(st->time_base));
    av_seek_frame(fmt, vs, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec);
    dropUntil = start_sec;  // stream BEGINS here — no marker
    preview = true;
  }
  while (!wantClose.load()) {
    const double sk = seekReq.exchange(-1.0);
    if (sk >= 0.0) {
      dirState = dirReq.load();
      if (dirState < 0) {
        revHead = sk;
        dropUntil = -1.0;
        oneShot = false;
        preview = false;
        eof = false;
      } else {
        const int64_t ts = static_cast<int64_t>(sk / av_q2d(st->time_base));
        av_seek_frame(fmt, vs, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(dec);
        dropUntil = sk;
        eof = false;
        drained = false;  // flush_buffers re-arms a drained decoder
        oneShot = true;
        preview = true;
      }
      if (!write_chunk_header(sk, 0)) break;  // seek marker
      std::fflush(stdout);
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
    if (av_read_frame(fmt, pkt) < 0) {
      // End of stream: DRAIN the decoder before idling. Frame-threaded decode
      // buffers ~thread_count frames internally — without the flush packet, a
      // video shorter than the pipeline emits NOTHING at all, and every file
      // silently loses its tail.
      if (!drained) {
        drained = true;
        avcodec_send_packet(dec, nullptr);
        pump_decoded();
      }
      eof = true;
      continue;
    }
    if (pkt->stream_index == vs && avcodec_send_packet(dec, pkt) >= 0) {
      pump_decoded();
    }
    av_packet_unref(pkt);
  }

#ifdef _WIN32
  if (ctlThread) CloseHandle(ctlThread);  // blocked on stdin; dies with the process
#else
  ctlThread.detach();
#endif
  av_packet_free(&pkt);
  av_frame_free(&rgba);
  av_frame_free(&frame);
  sws_freeContext(sws);
  avcodec_free_context(&dec);
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
  CtlCtx ctlCtx{ &wantClose, &paused, &seekReq, &dirReq };
#ifdef _WIN32
  HANDLE ctlThread = CreateThread(nullptr, 0, ctl_thread_proc, &ctlCtx, 0, nullptr);
#else
  std::thread ctlThread(ctl_loop, &ctlCtx);
#endif

  double dropUntil = -1.0;
  bool eof = false;
  bool drained = false;  // EOF flush packet sent (reset by seek)
  int dirState = 1;      // 1 = forward, -1 = reverse (sample-reversed chunks)
  double revHead = 0.0;  // reverse playback position (descending)
  auto do_seek = [&](double t) {
    const int64_t ts = static_cast<int64_t>(t / av_q2d(st->time_base));
    av_seek_frame(fmt, as, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec);
    swr_make();  // drop resampler history from the old position
    dropUntil = t;
    eof = false;
    drained = false;
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
    av_seek_frame(fmt, as, static_cast<int64_t>(chunkStart / av_q2d(st->time_base)),
                  AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec);
    swr_make();
    acc.clear();
    double accStart = -1.0;
    bool past = false, drainSent = false;
    while (!past) {
      if (wantClose.load() || seekReq.load() >= 0.0) return;  // preempted
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
      const uint32_t bytes = static_cast<uint32_t>(got) * outCh * sizeof(float);
      if (!write_chunk_header(ft, bytes) ||
          std::fwrite(outBuf.data(), 1, bytes, stdout) != bytes) {
        wantClose.store(true);
        break;
      }
      std::fflush(stdout);  // backpressure paces the decode
      if (wantClose.load() || seekReq.load() >= 0.0) break;
    }
  };

  std::vector<float> accBuf, revBuf;  // reverse-window scratch (reused)
  if (start_sec > 0.0) do_seek(start_sec);  // stream BEGINS here — no marker

  while (!wantClose.load()) {
    const double sk = seekReq.exchange(-1.0);
    if (sk >= 0.0) {
      dirState = dirReq.load();
      if (dirState < 0) {
        revHead = sk;
        eof = false;
      } else {
        do_seek(sk);
      }
      // Marker first, even while paused: the daemon flushes its ring buffer
      // and rebases position at the marker, so a paused seek lands instantly.
      if (!write_chunk_header(sk, 0)) break;
      std::fflush(stdout);
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
    if (av_read_frame(fmt, pkt) < 0) {
      // Drain decoder-buffered tail samples before idling (see video twin).
      if (!drained) {
        drained = true;
        avcodec_send_packet(dec, nullptr);
        pump_decoded();
      }
      eof = true;
      continue;
    }
    if (pkt->stream_index == as && avcodec_send_packet(dec, pkt) >= 0) {
      pump_decoded();
    }
    av_packet_unref(pkt);
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
int decode_server(const std::string&, int, double) {
  std::fprintf(stderr, "decode-server: libav not built in (LGPL ffmpeg dev package missing)\n");
  return 1;
}
int decode_server_audio(const std::string&, double) {
  std::fprintf(stderr, "decode-server: libav not built in (LGPL ffmpeg dev package missing)\n");
  return 1;
}
}  // namespace lathe
#endif
