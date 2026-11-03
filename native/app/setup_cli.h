// Vacant Systems Manager's app contract (vacant-labs apps/manager/docs/app-contract.md), Lathe's half: the manager
// installs Lathe from a zip and the app sets itself up, as the NSIS installer's Install / Uninstall sections do.
//
//   lathe-gui --setup [--test-root <dir>]                          Explorer's Convert with Lathe verbs, the shortcuts,
//                                                                  HKLM vacantsystems\Lathe, Shared/registry.json's
//                                                                  "lathe", the uninstall entry "Lathe"; idempotent
//   lathe-gui --uninstall --quiet --keep-files [--test-root <dir>] what --setup made, gone (the manager removes the folder)
//   lathe-gui --uninstall [--quiet]                                Windows' installed apps: the installer's uninstall
//                                                                  (elevated, asks to quit Lathe, asks about the user's
//                                                                  settings; --quiet asks nothing and keeps them), then
//                                                                  the folder once this process has gone
//
// --test-root: nothing real touched (the entry under HKCU\Software\Vacant Systems\Manager Test\Apps\Lathe, the
// shortcuts and ProgramData under <dir>, no verbs, no HKLM, no ie4uinit). Exit 0 done, 1 failed (why on stderr), 2 an
// argument it does not know. Run from main.cpp before anything else (before the window and the one-copy hand-off).
// Keep it in step with native/installer/lathe.nsi: the same keys, values and verbs.
#pragma once
#include <string>
#include <vector>

namespace lathe {

// -1: not a --setup / --uninstall launch (the app runs); otherwise the exit code
int setupCommand(const std::vector<std::string>& args);

}  // namespace lathe
