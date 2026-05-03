#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace lathe {

// HTTP(S) GET → file, with progress callbacks. Returns true on success.
//
// Progress callback fires every ~150ms during the transfer, with the
// final tally invoked once at the end (so the UI reaches 100%).
// `total` is 0 if the server didn't send Content-Length (Transfer-
// Encoding: chunked, etc.) — UI can fall back to indeterminate.
//
// Follows redirects automatically (cross-host included — required for
// GitHub releases → S3 / CDN). Uses WinHTTP on Windows; POSIX builds
// stub-fail for now.
bool download_with_progress(
  const std::string& url,
  const std::filesystem::path& dest,
  const std::function<void(uint64_t bytes, uint64_t total)>& on_progress);

}
