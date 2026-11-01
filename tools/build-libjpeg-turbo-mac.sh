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

rm -rf "$SOURCE" "$WORK/build"
tar -xzf "$ARCHIVE" -C "$WORK"

rm -rf "$PREFIX"
cmake -S "$SOURCE" -B "$WORK/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_MIN" \
  -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" \
  -DENABLE_SHARED=OFF \
  -DENABLE_STATIC=ON \
  -DWITH_JPEG8=ON \
  -DWITH_TURBOJPEG=OFF \
  -DWITH_TOOLS=OFF \
  -DWITH_TESTS=OFF
cmake --build "$WORK/build" -j"$(sysctl -n hw.ncpu)"
cmake --install "$WORK/build"
cp -f "$SOURCE/LICENSE.md" "$PREFIX/LICENSE-libjpeg-turbo.md"

[ -f "$PREFIX/lib/libjpeg.a" ] || { echo "ERROR: $PREFIX/lib/libjpeg.a not installed" >&2; exit 1; }
BAD="$(cd "$WORK/build" && find . -name '*.o' -print0 | xargs -0 otool -l 2>/dev/null \
  | awk '/LC_BUILD_VERSION/{b=1} b&&/minos/{print $2; b=0}' | sort -u | grep -vx "$MACOS_MIN" || true)"
if [ -n "$BAD" ]; then
  echo "ERROR: libjpeg-turbo objects target macOS $BAD, expected $MACOS_MIN." >&2
  exit 1
fi

echo "libjpeg-turbo $VERSION (static, macOS $MACOS_MIN+) installed at $PREFIX"
