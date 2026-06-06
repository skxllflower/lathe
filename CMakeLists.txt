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
  src/paths.cpp
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

# ffmpeg.exe is no longer copied next to the build output — it lives in
# the Vacant Systems shared bin (see src/paths.cpp) and a next-to-exe copy
# would permanently win the portable-override tier of the resolution order.
