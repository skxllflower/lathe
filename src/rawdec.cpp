#include "rawdec.h"

#include "process.h"

#include <cstdint>
#include <fstream>
#include <vector>

#ifdef LATHE_HAVE_LIBRAW
  #include <libraw/libraw.h>
#endif

namespace lathe {

namespace fs = std::filesystem;

bool is_camera_raw_ext(const std::string& ext) {
  static const char* kExts[] = {
    "cr2", "cr3", "crw",          // Canon
    "nef", "nrw",                 // Nikon
    "arw", "srf", "sr2",          // Sony
    "dng",                        // Adobe / generic
    "raf",                        // Fujifilm
    "orf",                        // Olympus
    "rw2",                        // Panasonic
    "pef",                        // Pentax
    "srw",                        // Samsung
    "dcr", "kdc",                 // Kodak
    "erf",                        // Epson
    "mef",                        // Mamiya
    "mrw",                        // Minolta
    "mos",                        // Leaf
    "x3f",                        // Sigma
    "3fr", "fff",                 // Hasselblad
    "iiq",                        // Phase One
    "rwl",                        // Leica
    "gpr",                        // GoPro
  };
  for (const char* e : kExts) {
    if (ext == e) return true;
  }
  return false;
}

bool raw_decoder_available() {
#ifdef LATHE_HAVE_LIBRAW
  return true;
#else
  return false;
#endif
}

#ifdef LATHE_HAVE_LIBRAW

bool raw_decode_to_ppm(const std::string& input,
                       const fs::path& ppm_out,
                       std::string* error) {
  // Read the file ourselves (wide-path safe) and decode from memory —
  // LibRaw's open_file goes through fopen, which mangles non-ANSI paths
  // on Windows.
  std::vector<char> buf;
  {
#ifdef _WIN32
    fs::path in = fs::path(utf8_to_utf16(input));
#else
    fs::path in = fs::path(input);
#endif
    std::ifstream f(in, std::ios::binary | std::ios::ate);
    if (!f) { *error = "cannot read input file"; return false; }
    auto sz = f.tellg();
    if (sz <= 0) { *error = "input file is empty"; return false; }
    buf.resize(static_cast<size_t>(sz));
    f.seekg(0);
    if (!f.read(buf.data(), static_cast<std::streamsize>(buf.size()))) {
      *error = "short read on input file";
      return false;
    }
  }

  LibRaw lr;
  lr.imgdata.params.use_camera_wb = 1;   // shot-time white balance
  lr.imgdata.params.output_bps   = 16;   // full depth into the encoder
  // Defaults beyond that: sRGB output space + gamma, AHD-family demosaic,
  // auto-brighten — dcraw-compatible, what users expect from "open a RAW".

  int rc = lr.open_buffer(buf.data(), buf.size());
  if (rc == LIBRAW_SUCCESS) rc = lr.unpack();
  if (rc == LIBRAW_SUCCESS) rc = lr.dcraw_process();
  libraw_processed_image_t* img = nullptr;
  if (rc == LIBRAW_SUCCESS) img = lr.dcraw_make_mem_image(&rc);
  if (!img || rc != LIBRAW_SUCCESS) {
    *error = libraw_strerror(rc);
    if (img) LibRaw::dcraw_clear_mem(img);
    return false;
  }
  if (img->type != LIBRAW_IMAGE_BITMAP || img->colors != 3 || img->bits != 16) {
    *error = "unexpected LibRaw image layout";
    LibRaw::dcraw_clear_mem(img);
    return false;
  }

  // 16-bit P6 PPM: maxval > 255 means big-endian samples per spec; LibRaw
  // hands back native little-endian, so swap as we write.
  std::ofstream out(ppm_out, std::ios::binary | std::ios::trunc);
  if (!out) {
    *error = "cannot write RAW intermediate";
    LibRaw::dcraw_clear_mem(img);
    return false;
  }
  out << "P6\n" << img->width << " " << img->height << "\n65535\n";
  const auto* px = reinterpret_cast<const std::uint16_t*>(img->data);
  const size_t n = static_cast<size_t>(img->width) * img->height * 3;
  std::vector<std::uint8_t> swapped(n * 2);
  for (size_t i = 0; i < n; ++i) {
    swapped[i * 2]     = static_cast<std::uint8_t>(px[i] >> 8);
    swapped[i * 2 + 1] = static_cast<std::uint8_t>(px[i] & 0xff);
  }
  out.write(reinterpret_cast<const char*>(swapped.data()),
            static_cast<std::streamsize>(swapped.size()));
  const bool ok = out.good();
  out.close();
  LibRaw::dcraw_clear_mem(img);
  if (!ok) {
    *error = "short write on RAW intermediate";
    std::error_code ec;
    fs::remove(ppm_out, ec);
    return false;
  }
  return true;
}

#else

bool raw_decode_to_ppm(const std::string&,
                       const fs::path&,
                       std::string* error) {
  *error = "this lathe build has no LibRaw decoder";
  return false;
}

#endif

}
