// Settings: the look (the skins: wdn::skinChoices), here rather than on the title strip (the owner, 2026-11-04: a small
// Settings window, as Latch's, whose Look row it shares). Lathe's other choices (format, destination, presets) stay in
// the main window, where Tauri had them; the window grows when Lathe has more to set.
#pragma once
#include "ext_window.h"
#include <functional>
#include <string>

namespace lathe {

class SettingsWindow : public wdn::ExtWindow {
public:
    static constexpr int kWidth = 360, kHeight = 128;
    SettingsWindow() { name = "lathe-settings"; }
    bool onKey(const vpf::gui::KeyEvent& e) override;
    bool wantsShortcuts() const override { return true; }
    // the look in use, and a pick handed to the app, which applies it and keeps it
    std::string skinName = "dev";
    std::function<void(const std::string& skin)> onSkin;

protected:
    std::string titleText() const override { return "Settings"; }
    std::string titleIcon() const override { return "settings"; }
    void layout() override;
    void act(const std::string& id, const vpf::gui::MouseEvent& e) override;
};

}  // namespace lathe
