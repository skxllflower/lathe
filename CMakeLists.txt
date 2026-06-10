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
)
target_include_directories(lathe PRIVATE src)

# --- libav (LGPL shared) for the linked decoder (Phase 3, decode-only). ---
# DYNAMIC link only: lathe stays closed-source (LGPL permits this), the DLLs
# ship next to the exe and stay user-swappable, and we use an LGPL build (no
# --enable-gpl / x264) so nothing forces lathe open. The dev package (headers +
# import libs + DLLs) is vendored under third_party/ (gitignored — fetch the
# BtbN ffmpeg-master-latest-win64-lgpl-shared release). If absent, lathe builds
# without the decoder (LATHE_HAVE_LIBAV undefined) and the pipe path still works.
file(GLOB _ffmpeg_roots "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/*lgpl-shared*")
list(GET _ffmpeg_roots 0 FFMPEG_DIR)
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
  file(GLOB _ffmpeg_dlls "${FFMPEG_DIR}/bin/*.dll")
  add_custom_command(TARGET lathe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_ffmpeg_dlls} "$<TARGET_FILE_DIR:lathe>"
    COMMENT "lathe: copying LGPL ffmpeg DLLs next to lathe.exe")
else()
  message(STATUS "lathe: LGPL libav dev package not found under third_party/ — building without the linked decoder")
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
