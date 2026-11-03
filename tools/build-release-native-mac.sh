#!/usr/bin/env bash
# build-release-native-mac.sh - the native Lathe on macOS (the vpf GUI in native/, replacing the Tauri bundle that
# tools/build-release-mac.sh makes): a universal Lathe.app and Vacant Systems Manager's package of it.
#
#   tools/build-release-native-mac.sh                  # full build: core + GUI, universal, signed, zipped
#   SKIP_CPP=1 tools/build-release-native-mac.sh       # reuse the core and GUI builds in native/build-release
#   tools/build-release-native-mac.sh --skip-notarize  # Developer ID signed, not notarized
#
# Steps:
#   1. the core CLI (cmake ., --target lathe, arm64 + x86_64) on the pinned LGPL libav and a static libjpeg-turbo, both
#      universal prefixes (tools/build-ffmpeg-lgpl-mac.sh and tools/build-libjpeg-turbo-mac.sh with MAC_ARCHS, built
#      here when missing), refused unless it reports an LGPL libav -> native/build-release/core/lathe
#   2. the core staged with its libav dylibs, each rewritten to @loader_path -> native/build-release/coredist
#   3. the GUI (native/, its own build tree, arm64 + x86_64) -> native/build-release/build/bin/Lathe.app, the staged
#      core and dylibs copied into Contents/MacOS/coredist (native/app/bundle_core.cmake), where the Tauri bundle had
#      them; the LGPL notices into Contents/Resources/licenses (non-code stays out of Contents/MacOS)
#   4. checks: every Mach-O universal, at or under the macOS floor (12.0), no Homebrew or build-prefix library linked
#   5. signed inside out (the dylibs, the core, then the app; never --deep)
#   6. with a Developer ID only: notarized and stapled
#   7. the manager's package (vacant-labs apps/manager/docs/app-contract.md, 8):
#      native/build-release/manager/Lathe-<version>-mac.zip holding Lathe.app/ (ditto -c -k --keepParent, no extended attributes)
#
# Signing modes (as tools/build-release-mac.sh and vacant-labs' tools/build-release-mac.sh):
#   - APPLE_SIGNING_IDENTITY unset: ad hoc. Runs on THIS mac only; the manager's `catalog.py pack` re-signs it inside out
#     with the Developer ID, notarizes and staples it (the contract's "ad hoc signed by its build").
#   - APPLE_SIGNING_IDENTITY="Developer ID Application: Name (TEAMID)": hardened runtime + secure timestamp on each
#     piece, then notarized (keychain profile NOTARY_PROFILE, default wavdesk-notary) and stapled.
#
# The GUI builds on the pinned vpf and WAVdesk commits (native/CMakeLists.txt GIT_TAG), never a local checkout's working
# tree. WAVDESK_SRC / VACANTLABS_SRC point FetchContent at a checkout instead (a pin not pushed yet): the script says so.
# The x86_64 slices of libav and libjpeg are built without their assembly when nasm is missing (C only, slower on an
# Intel Mac): brew install nasm and delete the two prefixes to rebuild them with it.
set -euo pipefail

SKIP_NOTARIZE="${SKIP_NOTARIZE:-0}"
for arg in "$@"; do
  case "$arg" in
    --skip-notarize) SKIP_NOTARIZE=1 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done

export PATH="/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$REPO/native/build-release"
CORE_BUILD="$OUT/core"
COREDIST="$OUT/coredist"
GUI_BUILD="$OUT/build"
MGR="$OUT/manager"
ARCHS="arm64;x86_64"
MAC_ARCHS="arm64 x86_64"
MACOS_MIN="12.0"
FFMPEG_LGPL="${FFMPEG_LGPL:-$HOME/Dev/ffmpeg-lgpl-mac-universal}"
LIBJPEG_TURBO_MAC="${LIBJPEG_TURBO_MAC:-$HOME/Dev/libjpeg-turbo-mac-universal}"
IDENTITY="${APPLE_SIGNING_IDENTITY:-}"
PROFILE="${NOTARY_PROFILE:-wavdesk-notary}"
VERSION="$(/usr/bin/python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$REPO/gui/src-tauri/tauri.conf.json")"

