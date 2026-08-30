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
- **The build no longer claims LGPL source compliance it does not have**: the corresponding-source
  tarball is staged for the Windows pipeline and then pruned from the mac bundle for size, so it
  reaches neither the app nor the DMG. The summary said "corresponding source (shippable)" anyway.
  It now says what actually shipped and flags that source delivery is unsettled. OPEN ITEM: shipping
  the LGPL dylibs needs the source to accompany them or a written offer.

<!-- Started 2026-08-30 alongside WAVdesk's, for the cross-repo rounds the three share. -->
