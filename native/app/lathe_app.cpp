#include "lathe_app.h"
#include "app_icon.h"
#include "look.h"
#include "skin.h"
#include <algorithm>
#include <filesystem>
#include <thread>
#include <vpf/gui/editor.h>

namespace lathe {

namespace fs = std::filesystem;
using vpf::gui::Color;
using vpf::gui::Paint;
using vpf::gui::Rect;
using wdn::Style;
using wdn::skin;

LatheApp::LatheApp() {
    name = "lathe-app";
    ownTips = true;
    escapeClears = true;
}

bool LatheApp::drawTitleIcon(const Paint& p, int cx, int cy, int px) const { return drawAppIcon(p, cx, cy, px); }

void LatheApp::exportMove(const std::vector<std::string>& paths, const std::string& dir) {
    std::shared_ptr<vpf::gui::UiQueue> q = editor() ? editor()->uiQueue() : nullptr;
    std::weak_ptr<bool> alive = alive_;
    std::thread([this, q, alive, paths, dir] {
        const std::vector<std::string> landed = moveTo(paths, dir);
        if (!q) return;
        q->post([this, alive, paths, landed] {
            if (alive.expired()) return;
            for (size_t i = 0; i < paths.size() && i < landed.size(); ++i) exported(paths[i], landed[i]);
        });
    }).detach();
}

// the title strip is the window's frame: a press on it (not on a button) moves the window, as the Tauri drag region did
bool LatheApp::onMouseDown(const vpf::gui::MouseEvent& e) {
    if (e.y < kTitle && hit(e.x, e.y).empty() && editor()) { editor()->beginWindowDrag(); return true; }
    return LatheWindow::onMouseDown(e);
}

// right to left, left of the X: Minimize, the look, About (the Tauri status bar's info button, here beside the look)
void LatheApp::titleExtras(std::vector<El>& /*out*/, int& right) {
    const Rect all{0, 0, bounds.w, bounds.h};
    const Style& t = skin().style("tool.titlebar");
    const int h = kTitle;
    {
        const Rect r = wdn::look::stripButton({right - 28, 0, 28, h}, bounds.w, 1, h);
        right = r.x;
        add(r, all, "app:minimize", true, [t](const Paint& p, const Rect& rr, bool hot) {
            if (wdn::look::spriteSquare(p, rr, "win:minimize", hot, false, t.mark)
                || wdn::look::squareButton(p, rr.x + rr.w / 2.0, rr.y + rr.h / 2.0, wdn::look::Square::Minimize, hot, t.ink, t.fill)) return;
            if (hot) p.canvas.fillRect(p.dev({rr.x + 7, rr.y + 7, 14, 14}), skin().style("tool.button", wdn::SkinState::Hover).fill);
            p.canvas.fillRect(p.dev({rr.x + rr.w / 2 - 4, rr.y + rr.h / 2, 9, 1}), hot ? t.ink : t.soft);
        });
    }
    auto textButton = [&](const std::string& id, const std::string& text) {
        const int w = measure(text, 8, false, 0.6f) + 14;
        right -= w + 2;
        const int bh = std::min<int>(18, h);
        add({right, (h - bh) / 2, w, bh}, all, id, true, [t, text](const Paint& p, const Rect& rr, bool hot) {
            if (hot) p.canvas.fillRect(p.dev(rr), skin().style("tool.button", wdn::SkinState::Hover).fill);
            const wdn::look::TextScope ts(8, false, false, 0.6f);
            wdn::look::text(p, rr.x + 7, wdn::look::baselineIn(p, rr.y, rr.h, true), text, hot ? t.ink : t.soft);
        });
    };
    std::string lookLabel = skinName;
    for (const auto& c : wdn::skinChoices()) if (c.first == skinName) lookLabel = c.second;
    textButton("app:skin", upper(lookLabel));
    textButton("app:about", "ABOUT");
}

void LatheApp::act(const std::string& id, const vpf::gui::MouseEvent& e) {
    if (id == "app:minimize") { if (editor()) editor()->minimizeWindow(); return; }
    if (id == "app:about") { if (onAbout) onAbout(); return; }
    if (id == "app:skin") {
        std::vector<Opt> opts;
        for (const auto& c : wdn::skinChoices()) opts.push_back({c.first, c.second});
        openSelectList(id, opts, skinName, [this](const std::string& v) { if (v != skinName && onSkin) onSkin(v); });
        return;
    }
    LatheWindow::act(id, e);
}

std::vector<std::string> LatheApp::moveTo(const std::vector<std::string>& paths, const std::string& dir) {
    std::vector<std::string> out;
    for (const std::string& src : paths) {
        const fs::path s = fs::u8path(src), d = fs::u8path(dir);
        const std::string name = s.filename().u8string();
        if (name.empty()) { out.emplace_back(); continue; }
        const std::string stem = s.stem().u8string(), ext = s.extension().u8string();
        std::error_code ec;
        fs::path dst = d / s.filename();
        for (int n = 2; fs::exists(dst, ec) && n <= 9999; ++n) dst = d / fs::u8path(stem + " (" + std::to_string(n) + ")" + ext);
        if (fs::equivalent(s, dst, ec)) { out.push_back(src); continue; }
        fs::rename(s, dst, ec);
        if (ec) {
            ec.clear();
            fs::copy_file(s, dst, fs::copy_options::none, ec);
            if (!ec) fs::remove(s, ec);
            if (ec) { out.emplace_back(); continue; }
        }
        std::string landed = dst.generic_u8string();
        out.push_back(landed);
    }
    return out;
}

}  // namespace lathe