step() { printf '\n\033[36m[%s] %s\033[0m\n' "$1" "$2"; }
die() { echo "RELEASE ABORTED: $*" >&2; exit 1; }

# notarize <file>: submit, wait, and fail with Apple's log unless it is Accepted
notarize() {
  local file="$1" json id status
  json="$(xcrun notarytool submit "$file" --keychain-profile "$PROFILE" --wait --output-format json)" || die "notarytool submit failed for $file"
  id="$(printf '%s' "$json" | /usr/bin/python3 -c 'import json,sys; print(json.load(sys.stdin).get("id",""))')"
  status="$(printf '%s' "$json" | /usr/bin/python3 -c 'import json,sys; print(json.load(sys.stdin).get("status",""))')"
  echo "  notarization $id: $status"
  if [ "$status" != "Accepted" ]; then
    xcrun notarytool log "$id" --keychain-profile "$PROFILE" >&2 || true
    die "notarization of $(basename "$file") was $status"
  fi
}

# the minos of a Mach-O (LC_BUILD_VERSION, or the older LC_VERSION_MIN_MACOSX), its first slice's
minos_of() {
  otool -l "$1" | awk '
    /LC_BUILD_VERSION|LC_VERSION_MIN_MACOSX/ { b = 1 }
    b && $1 == "minos"   { print $2; exit }
    b && $1 == "version" { print $2; exit }'
}
version_le() { [ "$(printf '%s\n%s\n' "$1" "$2" | sort -t. -k1,1n -k2,2n -k3,3n | head -1)" = "$1" ]; }
# true when the file carries every slice in $MAC_ARCHS
universal() { local a; for a in $MAC_ARCHS; do lipo "$1" -verify_arch "$a" 2>/dev/null || return 1; done; }

