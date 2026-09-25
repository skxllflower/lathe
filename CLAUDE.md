# Lathe

Standalone media converter: ffmpeg/libav wrapper (the `lathe` CLI, `src/`) + Tauri v2 GUI
(React/Vite `gui/src`, Rust host `gui/src-tauri`). Fork-and-owned from WAVdesk scaffolding; WAVdesk
also drives the lathe CLI directly (Convert flow, lossy round-trips, video decode-server).
Owner: skxllflower. Default branch: `master` (NOT main). Version 0.1.6. Ships on Windows (NSIS) and
macOS (Developer ID signed + notarized DMG). Day-to-day development happens on the Mac.

## Build / run
- Ship the RELEASE core: Debug LibRaw demosaic is ~7 s per RAW.

**macOS**
- Shell setup: `export PATH="/opt/homebrew/opt/rustup/bin:/opt/homebrew/bin:$PATH"`.
- Core: `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build` -> `build/lathe`.
  Dev links Homebrew libav through pkg-config: that is GPL, fine for dev, and must never ship.
- Dev GUI: `cd gui && pnpm tauri dev` (port 5174). The dev fallback finds `~/Dev/lathe/build/lathe`.
- Release: `tools/build-release-mac.sh` (builds into `build-release-mac/`). It links a pinned
  LGPL-only libav from `~/Dev/ffmpeg-lgpl-mac` (built by `tools/build-ffmpeg-lgpl-mac.sh` if
  missing) and ABORTS unless the core reports an LGPL libav. Bundles the dylibs rewritten to
  `@loader_path`, puts notices in `Resources/licenses`, signs inside-out, notarizes with keychain
  profile `wavdesk-notary`. Needs `APPLE_SIGNING_IDENTITY`.
- The ffmpeg CLI itself is fetched at runtime on mac (`src/bootstrap.cpp`).

**Windows**
- Dev GUI: `cd gui && export PATH="/c/Program Files/nodejs:$HOME/AppData/Roaming/npm:$PATH" && pnpm tauri dev`.
- Release: `& .\tools\build-release.ps1` (NSIS only).

**Checks (both):** `pnpm typecheck`; `cargo check` in `gui/src-tauri` (no Rust tests yet).

## Iron rules + lockstep invariants
- **The decode/convert contract with WAVdesk is sacred:** WAVdesk's Rust (`lathe_convert` in
  `external_tools.rs`, the video decode-server protocol) and C++ (`src/core/wavdesk_lathe.cpp`
  resolution + lossy round-trip bracket) depend on this CLI's surface. Never change flags or output
  shapes without a same-day WAVdesk-side update.
- **Shared bin:** Windows `%ProgramData%\Vacant Systems\Shared\bin`, macOS
  `~/Library/Application Support/Vacant Systems/Shared/bin`, plus `registry.json` manifests.
  `gui/src-tauri/src/tools.rs` is a fork of WAVdesk's `external_tools.rs` and `src/paths.cpp` keeps
  three platform branches; resolution order stays lockstep across the three repos.
  (`src/paths.h` still says LOCALAPPDATA in a comment: it is wrong.)
- **ffmpeg version pin is lockstep across three files here:** `FFMPEG_VERSION` + SHA256 in
  `tools/build-ffmpeg-lgpl-mac.sh`, `THIRD_PARTY_NOTICES.txt`, and `gui/src/AboutApp.tsx`.
  The LGPL source offer points at the pinned upstream URL; bump all three together.
- RAW: LibRaw 0.21.4 static (CDDL), `open_buffer` for UTF-8 paths, demosaic -> 16-bit PPM ->
  ffmpeg; CMP0077 `BUILD_SHARED_LIBS` cache-force gotcha in CMake.
- Input gating lives in the GUI addPaths filetype gate (`ConvertApp.tsx`): only `.svg` is refused;
  HEIC and RAW are allowed.
- New Rust commands doing I/O: `async` + `spawn_blocking`. No em dashes in user-facing text.
- There is no file log in lathe yet.

## Changelog (required)
`CHANGELOG.md` at the repo root is a running log, newest first. Every commit that changes behavior
adds its entry to the top dated section IN THAT COMMIT: one or two lines, what changed for the user
plus the non-obvious why. Pure chores are exempt. A cross-repo round adds a line in every repo it
touched.

## Coordination
Coordinated with WAVdesk, checked out as a sibling (`../wavdesk`). The agent playbook, verification
bars and cross-repo gotchas live in `../wavdesk/.claude/memory/` (orchestration.md, gotchas.md,
section "Cross-repo lockstep"). Read them before multi-file work here. Commit trailers follow the
harness's attribution instructions; don't hard-code a model name.
