# Changelog

What changed, newest first. One or two lines per entry: what it does for the user, plus the
non-obvious why. The commit body is the detail; this file is the skim layer.

**Every commit that changes behavior adds its entry here, in the same commit.** Pure chores
(formatting, ignore files) are exempt. Cross-repo rounds add a line in each repo they touched.
Short hashes are optional and get backfilled; never block a commit on one. Merge commits are
folded into the change they landed rather than given their own entry.

## 2026-11-03

- **Native Lathe on macOS**: `tools/build-release-native-mac.sh` builds a universal (arm64 + x86_64) `Lathe.app` with its own core and LGPL libav dylibs in `Contents/MacOS/coredist`, signed inside out, and Vacant Systems Manager's `Lathe-<version>-mac.zip`. The window's traffic lights are hidden (the title strip draws its own Minimize and Close), and the app never takes its own GUI for the core (the Mac's disk is case-insensitive: `MacOS/lathe` opens `MacOS/Lathe`). The LGPL FFmpeg and libjpeg-turbo build scripts take `MAC_ARCHS` for universal prefixes. WAVdesk pin c017f855.
- **Native Lathe: `--setup` / `--uninstall` run from main, not a static initializer**: in WAVdesk the same pattern ran setup before its string key globals were built, so it wrote onto the HKLM root and tried to delete HKLM itself; Lathe was safe only by luck. The registry helpers now refuse an empty or root key, and the one-copy hand-off stands aside for these runs. Explorer's Convert with Lathe verbs now go under the test key in a `--test-root` run too, so `native/tests/setup/check_setup.py` checks their paths and proves nothing outside the test root changes.

## 2026-11-02

- **One version for all of Lathe**: the CLI read its own `project(VERSION)`, so a 0.1.9 bump of the app still printed `lathe 0.1.8`. Now `tauri.conf.json` is the only source: the CLI and the native GUI read it at configure time (`cmake/lathe_version.cmake`), `tools/bump-version.ps1 <x.y.z>` keeps the Tauri manifests in step, and the release fails when the CLI, the GUI's version resource, the manifests or the installer disagree.
- **The manager's zip builds from Git Bash too**: the release script ran a bare `tar.exe`, which Git Bash resolves to GNU tar (no zip); it now runs Windows' own `%SystemRoot%\System32\tar.exe`.
- **A Vacant Systems Manager package beside the installer**: `tools/build-release-native.ps1` also writes `native/build-release/manager/Lathe-<version>-win-x64.zip`, the installed folder under one `Lathe/` folder (no `uninstall.exe`), zipped by tar with every file stamped at HEAD's commit time so a rebuild of the same commit packs the same bytes. vacant-labs' `check_app.py` passes on it (install, setup twice, remove, all under a test root).
- **Native Lathe sets itself up for Vacant Systems Manager**: `lathe-gui --setup` does what the installer's Install section does (Explorer's Convert with Lathe verbs, the shortcuts, HKLM vacantsystems\Lathe, Shared's registry.json entry, the uninstall entry "Lathe" with `UninstallString "<exe>" --uninstall`), silently and safely twice; `--uninstall --quiet --keep-files` undoes it for the manager, and `--uninstall [--quiet]` is the installer's uninstall in the app (elevates, asks to quit Lathe and about the settings, then removes its folder after it exits). `--test-root <dir>` keeps every write under a test folder and HKCU. Both run before the window and the one-copy hand-off; exit 0 / 1 / 2. The NSIS installer stays a release path, with the same keys and values.
- **Native Lathe (VX): menus hover in green**: the hovered row of the VX look's menus (the look chooser, the dropdowns) lights in Afterglow's green. WAVdesk pin 9904b249; vacant-labs stays 030d8d34.
- **Native Lathe on vacant-labs 030d8d34 and WAVdesk 700a742f**: both pins move together (WAVdesk's native shell pins that vacant-labs commit); the only vpf / Etch change since 11a77309 is Etch's own look label, now "1-Bit".
- **Native Lathe: the Dev look is called 1-Bit** in the look chooser (WAVdesk pin a863ee80, feat/native-shell). Only the label: the skin id stays "dev", so a saved choice keeps working.
- **Native Lathe: a dropped folder is one row**: "Drums/  2,000 files · 3.2 GB · aif, wav", folded past 20 files (click to open; its X removes the folder's files), with a summary line over the list (done / total and failures while converting) and a toggle that shows only the failed files. Only the visible rows are laid out, so 10,000 inputs stay as quick as a few. WAVdesk pin a36a2832 (464cc358).
- **Native Lathe: tooltips draw over everything**: the WAVdesk pin moves to 813b9a8a, where the shared window draws its tip in a child above the others and asks child widgets for theirs too (written for the Latch windows); the text field no longer covers a tip.
- **Native Lathe: a convert's end is never held up by another one**: the WAVdesk pin moves to 4903c46f, the parity branch merged with WAVdesk's Latch hooks, which carries the shared launcher's fix that a child inherits only its own std handles. A child started at the same moment used to hold this one's output pipe open, so its finish waited on the other's exit.
- **Native Lathe says why**: hovering a failed output shows lathe's error; an input shows its full path (and "Won't convert to X: kind mismatch"); the core chip, the Convert button (why it is disabled or skips some) and the icon buttons show the Tauri tooltips. The WAVdesk pin moves to 637234e1 (feat/lathe-parity), where the shared window reports its tips and runs them itself in a standalone app.
- **Native Lathe: Overwrite originals across a format change removes the original** after a clean convert, as the Tauri app did; a failed or cancelled convert keeps it. It used to leave both files.
- **Native Lathe: Export to... rows follow a renamed move**: a file that lands as "name (2)" is now tracked there, so Show in folder and drag-out keep working (the app reports where each move landed instead of the window guessing by name).
- **Native Lathe keys and gestures**: Delete removes the selected rows of the list last touched (outputs leave the log, files stay), Escape clears the input selection, each finished output row slides in a "Clear from log" X, and dragging an input row away removes it.
- **Native Lathe no longer stalls on big folders**: input sizes are read off the UI thread, 64 at a time, so a 2000-file folder or a network share keeps the window responsive.
- **Native Lathe shows its own icon** in the title strip and the About window (gui/src/assets/app-icon.png, embedded, area-downscaled per size).

## 2026-11-01

- **Native Lathe: dragging an output out carries the Tauri drag chip**: the file's name, plus "+N" for the rest of the picked outputs, instead of the OS's plain file picture. The WAVdesk pin moves to 4c68c3b2, where the shared Lathe window draws WAVdesk's own port of the chip (shell/drag_chip.cpp, now compiled here too) through vpf's drag-image hook.
- **Native Lathe keeps the Tauri app's settings**: its first launch reads the format, destination, quality knobs and presets the Tauri Lathe saved (its WebView2 localStorage, a LevelDB read directly: logs and Snappy tables) into native_ui.json, once, never over a value already there. The installer no longer deletes the Tauri profile at install (there would be nothing left to read); the uninstaller removes it.
- **Native Lathe: a headless run beside an open window runs**: the one-copy check runs before main, when vpf has not parsed the arguments yet, so its headless test always came out false and a `--steps` / `--screenshot` run handed its arguments to the open Lathe and quit. It now reads the raw command line (as native Latch does).
- **Native Lathe: no stuck `lathe.exe` when the window quits mid-launch**: the WAVdesk pin moves to 65cd0a8c, where the shared process launcher starts each child already inside its kill-on-close job instead of suspended until it is assigned. A Lathe exiting in that gap used to leave the child suspended forever; the CLI check now also runs in look-only headless runs, which skipped it to dodge this.
- **A cancelled convert leaves nothing behind**: an empty output that is not the input is now taken as the caller's name reservation and written straight over, not staged through a `.wdtmp<pid>` sibling. The native GUIs (Lathe's and WAVdesk's Lathe window) reserve the output name before spawning `lathe`, so every cancel used to strand a partial temp the hard kill skipped. Replacing a real existing file still stages; no flag or output changed.
- **Native Lathe GUI (vpf), Windows first** (`native/`, branch feat/native-vpf): the converter as a standalone vpf app replacing the Tauri GUI, built from WAVdesk native's own Lathe window (compiled from a pinned WAVdesk commit, not forked) plus the app's frame, Minimize, About, a look chooser (Dev look, VX, Focused, Zen Dark), the Explorer verb arguments and one-instance handoff, and the Shared registry self-registration. `tools/build-release-native.ps1` + `native/installer/lathe.nsi` build an installer with the Tauri identity (Program Files\Vacant Systems\Lathe, uninstall key "Lathe", lathe-gui.exe) so it installs over the Tauri app, and copy it where WAVdesk's release picks up its companion installer. Settings start fresh: the Tauri webview's localStorage is not migrated.
- **The Mac build runs on macOS 12 and later**: the `lathe` core, its libav dylibs and the bundled libjpeg were all built for the build Mac's own OS (minos 26.0), so dyld refused them on macOS 12-25 (Lathe could not convert; WAVdesk could not encode edits of lossy files). The core, the LGPL ffmpeg build and the GUI now target 12.0, and the release script aborts on any Mach-O above that floor.
- **libjpeg is built from pinned source, statically**: LibRaw's DNG lossy decoder used to link Homebrew's jpeg-turbo, which was copied into the bundle as an unvetted dylib built for the host OS. `tools/build-libjpeg-turbo-mac.sh` builds libjpeg-turbo 3.2.0 (checksum-verified) for macOS 12; its IJG/BSD/zlib notice now ships in the licenses folder, THIRD_PARTY_NOTICES.txt and the About window. The release script refuses any Homebrew library outright.

## 2026-10-07

- **Version 0.1.8.** Ships the camera RAW half-size convert (WAVdesk icon thumbnails) and the native arm64 ffmpeg bootstrap on Apple Silicon.
- **Apple Silicon downloads a native ffmpeg**: when the shared bin has no ffmpeg, `lathe` now fetches a pinned, checksum-verified arm64 build (martin-riedl.de 1783011502_8.1.2) instead of evermeet.cx's Intel-only one, which needed Rosetta (or failed without it). Intel Macs keep evermeet. Normally WAVdesk provisions the suite's arm64 ffmpeg first, so this is the standalone fallback.
- **`convert --raw-half-size` for camera RAW previews**: LibRaw's half-size mode reads each 2x2 Bayer
  quad as one pixel and skips the demosaic, so a 24 MP RAW converts in 0.25 s instead of 0.95 s (12 MP:
  0.15 s instead of 0.5 s). WAVdesk's icon thumbnails use it; a 512 px thumb from it is ~50 dB from the
  full decode. Every other convert keeps the full-resolution decode. WAVdesk detects the flag in
  `lathe --help`, so an older lathe keeps working.

## 2026-09-26

- **Version 0.1.7.** Ships this repo's fixes from the WAVdesk 0.1.8 stabilization round (see the 2026-09-25 entries below).

## 2026-09-25

- **A failed convert never deletes a file that was already there**: "Overwrite originals" with an
  unchanged format makes ffmpeg refuse (output == input), and the failure path then removed the
  output path, which was the user's only copy. Mac had a guard; Windows did not, and on every
  platform a failed convert or extract onto any existing file (an in-place extract, or a
  cross-format overwrite onto a same-named sibling) still removed it. Any existing target is now
  encoded to a `<stem>.wdtmp<pid>.<ext>` sibling and renamed over it only on success; a rename
  that fails (Windows: the file is open elsewhere) keeps the original and reports why.

## 2026-08-30

- **Mac builds are Developer ID signed and notarized**: the mac release script only ad-hoc signed
  (`codesign --deep -s -`), so every DMG it produced was refusable by Gatekeeper on any machine but
  the one that built it, and `--deep` is the flag the notary service rejects outright. It now signs
  inside-out per nested binary (the libav dylibs included, which is what keeps the hardened runtime's
  library validation happy without an entitlements carve-out), seals the bundle last, signs the DMG
  container, then notarizes and staples.
- **The LGPL notices moved out of `Contents/MacOS`**: `COPYING.LGPLv2.1`, the configure record and
  the third-party notices were being mirrored in next to the binaries. `Contents/MacOS` is a code
  directory, so strict signature verification refused the whole bundle: "code object is not signed
  at all". They live in `Contents/Resources/licenses` now. Latent since the mac pipeline was written
  and invisible until Developer ID signing made the script verify what it produced.
- **The FFmpeg notices describe the right FFmpeg**: the About window's only FFmpeg notice covered
  the converter program Lathe downloads and invokes, which is mere aggregation and carries no source
  obligation, while saying nothing about the libav shared libraries actually linked into the core.
  Those now get their own notice naming the libraries, the pinned upstream source archive, and the
  fact that the configure arguments shipping in the app reproduce them. The old notice stays,
  correctly scoped to the downloaded GPL build.
- **The notices no longer point at a file that is not there**: `THIRD_PARTY_NOTICES.txt` said the
  source archive was "distributed beside the shared libraries inside the application bundle". True
  on Windows, false on mac since the archive is pruned for size, so it sent people looking inside the
  app for something absent. It now gives the pinned URL, the real location of the LGPL text and
  configure record (`Contents/Resources/licenses`), and says plainly that the archive is not bundled.
  Source delivery for the LGPL dylibs is the URL plus the configure record: applying those arguments
  to that archive reproduces the shipped libraries. Version and URL are in lockstep across
  `tools/build-ffmpeg-lgpl-mac.sh`, the notices, and `AboutApp.tsx`: bump all three together.

## 2026-08-20

- **Version stamps unified at 0.1.6, ~29 MB off the mac app** (`d183062`): every stamp now agrees
  and the C++ banner derives from `project(VERSION)`. The mac bundle shipped the dylib set twice
  (Resources/coredist plus the MacOS mirror tools.rs actually uses) and shipped the 11 MB LGPL
  source tarball to end users; `tauri.macos.conf.json` empties `bundle.resources`.

## 2026-07-16

- **Pinned LGPL ffmpeg build and a shippable mac release pipeline** (`cdd0c84`): builds FFmpeg 8.1.2
  from the sha256-pinned official source with LGPL-only flags and VideoToolbox enabled, stages the
  compliance files into coredist, and rejects any GPL or Homebrew linkage in a release build.

## 2026-07-15

- **Ignore macOS Finder metadata** (`48381bb`).

## 2026-07-10

- **VideoToolbox hardware decode on macOS** (`cc8b0f1`, merged in `3b197d2`): the mac decode-server
  was pure software, so 1080p H.264 cost ~29.5 ms of CPU per frame on the target iMac. It pegged the
  CPU, the frontend 600 ms stall watchdog fired, and the reconnect re-decoded from a keyframe (the
  owner's "SUPER laggy and blurry"). VideoToolbox cuts that to ~7.4 ms. It is attached only for
  H.264/HEVC, so AV1/VP9 open the software dav1d decoder instead of failing on a context they cannot
  use. The Windows d3d11va path is byte-identical.
- **Rounded window corners on macOS** (`c7b2d55`, merged in `a93c927`): macOS draws undecorated
  NSWindows square where the Windows 11 DWM rounds every top-level window for free. The shell clips
  to a 10 px radius; the drag overlay is excluded so its transparent chip surface never clips.
- **The About window reads the runtime version** (`40accd5`, merged in `e0b5efa`): it was hardcoded
  to 1.0.
- **macOS tool acquisition and a .dmg release pipeline** (`5ea7b3a`, merged in `6ea09dd`): POSIX
  download through curl, ffmpeg bootstrap into the managed shared bin, and an `exe_dir` fix so the
  next-to-exe portable tier resolves instead of returning ".". The release script bundles the LGPL
  libav closure into the .app with `@loader_path` rewrites and ships an ad-hoc signed dmg. Windows
  paths stay byte-identical.
- **One drag chip on macOS** (`2866c6b`, merged in `c5bb359`): the native NSDraggingSession image
  and the overlay webview both rendered a chip, the field "double chip". The overlay is Linux-only
  now (XDND still uses it) and stays hidden on mac; every hide and cleanup path remains
  unconditional so a stray-shown overlay still tears down.
- **libav linked on macOS, and overwrite-in-place no longer eats the original** (`427a482`, merged
  in `bb84ab1`): the ffmpeg link block was gated on WIN32, so `lathe libav-version` reported "not
  built in" on mac, which fails the Latch capability probe and kills the WAVdesk video decode
  contract. Separately, converting to the same format with overwrite passed output == input; ffmpeg
  refuses in-place editing with -22, and the failure path then removed the output, deleting the only
  copy. Output now goes to a sibling temp and is renamed over the original on success.

<!-- Started 2026-08-30 alongside WAVdesk's, for the cross-repo rounds the three share; the sections before it backfilled 2026-10-08 from the 15 commits before (merges folded). -->
