// About Lathe (AboutApp.tsx, 420 x 500): the app's name, maker and version, then the third-party notices Lathe must
// show (the bundled LGPL libav and its source offer, the downloaded ffmpeg program, LibRaw, libjpeg-turbo), their
// links opened in the browser. The FFmpeg version and archive URL are in lockstep with
// tools/build-ffmpeg-lgpl-mac.sh, THIRD_PARTY_NOTICES.txt and gui/src/AboutApp.tsx: bump all four together.
#pragma once
#include "ext_window.h"

namespace lathe {

class AboutWindow : public wdn::ExtWindow {
public:
    static constexpr int kWidth = 420, kHeight = 500;
    AboutWindow() { name = "lathe-about"; }
    bool onKey(const vpf::gui::KeyEvent& e) override;
    bool wantsShortcuts() const override { return true; }
    static bool openUrl(const std::string& url);   // (the system's browser)

protected:
    std::string titleText() const override { return "About"; }
    std::string titleIcon() const override { return "info"; }
    void layout() override;
    void act(const std::string& id, const vpf::gui::MouseEvent& e) override;
};

}  // namespace lathe
