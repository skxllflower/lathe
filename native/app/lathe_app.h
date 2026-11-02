// The standalone Lathe: WAVdesk's native Lathe window (lathe_window.h, shared, unchanged) as the whole of an app's
// window instead of a popup over WAVdesk's main one. What it adds is what the Tauri ConvertApp's own frame had:
//   the title strip as the window's frame (dragged to move it), Minimize beside the X, the look chooser (the skins:
//   Dev look, VX, Focused, Zen Dark) and About (about_window.h)
//   Export to...: the move itself (tools.rs fs_move: a free name in the folder, rename else copy + delete)
//   Escape: nothing (in WAVdesk it closes the tool window; here it would quit the app)
#pragma once
#include "lathe_window.h"
#include <functional>
#include <string>

namespace lathe {

class LatheApp : public wdn::LatheWindow {
public:
    LatheApp();
    std::function<void()> onAbout;
    std::function<void(const std::string& skin)> onSkin;   // (a look picked: the app applies it and keeps it)
    std::string skinName = "dev";

    bool onMouseDown(const vpf::gui::MouseEvent& e) override;
    bool onKey(const vpf::gui::KeyEvent& e) override;

    // tools.rs fs_move: each file to a free name in `dir` (" (2)" ...), renamed, else copied and the source deleted;
    // blocking (a worker thread's). Returns where each landed ("" = failed)
    static std::vector<std::string> moveTo(const std::vector<std::string>& paths, const std::string& dir);

protected:
    void titleExtras(std::vector<El>& out, int& right) override;
    void act(const std::string& id, const vpf::gui::MouseEvent& e) override;
};

}  // namespace lathe
