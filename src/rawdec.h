#pragma once

#include <filesystem>
#include <string>

namespace lathe {

// True when `ext` (lowercase, no dot) is a camera-RAW still format.
bool is_camera_raw_ext(const std::string& ext);

// Whether this build carries the LibRaw decoder.
bool raw_decoder_available();

// Demosaic a camera RAW into a 16-bit binary PPM (P6, maxval 65535) at
// `ppm_out` — an intermediate ffmpeg ingests losslessly, so the normal
// encode path with all its options runs unchanged afterwards. Camera
// white balance + sRGB gamma, full resolution. Returns false and fills
// *error on failure (including builds without LibRaw).
bool raw_decode_to_ppm(const std::string& input,
                       const std::filesystem::path& ppm_out,
                       std::string* error);

}
