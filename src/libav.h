#pragma once
#include <string>

namespace lathe {

// Probe: returns the linked libav versions + the build's license posture, or a
// "not built in" stub when the LGPL dev package wasn't present at build time.
// Proves the dynamic link works before we build the persistent decoder on it.
std::string libav_versions();

}
