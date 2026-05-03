#pragma once

#include <string>

namespace lathe {

enum class ConvertResult {
  Ok,
  InputNotFound,
  FfmpegFailed,
  Cancelled,
};

ConvertResult convert(const std::string& input, const std::string& output);

}
