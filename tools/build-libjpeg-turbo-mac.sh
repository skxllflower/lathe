#!/usr/bin/env bash
# Builds the pinned libjpeg-turbo that LibRaw's DNG lossy codec links on mac, as a static library
# targeting MACOS_MIN. Homebrew's jpeg-turbo used to be picked up instead: its bottle targets the
# build host's OS, so the bundled libjpeg.8.dylib made dyld refuse the whole core on older macOS.
# License: IJG + BSD-3-Clause + zlib (LICENSE.md, copied into the prefix for the release script).
set -euo pipefail

export PATH="/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"

VERSION="${LIBJPEG_TURBO_VERSION:-3.2.0}"
SHA256="${LIBJPEG_TURBO_SHA256:-6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e}"
PREFIX="${LIBJPEG_TURBO_MAC:-$HOME/Dev/libjpeg-turbo-mac}"
WORK="${LIBJPEG_TURBO_BUILD_DIR:-$HOME/Dev/libjpeg-turbo-build}"
MACOS_MIN="${MACOS_MIN:-12.0}"
# The slices: the build host's own by default; "arm64 x86_64" for a universal libjpeg.a (the native Lathe.app,
# tools/build-release-native-mac.sh), each built on its own and lipo'd together. x86_64's SIMD needs nasm: without it
# that slice builds WITH_SIMD=OFF (C only) and says so.
MAC_ARCHS="${MAC_ARCHS:-$(uname -m)}"
ARCHIVE="$WORK/libjpeg-turbo-$VERSION.tar.gz"
SOURCE="$WORK/libjpeg-turbo-$VERSION"
URL="https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$VERSION/libjpeg-turbo-$VERSION.tar.gz"

mkdir -p "$WORK"
if [ ! -f "$ARCHIVE" ]; then
  curl -L --fail --proto '=https' --tlsv1.2 "$URL" -o "$ARCHIVE"
fi

ACTUAL_SHA256="$(shasum -a 256 "$ARCHIVE" | awk '{print $1}')"
if [ "$ACTUAL_SHA256" != "$SHA256" ]; then
  echo "ERROR: libjpeg-turbo source checksum mismatch." >&2
  echo "  expected: $SHA256" >&2
  echo "  actual:   $ACTUAL_SHA256" >&2
  exit 1
fi

rm -rf "$SOURCE" "$WORK"/build "$WORK"/build-*
tar -xzf "$ARCHIVE" -C "$WORK"

rm -rf "$PREFIX"
FIRST=""
for arch in $MAC_ARCHS; do
  BUILD_DIR="$WORK/build-$arch"
  ARCH_FLAGS=("-DCMAKE_OSX_ARCHITECTURES=$arch")
  # (another slice than the host's: libjpeg-turbo picks its SIMD sources from CMAKE_SYSTEM_PROCESSOR, the host's otherwise)
  if [ "$arch" != "$(uname -m)" ]; then ARCH_FLAGS+=(-DCMAKE_SYSTEM_NAME=Darwin "-DCMAKE_SYSTEM_PROCESSOR=$arch"); fi
  if [ "$arch" = "x86_64" ] && ! command -v nasm >/dev/null 2>&1; then
    echo "WARNING: nasm not found: the x86_64 slice builds without SIMD (brew install nasm, then rebuild)" >&2
    ARCH_FLAGS+=(-DWITH_SIMD=OFF)
  fi
  DEST="$PREFIX"
  [ -z "$FIRST" ] || DEST="$WORK/stage-$arch"
  rm -rf "$DEST"
  cmake -S "$SOURCE" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$DEST" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_MIN" \
    "${ARCH_FLAGS[@]}" \
    -DENABLE_SHARED=OFF \
    -DENABLE_STATIC=ON \
    -DWITH_JPEG8=ON \
    -DWITH_TURBOJPEG=OFF \
    -DWITH_TOOLS=OFF \
    -DWITH_TESTS=OFF
  cmake --build "$BUILD_DIR" -j"$(sysctl -n hw.ncpu)"
  cmake --install "$BUILD_DIR"
  if [ -z "$FIRST" ]; then
    FIRST="$arch"
  else
    lipo -create "$PREFIX/lib/libjpeg.a" "$DEST/lib/libjpeg.a" -output "$PREFIX/lib/libjpeg.a.universal"
    mv -f "$PREFIX/lib/libjpeg.a.universal" "$PREFIX/lib/libjpeg.a"
    rm -rf "$DEST"
  fi
done
cp -f "$SOURCE/LICENSE.md" "$PREFIX/LICENSE-libjpeg-turbo.md"

[ -f "$PREFIX/lib/libjpeg.a" ] || { echo "ERROR: $PREFIX/lib/libjpeg.a not installed" >&2; exit 1; }
BAD="$(cd "$WORK" && find build-* -name '*.o' -print0 | xargs -0 otool -l 2>/dev/null \
  | awk '/LC_BUILD_VERSION/{b=1} b&&/minos/{print $2; b=0}' | sort -u | grep -vx "$MACOS_MIN" || true)"
if [ -n "$BAD" ]; then
  echo "ERROR: libjpeg-turbo objects target macOS $BAD, expected $MACOS_MIN." >&2
  exit 1
fi

for arch in $MAC_ARCHS; do
  lipo "$PREFIX/lib/libjpeg.a" -verify_arch "$arch" || { echo "ERROR: libjpeg.a has no $arch slice." >&2; exit 1; }
done

echo "libjpeg-turbo $VERSION ($MAC_ARCHS, static, macOS $MACOS_MIN+) installed at $PREFIX"
