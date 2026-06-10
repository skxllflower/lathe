#include "decode.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#ifdef LATHE_HAVE_LIBAV
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
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

}  // namespace lathe
#else
namespace lathe {
int decode_probe(const std::string&, int, double, int) {
  std::fprintf(stderr, "decode-probe: libav not built in (LGPL ffmpeg dev package missing)\n");
  return 1;
}
}  // namespace lathe
#endif
