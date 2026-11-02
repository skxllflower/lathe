// The app's own icon picture (app_icon.cpp), for the title strip and About
#pragma once
#include <vpf/gui/widget.h>

namespace lathe {

// centred at cx, cy (logical), px logical pixels square; false when the picture is not there
bool drawAppIcon(const vpf::gui::Paint& p, int cx, int cy, int px);

}  // namespace lathe
