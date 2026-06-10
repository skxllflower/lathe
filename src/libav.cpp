#include "libav.h"

#include <cstdio>

#ifdef LATHE_HAVE_LIBAV
extern "C" {
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace lathe {

std::string libav_versions() {
  char buf[256];
  std::string s = "libav linked OK\n";
  std::snprintf(buf, sizeof(buf), "  version_info : %s\n", av_version_info());
  s += buf;
  const unsigned c = avcodec_version(), f = avformat_version(), u = avutil_version();
  std::snprintf(buf, sizeof(buf), "  avcodec      : %u.%u.%u\n",
                AV_VERSION_MAJOR(c), AV_VERSION_MINOR(c), AV_VERSION_MICRO(c));
  s += buf;
  std::snprintf(buf, sizeof(buf), "  avformat     : %u.%u.%u\n",
                AV_VERSION_MAJOR(f), AV_VERSION_MINOR(f), AV_VERSION_MICRO(f));
  s += buf;
  std::snprintf(buf, sizeof(buf), "  avutil       : %u.%u.%u\n",
                AV_VERSION_MAJOR(u), AV_VERSION_MINOR(u), AV_VERSION_MICRO(u));
  s += buf;
  // The linked decoder MUST be LGPL (not GPL/nonfree) or it would force lathe
  // open-source. Confirm from the build's own configure string.
  const std::string cfg = avcodec_configuration();
  const bool tainted = cfg.find("--enable-gpl") != std::string::npos ||
                       cfg.find("--enable-nonfree") != std::string::npos;
  s += "  license      : ";
  s += tainted ? "GPL/nonfree — PROBLEM, do not ship linked" : "LGPL (ok to link, lathe stays closed)";
  s += "\n";
  return s;
}

}  // namespace lathe
#else
namespace lathe {
std::string libav_versions() {
  return "libav not built in (LGPL ffmpeg dev package missing; see third_party/)\n";
}
}  // namespace lathe
#endif
