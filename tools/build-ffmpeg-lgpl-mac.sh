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
# The shipped dylibs' macOS floor. Unset, clang targets the build host's OS and dyld refuses them on
# anything older (WAVdesk loads them on macOS 12+).
MACOS_MIN="${MACOS_MIN:-12.0}"
export MACOSX_DEPLOYMENT_TARGET="$MACOS_MIN"
# The slices to build: the build host's own by default (the Tauri release); "arm64 x86_64" for a universal prefix (the
# native Lathe.app, tools/build-release-native-mac.sh). Each slice is configured and installed on its own (out of tree,
# DESTDIR-staged so every .pc file names $PREFIX), then the dylibs are lipo'd together. x86_64's hand-written assembly
# needs nasm: without it that slice builds --disable-x86asm (C only, slower) and says so.
MAC_ARCHS="${MAC_ARCHS:-$(uname -m)}"

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
  "--extra-cflags=-mmacosx-version-min=$MACOS_MIN"
  "--extra-ldflags=-mmacosx-version-min=$MACOS_MIN"
)

# (an in-tree configure from an earlier single-slice build would block the out-of-tree ones)
(cd "$SOURCE" && make distclean >/dev/null 2>&1 || true)
rm -rf "$PREFIX"
FIRST=""
SLICE_FLAGS=()
for arch in $MAC_ARCHS; do
  ARCH_FLAGS=("--arch=$arch" "--cc=clang -arch $arch")
  if [ "$arch" != "$(uname -m)" ]; then ARCH_FLAGS+=(--enable-cross-compile --target-os=darwin); fi
  if [ "$arch" = "x86_64" ] && ! command -v nasm >/dev/null 2>&1; then
    echo "WARNING: nasm not found: the x86_64 slice builds without its assembly (brew install nasm, then rebuild)" >&2
    ARCH_FLAGS+=(--disable-x86asm)
  fi
  BUILD_DIR="$WORK/build-$arch"
  STAGE="$WORK/stage-$arch"
  rm -rf "$BUILD_DIR" "$STAGE"
  mkdir -p "$BUILD_DIR"
  (
    cd "$BUILD_DIR"
    "$SOURCE/configure" "${CONFIGURE_FLAGS[@]}" "${ARCH_FLAGS[@]}"
    make -j"$(sysctl -n hw.ncpu)"
    make install DESTDIR="$STAGE"
  )
  if [ -z "$FIRST" ]; then
    FIRST="$arch"
    mkdir -p "$(dirname "$PREFIX")"
    cp -R "$STAGE$PREFIX" "$PREFIX"
  else
    # every real dylib (not the version symlinks) made universal in place
    for lib in "$PREFIX"/lib/*.dylib; do
      [ -L "$lib" ] && continue
      lipo -create "$lib" "$STAGE$PREFIX/lib/$(basename "$lib")" -output "$lib.universal"
      mv -f "$lib.universal" "$lib"
    done
  fi
  rm -rf "$STAGE"
  SLICE_FLAGS+=("# $arch: ${ARCH_FLAGS[*]}")
done

cp -f "$ARCHIVE" "$PREFIX/ffmpeg-$VERSION-source.tar.xz"
printf '%s\n' "${CONFIGURE_FLAGS[@]}" "${SLICE_FLAGS[@]}" > "$PREFIX/lathe-ffmpeg-configure.txt"
cp -f "$SOURCE/COPYING.LGPLv2.1" "$PREFIX/"

if grep -Rqs -- '--enable-gpl\|--enable-nonfree' "$PREFIX/lib/pkgconfig"; then
  echo "ERROR: forbidden GPL/nonfree configuration found in installed metadata." >&2
  exit 1
fi

BUILT_MIN="$(otool -l "$PREFIX/lib/libavutil.dylib" | awk '/LC_BUILD_VERSION/{b=1} b&&/minos/{print $2; exit}')"
if [ "$BUILT_MIN" != "$MACOS_MIN" ]; then
  echo "ERROR: libavutil targets macOS $BUILT_MIN, expected $MACOS_MIN." >&2
  exit 1
fi

for arch in $MAC_ARCHS; do
  lipo "$PREFIX/lib/libavutil.dylib" -verify_arch "$arch" || { echo "ERROR: libavutil has no $arch slice." >&2; exit 1; }
done

echo "LGPL FFmpeg $VERSION ($MAC_ARCHS, macOS $MACOS_MIN+) installed at $PREFIX"
