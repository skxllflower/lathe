// Lathe's native GUI: the converter window (lathe_app.h) on vpf's standalone runner, replacing the Tauri GUI (gui/).
//
//   lathe-gui [<file> ...]                       files to convert (an Explorer "Open with", a drop on the exe or the Dock)
//             [--convert <file> --format <fmt>]   the Explorer "Convert with Lathe" verb: the file in, that format picked
//             [--skin dev|vx|focused|zen-dark]    the look for this run (Settings keeps the last one picked)
// and the runner's own flags (--screenshot, --steps, --dump-tree, --size, --scale ...: vacant-labs vpf/CLAUDE.md).
//
// One Lathe at a time (Tauri's single-instance plugin, Windows): a second launch hands its arguments to the running
// window (which comes to the front and takes the files) and exits before anything opens. The lathe CLI does every
// conversion (lathe_window.h: `lathe convert` a file, three at once); this process never converts anything itself.
#include "about_window.h"
#include "data_root.h"
#include "dev_run.h"
#include "dialog_window.h"
#include "external_tools.h"
#include "lathe_app.h"
#include "look.h"
#include "settings_store.h"
#include "setup_cli.h"
#include "skin.h"
#include "ui_state.h"
#include "window_place.h"
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <vpf/gui/editor.h>
#include <vpf/plugin_entry.h>
#include <vpf/preset.h>
#if defined(_WIN32)
#include <cstdio>
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

using vpf::gui::Editor;

bool headless() { return vpf::hasStandaloneArg("--steps") || vpf::hasStandaloneArg("--screenshot") || vpf::hasStandaloneArg("--dump-tree"); }

#if defined(_WIN32)
// a GUI-subsystem exe (no console window); a run from a terminal (the headless flags, the checks) writes to it instead.
// Redirected handles (a pipe, a file) are inherited as they are and left alone
const bool g_parentConsole = [] {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if ((out && out != INVALID_HANDLE_VALUE) || !AttachConsole(ATTACH_PARENT_PROCESS)) return false;
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    return true;
}();

// ---- one Lathe at a time: a named mutex, and a message-only window the second launch sends its arguments to ----
const wchar_t* const kMutex = L"Local\\VacantSystems.Lathe.Native";
const wchar_t* const kHandoffClass = L"VacantSystems.Lathe.Handoff";
std::function<void(std::vector<std::string>)> g_onLaunch;   // (the UI thread's: set once the window is built)
std::vector<std::vector<std::string>> g_early;               // (launches that came before it)

std::vector<std::string> ownArgs() {
    std::vector<std::string> out;
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 1; w && i < n; ++i) out.push_back(std::filesystem::path(w[i]).u8string());
    if (w) LocalFree(w);
    return out;
}

LRESULT CALLBACK handoffProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COPYDATA) {
        const auto* cd = reinterpret_cast<const COPYDATASTRUCT*>(lp);
        std::vector<std::string> args;
        const std::string all(static_cast<const char*>(cd->lpData), cd->cbData);
        for (size_t a = 0; a <= all.size();) {   // ('\0' between the arguments)
            const size_t b = all.find('\0', a);
            const std::string s = all.substr(a, b == std::string::npos ? std::string::npos : b - a);
            if (!s.empty()) args.push_back(s);
            if (b == std::string::npos) break;
            a = b + 1;
        }
        if (g_onLaunch) g_onLaunch(args); else g_early.push_back(args);
        return TRUE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// (before main) false: another Lathe has this launch's arguments, and this process exits
const bool g_sole = [] {
    // (the manager's --setup / --uninstall: no hand-off and no mutex, which --uninstall reads as "Lathe is running"; they
    // run from latheDef)
    if (lathe::setupRequested()) return true;
    // (vpf has not parsed the arguments yet: before main, the command line itself)
    for (const std::string& a : ownArgs()) if (a == "--steps" || a == "--screenshot" || a == "--dump-tree") return true;
    CreateMutexW(nullptr, TRUE, kMutex);
    if (GetLastError() != ERROR_ALREADY_EXISTS) return true;
    for (int tries = 0; tries < 40; ++tries) {   // (the first launch may still be starting: up to 2 s for its window)
        if (HWND to = FindWindowExW(HWND_MESSAGE, nullptr, kHandoffClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(to, &pid);
            AllowSetForegroundWindow(pid);   // (it may come to the front: this launch is the user's)
            std::string payload;
            for (const std::string& a : ownArgs()) { payload += a; payload += '\0'; }
            COPYDATASTRUCT cd{1, (DWORD)payload.size(), payload.data()};
            DWORD_PTR r = 0;
            SendMessageTimeoutW(to, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 3000, &r);
            std::_Exit(0);
        }
        Sleep(50);
    }
    return true;   // (no window answered: a stale mutex of a hung one; run anyway)
}();

void makeHandoffWindow() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = handoffProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kHandoffClass;
    RegisterClassW(&wc);
    CreateWindowExW(0, kHandoffClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
}

// the main window brought to the front (the plugin's unminimize / show / set_focus): this thread's visible top level
void focusMainWindow() {
    EnumThreadWindows(GetCurrentThreadId(), [](HWND h, LPARAM) -> BOOL {
        if (!IsWindowVisible(h) && !IsIconic(h)) return TRUE;
        if (GetWindow(h, GW_OWNER)) return TRUE;   // (a popup's)
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        SetForegroundWindow(h);
        return FALSE;
    }, 0);
}
#endif

// Tauri's parse_convert_arg: --convert <file> and --format <fmt> (both, or nothing)
bool convertArg(const std::vector<std::string>& args, std::string& input, std::string& format) {
    input.clear(); format.clear();
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--convert") input = args[i + 1];
        else if (args[i] == "--format") format = args[i + 1];
    }
    return !input.empty() && !format.empty();
}

// the files a launch names (not flags, not a flag's value)
std::vector<std::string> fileArgs(const std::vector<std::string>& args) {
    std::vector<std::string> out;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i].rfind("--", 0) == 0) { if (args[i] == "--convert" || args[i] == "--format" || args[i] == "--skin") ++i; continue; }
        out.push_back(args[i]);
    }
    return out;
}

