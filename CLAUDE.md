# Lathe

Standalone media converter: ffmpeg/libav wrapper (the `lathe` CLI, `src/`) + Tauri v2 GUI
(React/Vite `gui/src`, Rust host `gui/src-tauri`), being replaced by a native vpf GUI (`native/`,
branch `feat/native-vpf`, Windows first; see "Native GUI" below). Fork-and-owned from WAVdesk scaffolding; WAVdesk
also drives the lathe CLI directly (Convert flow, lossy round-trips, video decode-server).
Owner: skxllflower. Default branch: `master` (NOT main). Version 0.1.8. Ships on Windows (NSIS) and
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

**Native GUI (vpf, `native/`)**
- WAVdesk native's own Lathe window (`native/shell/lathe_window.*` + ExtWindow, the skins, dialogs,
  tool runner) compiled UNCHANGED from a pinned WAVdesk commit; `native/app/` adds the app frame,
  Minimize, About, the look chooser, launch args, one-instance handoff, registry self-registration,
  and `host_stubs.cpp` (the 5 WAVdesk-library calls those files make: data root = Lathe's folder).
  vpf and WAVdesk are FetchContent-pinned to commits (`native/CMakeLists.txt`): a fix to the window
  lands in WAVdesk first, then this pin moves. The pin must be a pushed commit (release fetches it).
- Dev: `cmake -S native -B native/build -A x64 -DFETCHCONTENT_SOURCE_DIR_WAVDESK=<wavdesk checkout>`
  then `cmake --build native/build --config Release` -> `native/build/bin/Release/lathe-gui.exe`.
  A dev run (exe in a build tree) uses the repo's `build/Release/lathe.exe` via LATHE_EXE and does not
  self-register; an installed run uses `<exe>/coredist/lathe.exe` and registers it in registry.json.
- Release (Windows): `& .\tools\build-release-native.ps1` -> `native/build-release/Lathe_<ver>_x64-setup.exe`
  (`native/installer/lathe.nsi`: the Tauri install folder, uninstall key and exe name), also copied to
  `gui/src-tauri/target/release/bundle/nsis/` where WAVdesk's release picks up companion installers.
- Data: `%LOCALAPPDATA%\Vacant Systems\Lathe` (settings.json: skin; native_ui.json: the window's
  settings and presets under the Tauri localStorage key names). Headless runs write nothing.

**Checks (both):** `pnpm typecheck`; `cargo check` in `gui/src-tauri` (no Rust tests yet); the native GUI builds
and its headless runs (`lathe-gui --skin vx --screenshot out.png`, `--steps file.json`: vacant-labs vpf/CLAUDE.md).

## Iron rules + lockstep invariants
- **One version:** `gui/src-tauri/tauri.conf.json` `version`. The CLI (`project(VERSION)`) and the native GUI read it
  through `cmake/lathe_version.cmake`; bump with `tools/bump-version.ps1 <x.y.z>` (also package.json, Cargo.toml,
  Cargo.lock). `build-release-native.ps1` fails when any built piece says another version.
- **The decode/convert contract with WAVdesk is sacred:** WAVdesk's Rust (`lathe_convert` in
  `external_tools.rs`, the video decode-server protocol) and C++ (`src/core/wavdesk_lathe.cpp`
  resolution + lossy round-trip bracket) depend on this CLI's surface. Never change flags or output
  shapes without a same-day WAVdesk-side update.
- **Shared bin:** Windows `%ProgramData%\Vacant Systems\Shared\bin`, macOS
  `~/Library/Application Support/Vacant Systems/Shared/bin`, plus `registry.json` manifests.
  `gui/src-tauri/src/tools.rs` is a fork of WAVdesk's `external_tools.rs` and `src/paths.cpp` keeps
  three platform branches; resolution order stays lockstep across the three repos.
  (`src/paths.h` still says LOCALAPPDATA in a comment: it is wrong.)
- **ffmpeg version pin is lockstep across four files here:** `FFMPEG_VERSION` + SHA256 in
  `tools/build-ffmpeg-lgpl-mac.sh`, `THIRD_PARTY_NOTICES.txt`, `gui/src/AboutApp.tsx` and
  `native/app/about_window.cpp`. The LGPL source offer points at the pinned upstream URL; bump all together.
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
