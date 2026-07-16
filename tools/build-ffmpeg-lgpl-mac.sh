#!/usr/bin/env bash
set -euo pipefail

export PATH="/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"

VERSION="${FFMPEG_VERSION:-8.1.2}"
SHA256="${FFMPEG_SHA256:-464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c}"
PREFIX="${FFMPEG_LGPL:-$HOME/Dev/ffmpeg-lgpl-mac}"
WORK="${FFMPEG_BUILD_DIR:-$HOME/Dev/ffmpeg-lgpl-build}"
ARCHIVE="$WORK/ffmpeg-$VERSION.tar.xz"
SOURCE="$WORK/ffmpeg-$VERSION"
URL="https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz"

mkdir -p "$WORK"
if [ ! -f "$ARCHIVE" ]; then
  curl -L --fail --proto '=https' --tlsv1.2 "$URL" -o "$ARCHIVE"
fi

ACTUAL_SHA256="$(shasum -a 256 "$ARCHIVE" | awk '{print $1}')"
if [ "$ACTUAL_SHA256" != "$SHA256" ]; then
  echo "ERROR: FFmpeg source checksum mismatch." >&2
  echo "  expected: $SHA256" >&2
  echo "  actual:   $ACTUAL_SHA256" >&2
  exit 1
fi

if [ ! -d "$SOURCE" ]; then
  tar -xf "$ARCHIVE" -C "$WORK"
fi

CONFIGURE_FLAGS=(
  "--prefix=$PREFIX"
  --disable-gpl
  --disable-nonfree
  --disable-version3
  --enable-shared
  --disable-static
  --disable-programs
  --disable-doc
  --disable-debug
  --disable-avdevice
  --disable-avfilter
  --enable-videotoolbox
  --enable-audiotoolbox
  --enable-securetransport
)

rm -rf "$PREFIX"
mkdir -p "$PREFIX"
(
  cd "$SOURCE"
  make distclean >/dev/null 2>&1 || true
  ./configure "${CONFIGURE_FLAGS[@]}"
  make -j"$(sysctl -n hw.ncpu)"
  make install
)

cp -f "$ARCHIVE" "$PREFIX/ffmpeg-$VERSION-source.tar.xz"
printf '%s\n' "${CONFIGURE_FLAGS[@]}" > "$PREFIX/lathe-ffmpeg-configure.txt"
cp -f "$SOURCE/COPYING.LGPLv2.1" "$PREFIX/"

if grep -Rqs -- '--enable-gpl\|--enable-nonfree' "$PREFIX/lib/pkgconfig"; then
  echo "ERROR: forbidden GPL/nonfree configuration found in installed metadata." >&2
  exit 1
fi

echo "LGPL FFmpeg $VERSION installed at $PREFIX"