# Every non-system dylib in the core's closure copied beside it, each reference (and each copied lib's id and its own
# references) rewritten to @loader_path: the shipped core names no build prefix (tools/build-release-mac.sh's, as is)
bundle_dylibs() {
  local main_bin="$1"
  local dest_dir; dest_dir="$(dirname "$main_bin")"
  local queue=("$main_bin")
  while [ ${#queue[@]} -gt 0 ]; do
    local f="${queue[0]}"
    queue=("${queue[@]:1}")
    local line dep base target
    while IFS= read -r line; do
      dep="$(printf '%s\n' "$line" | awk '{print $1}')"
      case "$dep" in
        /usr/local/*|/opt/homebrew/*)
          die "$(basename "$f") links $dep: a Homebrew library inherits the build host's macOS and its license is unvetted"
          ;;
        "$FFMPEG_LGPL"/*)
          base="$(basename "$dep")"
          target="$dest_dir/$base"
          if [ ! -e "$target" ]; then
            cp -L "$dep" "$target"
            chmod u+w "$target"
            install_name_tool -id "@loader_path/$base" "$target" 2>/dev/null || true
            queue+=("$target")
          fi
          install_name_tool -change "$dep" "@loader_path/$base" "$f" 2>/dev/null || true
          ;;
      esac
    done < <(otool -L "$f" | tail -n +2)
  done
}

if [ -n "$IDENTITY" ]; then
  security find-identity -v -p codesigning | grep -qF "$IDENTITY" || die "signing identity not in the keychain: $IDENTITY"
fi
pgrep -x Lathe >/dev/null 2>&1 && echo "warning: Lathe is running (quit it before installing this build)"

# ---- 1: the core ----
step 1 "the core CLI $VERSION ($ARCHS)"
if [ "${SKIP_CPP:-0}" = "1" ]; then
  echo "  SKIP_CPP=1: reusing $CORE_BUILD"
else
  if [ ! -f "$FFMPEG_LGPL/lib/pkgconfig/libavcodec.pc" ] || ! universal "$FFMPEG_LGPL/lib/libavutil.dylib" \
     || [ "$(minos_of "$FFMPEG_LGPL/lib/libavutil.dylib")" != "$MACOS_MIN" ]; then
    echo "  no universal LGPL FFmpeg for macOS $MACOS_MIN at $FFMPEG_LGPL: building it"
    MAC_ARCHS="$MAC_ARCHS" MACOS_MIN="$MACOS_MIN" FFMPEG_LGPL="$FFMPEG_LGPL" \
      FFMPEG_BUILD_DIR="${FFMPEG_BUILD_DIR:-$HOME/Dev/ffmpeg-lgpl-build-universal}" "$REPO/tools/build-ffmpeg-lgpl-mac.sh"
  fi
  if [ ! -f "$LIBJPEG_TURBO_MAC/lib/libjpeg.a" ] || ! universal "$LIBJPEG_TURBO_MAC/lib/libjpeg.a" || \
     ! otool -l "$LIBJPEG_TURBO_MAC/lib/libjpeg.a" | awk '$1 == "minos" { print $2 }' | sort -u | grep -qx "$MACOS_MIN"; then
    echo "  no universal libjpeg-turbo for macOS $MACOS_MIN at $LIBJPEG_TURBO_MAC: building it"
    MAC_ARCHS="$MAC_ARCHS" MACOS_MIN="$MACOS_MIN" LIBJPEG_TURBO_MAC="$LIBJPEG_TURBO_MAC" \
      LIBJPEG_TURBO_BUILD_DIR="${LIBJPEG_TURBO_BUILD_DIR:-$HOME/Dev/libjpeg-turbo-build-universal}" "$REPO/tools/build-libjpeg-turbo-mac.sh"
  fi
  echo "  LGPL libav from $FFMPEG_LGPL, static libjpeg-turbo from $LIBJPEG_TURBO_MAC"
  PKG_CONFIG_LIBDIR="$FFMPEG_LGPL/lib/pkgconfig" cmake -S "$REPO" -B "$CORE_BUILD" -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_OSX_ARCHITECTURES=$ARCHS" "-DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOS_MIN" \
    "-DJPEG_INCLUDE_DIR=$LIBJPEG_TURBO_MAC/include" \
    "-DJPEG_LIBRARY_RELEASE=$LIBJPEG_TURBO_MAC/lib/libjpeg.a" \
    "-DJPEG_LIBRARY=$LIBJPEG_TURBO_MAC/lib/libjpeg.a" >/dev/null
  PKG_CONFIG_LIBDIR="$FFMPEG_LGPL/lib/pkgconfig" cmake --build "$CORE_BUILD" --target lathe -j "$(sysctl -n hw.ncpu)"
fi
CORE="$CORE_BUILD/lathe"
[ -f "$CORE" ] || die "core binary not found: $CORE (run without SKIP_CPP?)"
# (the guard tools/build-release-mac.sh keeps: no decoder, or a GPL / nonfree one, never ships)
LIBAV_INFO="$("$CORE" libav-version 2>&1 || true)"
printf '%s\n' "$LIBAV_INFO" | sed 's/^/  /'
printf '%s' "$LIBAV_INFO" | grep -qi 'not built in' && die "the core was built WITHOUT libav (its decode-server would be a stub)"
printf '%s' "$LIBAV_INFO" | grep -qi 'GPL/nonfree\|do not ship\|enable-gpl\|enable-nonfree' && die "the core reports a GPL / nonfree libav"
printf '%s' "$LIBAV_INFO" | grep -qi 'LGPL' || die "the core did not report an LGPL libav"

# ---- 2: the core staged with its dylibs ----
step 2 "the core + its libav dylibs -> $COREDIST"
rm -rf "$COREDIST"
mkdir -p "$COREDIST"
cp -f "$CORE" "$COREDIST/lathe"
chmod 0755 "$COREDIST/lathe"
bundle_dylibs "$COREDIST/lathe"
if otool -L "$COREDIST/lathe" | tail -n +2 | grep -qE "$FFMPEG_LGPL|/usr/local/|/opt/homebrew/"; then
  die "the staged core still names a Homebrew or build-prefix library"
fi
echo "  lathe + $(find "$COREDIST" -name '*.dylib' | wc -l | tr -d ' ') dylib(s)"

# ---- 3: the GUI ----
step 3 "the GUI ($ARCHS)"
SRC_ARGS=()
if [ -n "${WAVDESK_SRC:-}" ]; then
  echo "  WAVDESK_SRC: WAVdesk from $WAVDESK_SRC, not the pinned commit"
  SRC_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_WAVDESK=$WAVDESK_SRC")
fi
if [ -n "${VACANTLABS_SRC:-}" ]; then
  echo "  VACANTLABS_SRC: vacant-labs from $VACANTLABS_SRC, not the pinned commit"
  SRC_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_VACANTLABS=$VACANTLABS_SRC")
fi
if [ "${SKIP_CPP:-0}" != "1" ] || [ ! -d "$GUI_BUILD/bin/Lathe.app" ]; then
  cmake -S "$REPO/native" -B "$GUI_BUILD" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=$ARCHS" \
        "-DLATHE_CORE_BINARY=$COREDIST/lathe" "-DVPF_CODESIGN_IDENTITY=-" -Wno-dev ${SRC_ARGS[@]+"${SRC_ARGS[@]}"} >/dev/null
fi
# (always built, even with SKIP_CPP: lathe_bundle_core copies the core just staged in)
cmake --build "$GUI_BUILD" -j "$(sysctl -n hw.ncpu)"
BUILT="$GUI_BUILD/bin/Lathe.app"
[ -d "$BUILT" ] || die "no Lathe.app under $GUI_BUILD/bin"
# a staged copy: what is signed and zipped (the build tree's own stays as the build left it)
APP="$OUT/stage/Lathe.app"
rm -rf "$OUT/stage"
mkdir -p "$OUT/stage"
/usr/bin/ditto "$BUILT" "$APP"
DIST="$APP/Contents/MacOS/coredist"
[ -f "$DIST/lathe" ] || die "Lathe.app carries no core (Contents/MacOS/coredist/lathe)"
# (not byte-compared with the staged core: the build signed its copy, which rewrites it; it must be no older)
[ ! "$COREDIST/lathe" -nt "$DIST/lathe" ] || die "Lathe.app's core is older than the one just staged"
for lib in "$COREDIST"/*.dylib; do
  [ -f "$DIST/$(basename "$lib")" ] || die "Lathe.app lacks $(basename "$lib") beside its core"
done
"$DIST/lathe" libav-version 2>&1 | grep -qi 'LGPL' || die "Lathe.app's core does not run (or reports no LGPL libav)"
# the LGPL notices and the configure record, where non-code belongs (codesign --strict refuses plain files in MacOS/).
# The source archive stays out: the notices and About point at the pinned upstream URL (tools/build-release-mac.sh)
LICENSES="$APP/Contents/Resources/licenses"
mkdir -p "$LICENSES"
for doc in "$REPO/THIRD_PARTY_NOTICES.txt" "$FFMPEG_LGPL/lathe-ffmpeg-configure.txt" "$FFMPEG_LGPL/COPYING.LGPLv2.1" \
           "$LIBJPEG_TURBO_MAC/LICENSE-libjpeg-turbo.md"; do
  [ -f "$doc" ] || die "missing LGPL compliance file: $doc"
  cp -f "$doc" "$LICENSES/"
done

# ---- 4: checks ----
step 4 "universal, macOS $MACOS_MIN floor, no Homebrew"
BAD=0
EXE="$APP/Contents/MacOS/$(defaults read "$APP/Contents/Info" CFBundleExecutable)"
[ "$(basename "$EXE")" = "Lathe" ] || { echo "  CFBundleExecutable is not Lathe"; BAD=1; }
[ "$(defaults read "$APP/Contents/Info" CFBundleIdentifier)" = "com.vacantsystems.lathe" ] || { echo "  CFBundleIdentifier is not com.vacantsystems.lathe"; BAD=1; }
PLIST_VER="$(defaults read "$APP/Contents/Info" CFBundleShortVersionString)"
[ "$PLIST_VER" = "$VERSION" ] || { echo "  CFBundleShortVersionString $PLIST_VER, tauri.conf.json $VERSION"; BAD=1; }
while IFS= read -r -d '' bin; do
  file -b "$bin" | grep -q 'Mach-O' || continue
  archs="$(lipo -archs "$bin")"
  min="$(minos_of "$bin")"
  echo "  ${bin#"$APP"/}: $archs, macOS $min"
  case " $archs " in *" arm64 "*) ;; *) BAD=1 ;; esac
  case " $archs " in *" x86_64 "*) ;; *) BAD=1 ;; esac
  if [ -z "$min" ] || ! version_le "$min" "$MACOS_MIN"; then BAD=1; fi
  if otool -L "$bin" | tail -n +2 | grep -qE "/opt/homebrew/|/usr/local/|$FFMPEG_LGPL"; then echo "    ^ links a Homebrew or build-prefix library"; BAD=1; fi
done < <(find "$APP" -type f -print0)
[ "$BAD" = "0" ] || die "a check above failed"

# ---- 5: sign, inside out ----
step 5 "codesign"
SIGN=(codesign --force --sign -)
[ -z "$IDENTITY" ] || SIGN=(codesign --force --options runtime --timestamp --sign "$IDENTITY")
for lib in "$DIST"/*.dylib; do "${SIGN[@]}" "$lib"; done
"${SIGN[@]}" "$DIST/lathe"
"${SIGN[@]}" "$APP"
if [ -z "$IDENTITY" ]; then
  echo "  ad hoc: this mac only (the manager's catalog.py pack re-signs it with the Developer ID)"
else
  echo "  Developer ID signed, hardened runtime, timestamped"
fi
codesign --verify --strict --deep "$APP" || die "codesign verification failed: $APP"

# ---- 6: notarize ----
step 6 "notarize + staple"
if [ -z "$IDENTITY" ]; then
  echo "  skipped: an ad hoc build has nothing to notarize"
elif [ "$SKIP_NOTARIZE" = "1" ]; then
  echo "  --skip-notarize: signed but NOT notarized"
else
  SUB="$OUT/Lathe-notarize.zip"
  rm -f "$SUB"
  /usr/bin/ditto -c -k --norsrc --noextattr --noacl --keepParent "$APP" "$SUB"
  notarize "$SUB"
  rm -f "$SUB"
  xcrun stapler staple -q "$APP" || die "stapling failed"
  xcrun stapler validate -q "$APP" || die "staple validation failed"
  echo "  notarized and stapled"
fi

# ---- 7: the manager's package ----
step 7 "the manager's zip"
mkdir -p "$MGR"
ZIP="$MGR/Lathe-$VERSION-mac.zip"
rm -f "$ZIP"
# (--norsrc --noextattr: no AppleDouble ._ entries for the files' extended attributes, com.apple.provenance and the
# like; unpacked by the manager they would be plain files inside the sealed bundle, and its signature would break)
/usr/bin/ditto -c -k --norsrc --noextattr --noacl --keepParent "$APP" "$ZIP"
if /usr/bin/unzip -Z1 "$ZIP" | grep -qvE '^Lathe\.app/' || /usr/bin/unzip -Z1 "$ZIP" | grep -qE '(^|/)\._|__MACOSX'; then
  die "$ZIP holds more than Lathe.app/ (or an AppleDouble ._ entry)"
fi

printf '\n\033[32mLathe %s (native, macOS):\033[0m\n' "$VERSION"
printf '  app      %s\n' "$APP"
printf '  manager  %s (%s)\n' "$ZIP" "$(du -h "$ZIP" | cut -f1)"
