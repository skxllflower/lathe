cmake_minimum_required(VERSION 3.20)
project(lathe LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(lathe
  src/main.cpp
  src/bootstrap.cpp
  src/convert.cpp
  src/download.cpp
  src/process.cpp
  src/progress.cpp
)
target_include_directories(lathe PRIVATE src)

if (MSVC)
  target_compile_options(lathe PRIVATE /W3 /utf-8)
  target_compile_definitions(lathe PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  target_compile_options(lathe PRIVATE -Wall -Wextra)
endif()

if (WIN32)
  set(LATHE_FFMPEG_BIN "${CMAKE_SOURCE_DIR}/binaries/ffmpeg.exe")
  if (EXISTS "${LATHE_FFMPEG_BIN}")
    add_custom_command(TARGET lathe POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${LATHE_FFMPEG_BIN}"
        "$<TARGET_FILE_DIR:lathe>/ffmpeg.exe"
    )
  else()
    message(WARNING "ffmpeg.exe not found at ${LATHE_FFMPEG_BIN}")
  endif()
endif()
