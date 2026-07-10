cmake_minimum_required(VERSION 3.20)
project(lathe LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(lathe
  src/main.cpp
  src/bootstrap.cpp
  src/convert.cpp
  src/decode.cpp
  src/download.cpp
  src/libav.cpp
  src/paths.cpp
  src/process.cpp
  src/progress.cpp
  src/rawdec.cpp
)
target_include_directories(lathe PRIVATE src)

# --- libav (LGPL shared) for the linked decoder (Phase 3, decode-only). ---
# DYNAMIC link only: lathe stays closed-source (LGPL permits this), the DLLs
# ship next to the exe and stay user-swappable, and we use an LGPL build (no
# --enable-gpl / x264) so nothing forces lathe open. The dev package (headers +
# import libs + DLLs) is vendored under third_party/ (gitignored). On a clean
# checkout / CI, configure with -DLATHE_FETCH_FFMPEG=ON to download it
# automatically (BtbN ffmpeg-master-latest-win64-lgpl-shared, ~100 MB once).
# If absent, lathe builds without the decoder (LATHE_HAVE_LIBAV undefined) and
# the pipe path still works.
option(LATHE_FETCH_FFMPEG "Download the LGPL ffmpeg dev package at configure time when missing" OFF)

set(_ffmpeg_base "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg")
file(GLOB _ffmpeg_roots "${_ffmpeg_base}/*lgpl-shared*")
if (NOT _ffmpeg_roots AND LATHE_FETCH_FFMPEG AND WIN32)
  set(_ffmpeg_zip "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg-lgpl-shared.zip")
  message(STATUS "lathe: fetching the LGPL ffmpeg dev package (BtbN latest, ~100 MB)…")
  file(DOWNLOAD
    "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-lgpl-shared.zip"
    "${_ffmpeg_zip}"
    STATUS _ffmpeg_dl_status SHOW_PROGRESS)
  list(GET _ffmpeg_dl_status 0 _ffmpeg_dl_code)
  if (_ffmpeg_dl_code EQUAL 0)
    file(MAKE_DIRECTORY "${_ffmpeg_base}")
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E tar xf "${_ffmpeg_zip}"
      WORKING_DIRECTORY "${_ffmpeg_base}"
      RESULT_VARIABLE _ffmpeg_untar_code)
    file(REMOVE "${_ffmpeg_zip}")
    if (NOT _ffmpeg_untar_code EQUAL 0)
      message(WARNING "lathe: ffmpeg dev package extraction failed — building without the linked decoder")
    endif()
    file(GLOB _ffmpeg_roots "${_ffmpeg_base}/*lgpl-shared*")
  else()
    message(WARNING "lathe: ffmpeg dev package download failed — building without the linked decoder")
  endif()
endif()

if (_ffmpeg_roots)
  list(GET _ffmpeg_roots 0 FFMPEG_DIR)
endif()
if (FFMPEG_DIR AND EXISTS "${FFMPEG_DIR}/include")
  message(STATUS "lathe: linking LGPL libav from ${FFMPEG_DIR}")
  target_include_directories(lathe PRIVATE "${FFMPEG_DIR}/include")
  target_link_libraries(lathe PRIVATE
    "${FFMPEG_DIR}/lib/avformat.lib"
    "${FFMPEG_DIR}/lib/avcodec.lib"
    "${FFMPEG_DIR}/lib/avutil.lib"
    "${FFMPEG_DIR}/lib/swscale.lib"
    "${FFMPEG_DIR}/lib/swresample.lib"
  )
  target_compile_definitions(lathe PRIVATE LATHE_HAVE_LIBAV=1)
  # The DLLs and the LGPL notice travel TOGETHER with the exe: any packaging
  # that grabs the output directory ships compliant (dynamic link, swappable
  # libraries, written offer of source in the notice).
  file(GLOB _ffmpeg_dlls "${FFMPEG_DIR}/bin/*.dll")
  add_custom_command(TARGET lathe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_ffmpeg_dlls}
            "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.txt"
            "$<TARGET_FILE_DIR:lathe>"
    COMMENT "lathe: copying LGPL ffmpeg DLLs + notice next to lathe.exe")