void applySkin(const std::string& name) {
    wdn::setSkin(wdn::makeSkin(name));
    wdn::look::setTextScale(1.0f);
}

Editor* createEditor(vpf::gui::EditorHost& host) {
    wdn::dev::dropBinaryOverrides();   // (a packaged run: LATHE_EXE and the rest never reach the children)
    const bool quiet = headless();
    lathe::setWritable(!quiet);
    wdn::ui_state::setEnabled(!quiet);   // (the checks start from the defaults and leave nothing behind)
    wdn::ext::setSharedWritable(!quiet);

    // this app's own CLI: in a dev run through LATHE_EXE (the tiers' env override, dev runs only); installed, through
    // the shared registry it registers into (as the Tauri app did at setup: WAVdesk finds this install there too)
    const std::string core = lathe::ownCore();
    if (!core.empty() && wdn::dev::devRun()) {
#if defined(_WIN32)
        if (!_wgetenv(L"LATHE_EXE")) _wputenv_s(L"LATHE_EXE", std::filesystem::u8path(core).wstring().c_str());
#else
        setenv("LATHE_EXE", core.c_str(), 0);
#endif
    } else if (!core.empty()) {
        lathe::registerSelf(core);
    }
    lathe::importTauriSettings();   // (once: the Tauri app's format, destination and presets, before the window reads them)

    auto* ed = new Editor(host, lathe::LatheApp::kWidth, lathe::LatheApp::kHeight);
    ed->setResizable(lathe::LatheApp::kWidth, lathe::LatheApp::kHeight);   // (tauri.conf: resizable, never smaller than its first size)
    ed->setCustomFrame(Editor::customFrameSupported());   // (the title strip is the window's: decorations false)
    ed->setCornerStyle(vpf::gui::CornerStyle::Round);
    ed->setExactEdges(true);
    ed->setWindowTitle("Lathe");

    std::string skinName = vpf::standaloneArgValue("--skin");
    if (skinName.empty()) skinName = wdn::settings::text("skin", "dev");
    applySkin(skinName);

    auto* app = ed->root().make<lathe::LatheApp>();
    app->skinName = skinName;
    app->bounds = {0, 0, lathe::LatheApp::kWidth, lathe::LatheApp::kHeight};
    app->onBoundsChanged();
    ed->onResize = [app](int w, int h) { app->bounds = {0, 0, w, h}; app->onBoundsChanged(); };
    ed->setFocus(app);

    // the close (the X, Alt+F4, Cmd+Q): asked first while converting, as the Tauri onCloseRequested did
    ed->onCloseRequest = [ed, app] {
        if (!app->running()) return true;
        wdn::askConfirm(*ed, "Stop converting?", "Closing Lathe stops the conversions in progress.", "Stop and Close", [ed] { ed->closeStandaloneWindow(); });
        return false;
    };
    app->onClose = [ed] { if (ed->closeRequested()) ed->closeStandaloneWindow(); };

    // Export to...: the move on a worker (tools.rs fs_move); the window's rows follow where the files landed
    app->onExport = [app](const std::vector<std::string>& paths, const std::string& dir) { app->exportMove(paths, dir); };

    // About: one window, raised when open
    auto about = std::make_shared<lathe::AboutWindow*>(nullptr);
    app->onAbout = [ed, about] {
        if (*about && wdn::liveWindow(*ed, *about)) { ed->raisePopup(*about); return; }
        auto w = std::make_unique<lathe::AboutWindow>();
        lathe::AboutWindow* raw = w.get();
        *about = raw;
        raw->bounds = {(lathe::LatheApp::kWidth - lathe::AboutWindow::kWidth) / 2, 0, lathe::AboutWindow::kWidth, lathe::AboutWindow::kHeight};
        raw->onBoundsChanged();
        raw->onClose = [ed, raw, about] { if (wdn::liveWindow(*ed, raw)) ed->closePopup(raw); *about = nullptr; };
        vpf::gui::PopupOptions o;
        o.window = true;
        o.detached = true;
        o.activates = true;
        o.corners = vpf::gui::CornerStyle::Round;
        wdn::openWindow(*ed, std::move(w), o);
        ed->setFocus(raw);
    };

    // another look: applied at once, kept in settings.json
    app->onSkin = [app, about, ed](const std::string& name) {
        applySkin(name);
        wdn::settings::set("skin", name);
        app->skinName = name;
        app->onBoundsChanged();
        if (*about && wdn::liveWindow(*ed, *about)) (*about)->onBoundsChanged();
        ed->requestRepaint();
    };

    // the binary's state (tool_binary_probe), off the UI thread: Lathe's capability probe spawns it once (born inside
    // the kill-on-close job, win_spawn.h, so a run that exits mid-probe takes it along)
    {
        std::shared_ptr<vpf::gui::UiQueue> q = ed->uiQueue();
        std::thread([q, app, core] {
            wdn::ext::ToolStatus st = wdn::ext::probeTool("lathe");
            // (the window shows "configured" in the ok tone, any other tier as a fallback: this app's own core is no
            // fallback, whichever tier found it)
            if (st.resolved && !core.empty() && std::filesystem::u8path(st.path) == std::filesystem::u8path(core)) st.source = "configured";
            q->post([app, st] { app->setStatus(st); });
        }).detach();
    }

    // what a launch hands over: the convert verb's file and format, or files to add
    auto takeLaunch = [app](const std::vector<std::string>& args) {
        std::string input, format;
        if (convertArg(args, input, format)) app->seed({input}, format);
        const std::vector<std::string> files = fileArgs(args);
        if (!files.empty()) app->seed(files, "");
    };
    ed->onOpenFiles = [app](const std::vector<std::string>& paths) { app->seed(paths, ""); };   // (the runner's document arguments, the Dock)
    {
        std::string input, format;
        if (convertArg(vpf::standaloneArgs(), input, format)) app->seed({input}, format);
    }
#if defined(_WIN32)
    if (!quiet) {
        makeHandoffWindow();
        std::shared_ptr<vpf::gui::UiQueue> q = ed->uiQueue();
        g_onLaunch = [q, takeLaunch](std::vector<std::string> args) {
            q->post([takeLaunch, args] { focusMainWindow(); takeLaunch(args); });
        };
        for (auto& a : g_early) g_onLaunch(a);
        g_early.clear();
    }
#else
    (void)takeLaunch;
#endif
    return ed;
}

// vpf's main asks for the definition first, before it reads a flag or builds anything: the manager's --setup /
// --uninstall (setup_cli.h) run there, after every static initializer (never from one: a file's globals may not be built
// yet), with no window; the process exits with its code
const vpf::PluginDef& latheDef() {
#if defined(_WIN32)
    if (lathe::setupRequested()) {
        const int code = lathe::setupCommand(ownArgs());
        std::fflush(stdout);
        std::fflush(stderr);
        std::_Exit(code < 0 ? 2 : code);
    }
#endif
    static vpf::PluginDef d = [] {
        vpf::PluginDef p;
        p.info.id = "com.vacantsystems.lathe";
        p.info.name = "Lathe";
        p.createEditor = &createEditor;
        return p;
    }();
    return d;
}

}  // namespace

VPF_PLUGIN(latheDef)
