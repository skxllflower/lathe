#!/usr/bin/env bash
# build-release-mac.sh - one command to produce a shippable Lathe .app + .dmg on macOS.
#
# The macOS counterpart of tools/build-release.ps1. Lathe's Tauri GUI spawns its
# C++ core (the `lathe` CLI) as a subprocess. This stages the freshly-built core
# plus the LGPL libav dylibs it links into gui/src-tauri/coredist/, which
# tauri.conf bundles as a resource; tools.rs resolves the core next to the GUI
# binary inside the .app.
#
# libav linkage:
#   * If the LGPL ffmpeg prefix exists (/Users/skxllflower/dev/ffmpeg-lgpl-mac),
#     configure against it via PKG_CONFIG_PATH -> the shipped lathe links LGPL
#     libav (safe to distribute). The linked dylibs are bundled INTO the .app and
#     their install names rewritten to @loader_path so the binary references no
#     /usr/local or build-prefix paths (verified with otool).
#   * Otherwise fall back to Homebrew ffmpeg (GPL, dev-only) with a loud warning;
#     the resulting dmg is a DEV artifact, not shippable to testers.
#
# The Windows bundle config is untouched: we pass --bundles app,dmg on the CLI
# (tauri.conf still says nsis) so Windows builds stay byte-identical.
#
#   tools/build-release-mac.sh            # full release build
#   SKIP_CPP=1 tools/build-release-mac.sh # reuse existing build/
set -euo pipefail

export PATH="$HOME/.cargo/bin:/usr/local/bin:$PATH"

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$REPO/build"
GUI="$REPO/gui"
TAURI="$GUI/src-tauri"
COREDIST="$TAURI/coredist"
CORE="lathe"
PRODUCT="Lathe"
BUNDLE="$TAURI/target/release/bundle"
FFMPEG_LGPL="/Users/skxllflower/dev/ffmpeg-lgpl-mac"

step() { printf '\n\033[36m[%s] %s\033[0m\n' "$1" "$2"; }

