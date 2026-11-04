#include "settings_window.h"
#include "look.h"
#include "skin.h"
#include <algorithm>

namespace lathe {

using vpf::gui::Rect;
using wdn::skin;

bool SettingsWindow::onKey(const vpf::gui::KeyEvent& e) {
    if (e.key == vpf::gui::Key::Escape) { if (onClose) onClose(); return true; }
    return false;
}

// Latch's Settings layout (its section heading and rule, 16 px in), one section: Look
void SettingsWindow::layout() {
    const int W = bounds.w > 0 ? bounds.w : kWidth;
    const vpf::gui::Color line = skin().style("tool.footer").line, muted = skin().style("tool.label").ink;
    const int x = 16, fw = W - 32;
    const Rect body{0, (int)kTitle, W, (bounds.h > 0 ? bounds.h : kHeight) - (int)kTitle};
    int y = body.y + 14;
    label(body, x, y, "Look", 8, muted, true, true);
    y += 13;
    add({x, y, fw, 1}, body, "", true, [line](const vpf::gui::Paint& p, const Rect& r, bool) { p.canvas.fillRect(p.dev(r), line); });
    y += 10;
    std::vector<Opt> looks;
    for (const auto& l : wdn::skinChoices()) looks.push_back({l.first, l.second});
    select(body, {x, y, std::min(fw, 200), 20}, "select:skin", looks, skinName, true);
}

void SettingsWindow::act(const std::string& id, const vpf::gui::MouseEvent&) {
    if (id == "close") { if (onClose) onClose(); return; }
    if (id == "select:skin") {
        if (!selectOpts_.count(id)) return;
        openSelectList(id, selectOpts_[id], skinName, [this](const std::string& v) { if (v != skinName && onSkin) onSkin(v); });
    }
}

}  // namespace lathe
