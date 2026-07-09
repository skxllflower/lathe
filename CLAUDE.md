# Lathe

Standalone media converter: ffmpeg wrapper (`lathe.exe` CLI, `src/`) + Tauri v2 GUI (React/Vite
`gui/src`, Rust host `gui/src-tauri`). Fork-and-owned from WAVdesk scaffolding; WAVdesk also drives
lathe.exe directly (Convert flow, lossy round-trips, video decode-server).
Owner: skxllflower. Default branch: `master` (NOT main).

## Build / run
- CLI: CMake build; SHIP THE RELEASE lathe.exe (Debug LibRaw demosaic is ~7s per RAW).
- Dev GUI: `cd gui && export PATH="/c/Program Files/nodejs:/c/Users/Owner/AppData/Roaming/npm:$PATH" && pnpm tauri dev` (port 5174).
- Checks: `pnpm typecheck`; `cargo check` in gui/src-tauri. Release: `& .\tools\build-release.ps1` (NSIS only).

## Iron rules + lockstep invariants
- **The decode/convert contract with WAVdesk is sacred**: WAVdesk's Rust (`lathe_convert`, the video
  decode-server protocol) and C++ (`wavdesk_lathe.cpp` resolution + lossy round-trip bracket) depend
  on lathe.exe's CLI surface. Never change flags/output shapes without a same-day WAVdesk-side update.
- **Shared bin**: ffmpeg in `%LOCALAPPDATA%\Vacant Systems\Shared\bin` + manifests; resolution order
  env/portable/managed + installed-location fallbacks stay lockstep across the three repos.
- RAW: LibRaw 0.21.4 static (CDDL), `open_buffer` for UTF-8 paths, demosaic -> 16-bit PPM -> ffmpeg;
  CMP0077 BUILD_SHARED_LIBS gotcha in CMake.
- Input gating lives in the GUI addPaths filetype gate (svg/heic stay gated; RAW allowed).
- New Rust commands doing I/O: `async` + `spawn_blocking`. No em dashes in user-facing text.

## Coordination
Coordinated together with WAVdesk. The agent-workflow playbook, verification bars, and the full
cross-repo gotcha list live in `C:\Users\bansh\Dev\wavdesk\.claude\memory\` (orchestration.md,
gotchas.md). Read them before multi-file work here.
Commit trailer: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