# Copy every non-system dylib in `main_bin`'s dependency closure next to it and
# rewrite each reference (and each copied lib's id + inter-lib references) to
# @loader_path, so the shipped binary references no absolute build/Homebrew
# paths. bash 3.2-safe: no associative arrays; "already copied" == file present
# in the dest dir.
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
        "$FFMPEG_LGPL"/*|/usr/local/*)
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

# ---- Step 0: kill running dev/release instances (tolerant) ----------------
step 0 "Kill running $PRODUCT instances"
pkill -x "$PRODUCT" 2>/dev/null || true
pkill -x "$CORE" 2>/dev/null || true
sleep 1 || true

# ---- Step 1: build the C++ core (Release), linking LGPL libav -------------
step 1 "C++ core (Release)"
LGPL_OK=0
if [ "${SKIP_CPP:-0}" = "1" ]; then
  echo "  SKIP_CPP=1: reusing existing build/"
  [ -d "$FFMPEG_LGPL/lib" ] && LGPL_OK=1
else
  if [ -d "$FFMPEG_LGPL/lib" ]; then
    echo "  linking LGPL libav from $FFMPEG_LGPL"
    LGPL_OK=1
    PKG_CONFIG_PATH="$FFMPEG_LGPL/lib/pkgconfig" cmake -B "$BUILD" -S "$REPO" -DCMAKE_BUILD_TYPE=Release
    PKG_CONFIG_PATH="$FFMPEG_LGPL/lib/pkgconfig" cmake --build "$BUILD" -j6
  else
    printf '\033[33m  WARNING: %s not found — falling back to Homebrew ffmpeg.\n' "$FFMPEG_LGPL/lib"
    printf '  The linked libav is GPL: this dmg is a DEV artifact, NOT shippable.\033[0m\n'
    cmake -B "$BUILD" -S "$REPO" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILD" -j6
  fi
fi
CORE_SRC="$BUILD/$CORE"
[ -f "$CORE_SRC" ] || { echo "core binary not found: $CORE_SRC" >&2; exit 1; }

# Guard (mirror of the Windows script): refuse to ship a no-video / GPL core.
LIBAV_INFO="$("$CORE_SRC" libav-version 2>&1 || true)"
echo "  libav-version:"; printf '%s\n' "$LIBAV_INFO" | sed 's/^/    /'
if printf '%s' "$LIBAV_INFO" | grep -qi 'not built in'; then
  echo "ERROR: lathe built WITHOUT libav — decode-server would be a stub." >&2
  exit 1
fi
if [ "$LGPL_OK" = "1" ] && printf '%s' "$LIBAV_INFO" | grep -qi 'GPL/nonfree\|do not ship'; then
  echo "ERROR: expected LGPL libav but the core reports GPL/nonfree." >&2
  exit 1
fi

# ---- Step 2: stage core + libav dylibs + notices into coredist ------------
step 2 "Stage core + dylibs -> coredist"
mkdir -p "$COREDIST"
find "$COREDIST" -mindepth 1 ! -name '.gitkeep' -exec rm -rf {} + 2>/dev/null || true
cp -f "$CORE_SRC" "$COREDIST/$CORE"
chmod 0755 "$COREDIST/$CORE"
[ -f "$REPO/THIRD_PARTY_NOTICES.txt" ] && cp -f "$REPO/THIRD_PARTY_NOTICES.txt" "$COREDIST/"

echo "  bundling libav dylib closure + fixing install names..."
bundle_dylibs "$COREDIST/$CORE"

echo "  otool -L (staged core) — must show NO /usr/local or build-prefix paths:"
otool -L "$COREDIST/$CORE" | sed -n '2,40p'
if otool -L "$COREDIST/$CORE" | tail -n +2 | grep -qE "$FFMPEG_LGPL|/usr/local/"; then
  echo "ERROR: staged core still references absolute /usr/local or build-prefix libs." >&2
  exit 1
fi
DYLIB_COUNT="$(find "$COREDIST" -name '*.dylib' | wc -l | tr -d ' ')"
echo "  staged $CORE + $DYLIB_COUNT dylib(s) -> $COREDIST"

# ---- Step 3: bundle the .app ----------------------------------------------
# Only the `app` bundle: Tauri's own dmg step drives Finder via AppleScript to
# lay out the disk-image window, which fails without an interactive GUI session
# (headless build agents, ssh). We build the .app here and package the .dmg
# ourselves with hdiutil in Step 5 (no Finder dependency). The Windows bundle
# config (nsis) is untouched — targets are passed on the CLI.
step 3 "pnpm tauri build (app)"
command -v pnpm >/dev/null 2>&1 || { echo "pnpm not found on PATH" >&2; exit 1; }
( cd "$GUI" && pnpm install && pnpm tauri build --bundles app )

APP="$(find "$BUNDLE/macos" -maxdepth 1 -name '*.app' 2>/dev/null | head -1)"
[ -n "$APP" ] || { echo "no .app produced under $BUNDLE/macos" >&2; exit 1; }

# Self-resolution belt-and-suspenders: tools.rs looks for the core at
# <dir-of-GUI-binary>/coredist/lathe (Contents/MacOS/coredist). Tauri resources
# land in Contents/Resources/. Mirror the whole coredist (core + dylibs) next to
# the GUI binary if it isn't already there.
MACOS_DIR="$APP/Contents/MacOS"
if [ ! -e "$MACOS_DIR/coredist/$CORE" ]; then
  echo "  mirroring coredist next to the GUI binary (Contents/MacOS/coredist)"
  mkdir -p "$MACOS_DIR/coredist"
  cp -Rf "$COREDIST/." "$MACOS_DIR/coredist/"
  rm -f "$MACOS_DIR/coredist/.gitkeep"
  chmod 0755 "$MACOS_DIR/coredist/$CORE"
fi

echo "  otool -L (shipped core inside .app):"
otool -L "$MACOS_DIR/coredist/$CORE" | sed -n '2,40p'
if otool -L "$MACOS_DIR/coredist/$CORE" | tail -n +2 | grep -qE "$FFMPEG_LGPL|/usr/local/"; then
  echo "ERROR: shipped core still references absolute /usr/local or build-prefix libs." >&2
  exit 1
fi

# ---- Step 4: ad-hoc codesign so translocation behaves ---------------------
step 4 "Ad-hoc codesign"
codesign --force --deep -s - "$APP"
codesign --verify --verbose=1 "$APP" || echo "  (codesign verify warned; ad-hoc is expected to be shallow)"

# ---- Step 5: package a dmg carrying the SIGNED app (hdiutil, no Finder) ----
step 5 "Package signed .dmg"
VER="$(node -p "require('$TAURI/tauri.conf.json').version" 2>/dev/null || echo 0.0.0)"
DMG="$BUNDLE/dmg/${PRODUCT}_${VER}_x64.dmg"
mkdir -p "$BUNDLE/dmg"
STAGE="$(mktemp -d)"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
rm -f "$DMG"
hdiutil create -volname "$PRODUCT" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
rm -rf "$STAGE"

# ---- Done -----------------------------------------------------------------
printf '\n\033[32mArtifacts:\033[0m\n'
printf '  app: %s  (%s)\n' "$APP" "$(du -sh "$APP" | cut -f1)"
printf '  dmg: %s  (%s)\n' "$DMG" "$(du -sh "$DMG" | cut -f1)"
if [ "$LGPL_OK" = "1" ]; then
  echo "  libav: LGPL (shippable)"
else
  printf '\033[33m  libav: GPL via Homebrew (DEV ONLY — do not distribute)\033[0m\n'
fi
cat <<EOF

Tester install note:
  1. Open the .dmg and drag $PRODUCT to Applications (or run it in place).
  2. First launch: right-click $PRODUCT.app -> Open (unsigned app), OR run:
       xattr -dr com.apple.quarantine "/Applications/$PRODUCT.app"
  This build is ad-hoc signed and NOT notarized (alpha).
EOF
