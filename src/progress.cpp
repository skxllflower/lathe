#include "progress.h"

#include <cstdio>
#include <string>

namespace lathe {

namespace {

std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

void emit(const std::string& json) {
  std::fputs(json.c_str(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

}

void progress_start(const std::string& input,
                    const std::string& output,
                    double duration_s) {
  char buf[2048];
  std::snprintf(buf, sizeof(buf),
    "{\"type\":\"start\",\"input\":\"%s\",\"output\":\"%s\",\"duration_s\":%.3f}",
    json_escape(input).c_str(),
    json_escape(output).c_str(),
    duration_s);
  emit(buf);
}

void progress_update(double percent, double time_s, const std::string& speed) {
  char buf[512];
  std::snprintf(buf, sizeof(buf),
    "{\"type\":\"progress\",\"percent\":%.2f,\"time_s\":%.3f,\"speed\":\"%s\"}",
    percent, time_s, json_escape(speed).c_str());
  emit(buf);
}

void progress_done(const std::string& output) {
  char buf[2048];
  std::snprintf(buf, sizeof(buf),
    "{\"type\":\"done\",\"output\":\"%s\"}",
    json_escape(output).c_str());
  emit(buf);
}

void progress_cancelled() {
  emit("{\"type\":\"cancelled\"}");
}

void progress_error(const std::string& message) {
  char buf[4096];
  std::snprintf(buf, sizeof(buf),
    "{\"type\":\"error\",\"message\":\"%s\"}",
    json_escape(message).c_str());
  emit(buf);
}

}
