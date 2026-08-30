# Changelog

What changed, newest first. One or two lines per entry: what it does for the user, plus the
non-obvious why. The commit body is the detail; this file is the skim layer.

**Every commit that changes behavior adds its entry here, in the same commit.** Pure chores
(formatting, ignore files) are exempt. Cross-repo rounds add a line in each repo they touched.
Short hashes are optional and get backfilled; never block a commit on one.

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

<!-- Started 2026-08-30 alongside WAVdesk's, for the cross-repo rounds the three share. -->