elseif (APPLE)
  # --- macOS: link the system/Homebrew libav via pkg-config. -----------------
  # There is no win64 lgpl-shared dev package to unzip here; instead the port
  # links whatever ffmpeg the developer installed (Homebrew's `ffmpeg` ships the
  # dev headers + .pc files). This is APPLE-only and never touches the Windows
  # path above, so the Windows build stays byte-identical. If pkg-config or the
  # libav modules are absent we fall through to the same stub build as before
  # (LATHE_HAVE_LIBAV undefined -> decode.cpp compiles to stubs).
  find_package(PkgConfig QUIET)
  if (PkgConfig_FOUND)
    pkg_check_modules(LIBAV
      libavformat libavcodec libavutil libswscale libswresample)
    if (LIBAV_FOUND)
      message(STATUS "lathe: linking libav via pkg-config (avformat ${LIBAV_libavformat_VERSION})")
      # _LINK_LIBRARIES carries absolute library paths; _INCLUDE_DIRS the headers.
      target_include_directories(lathe PRIVATE ${LIBAV_INCLUDE_DIRS})
      target_link_directories(lathe PRIVATE ${LIBAV_LIBRARY_DIRS})
      target_link_libraries(lathe PRIVATE ${LIBAV_LINK_LIBRARIES})
      target_compile_definitions(lathe PRIVATE LATHE_HAVE_LIBAV=1)
    else()
      message(WARNING "lathe: libav modules not found via pkg-config — building without the linked decoder (install Homebrew ffmpeg to enable it)")
    endif()
  else()
    message(WARNING "lathe: pkg-config not found — building without the linked decoder (install pkg-config + Homebrew ffmpeg to enable it)")
  endif()
else()
  message(STATUS "lathe: LGPL libav dev package not found under third_party/ — building without the linked decoder (configure with -DLATHE_FETCH_FFMPEG=ON to fetch it)")
endif()

# --- LibRaw (camera-RAW decoder). Dual-licensed CDDL-1.0 / LGPL-2.1; used
# unmodified under CDDL, statically linked (CDDL is file-scoped copyleft, so
# the combination stays closed). Notice ships in THIRD_PARTY_NOTICES.txt.
# Sources fetch at configure time (LibRaw upstream has no CMake build; the
# LibRaw-cmake companion project provides it). -DLATHE_WITH_LIBRAW=OFF builds
# without RAW support; rawdec.cpp then compiles to a stub.
option(LATHE_WITH_LIBRAW "Build the LibRaw camera-RAW decoder" ON)
if (LATHE_WITH_LIBRAW)
  include(FetchContent)
  FetchContent_Declare(libraw_src
    URL https://github.com/LibRaw/LibRaw/archive/refs/tags/0.21.4.zip)
  FetchContent_Declare(libraw_cmake
    URL https://github.com/LibRaw/LibRaw-cmake/archive/refs/heads/master.zip)
  FetchContent_MakeAvailable(libraw_src)
  set(LIBRAW_PATH "${libraw_src_SOURCE_DIR}" CACHE PATH "LibRaw source dir for LibRaw-cmake" FORCE)
  set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(ENABLE_OPENMP   OFF CACHE BOOL "" FORCE)
  set(ENABLE_JASPER   OFF CACHE BOOL "" FORCE)
  set(ENABLE_LCMS     OFF CACHE BOOL "" FORCE)
  # Cache-forced: LibRaw-cmake's own option() would override a plain
  # variable under the pre-CMP0077 behavior its minimum version selects.
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(libraw_cmake)
  target_link_libraries(lathe PRIVATE libraw::libraw)
  target_compile_definitions(lathe PRIVATE LATHE_HAVE_LIBRAW=1)
endif()

if (MSVC)
  target_compile_options(lathe PRIVATE /W3 /utf-8)
  target_compile_definitions(lathe PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  target_compile_options(lathe PRIVATE -Wall -Wextra)
endif()

# ffmpeg.exe is no longer copied next to the build output — it lives in
# the Vacant Systems shared bin (see src/paths.cpp) and a next-to-exe copy
# would permanently win the portable-override tier of the resolution order.
