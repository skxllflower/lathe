# Changelog

What changed, newest first. One or two lines per entry: what it does for the user, plus the
non-obvious why. The commit body is the detail; this file is the skim layer.

**Every commit that changes behavior adds its entry here, in the same commit.** Pure chores
(formatting, ignore files) are exempt. Cross-repo rounds add a line in each repo they touched.
Short hashes are optional and get backfilled; never block a commit on one. Merge commits are
folded into the change they landed rather than given their own entry.

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
