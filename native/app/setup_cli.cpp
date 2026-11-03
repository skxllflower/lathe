// --setup / --uninstall (setup_cli.h): lathe.nsi's Install and Uninstall sections, done by the app for the manager.
#include "setup_cli.h"
#include "data_root.h"

#if !defined(_WIN32)
namespace lathe {
bool setupRequested() { return false; }
int setupCommand(const std::vector<std::string>&) { return -1; }   // (the manager's apps are Windows first)
}  // namespace lathe
#else
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <filesystem>

namespace lathe {

namespace {

namespace fs = std::filesystem;

// (lathe.nsi's defines. Every name a compile-time constant, never a std::wstring global: WAVdesk once ran its setup from
// a static initializer before its string key globals were built, and the empty keys opened the HKLM root itself)
const wchar_t kProduct[] = L"Lathe";
const wchar_t kPublisher[] = L"Vacant Systems";
const wchar_t kMainExe[] = L"lathe-gui.exe";
const wchar_t kUninstKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Lathe";
const wchar_t kTestKey[] = L"Software\\Vacant Systems\\Manager Test\\Apps\\Lathe";
const wchar_t kDirKey[] = L"Software\\vacantsystems\\Lathe";
const wchar_t kDirKeyParent[] = L"Software\\vacantsystems";
const wchar_t kVerbKey[] = L"Software\\Classes\\*\\shell\\LatheConvert";
const wchar_t kSubKey[] = L"Software\\Classes\\Lathe.Convert";
const wchar_t kTestVerbs[] = L"Software\\Vacant Systems\\Manager Test\\Verbs\\Lathe";   // (a test root's verbs, under HKCU)
const wchar_t kTestVerbsParent[] = L"Software\\Vacant Systems\\Manager Test\\Verbs";
const wchar_t kMutex[] = L"Local\\VacantSystems.Lathe.Native";   // (main.cpp's one-copy mutex: Lathe is running)

// the "Convert with Lathe" cascade (lathe.nsi CONVERT_VERB): the key orders the submenu, the format is --format's
struct Verb { const wchar_t* key; const wchar_t* label; const wchar_t* fmt; };
const Verb kVerbs[] = {
    {L"a1wav", L"WAV", L"wav"}, {L"a2mp3", L"MP3", L"mp3"}, {L"a3flac", L"FLAC", L"flac"}, {L"a4aac", L"AAC", L"aac"},
    {L"a5m4a", L"M4A", L"m4a"}, {L"b1mp4", L"MP4", L"mp4"}, {L"b2mkv", L"MKV", L"mkv"}, {L"b3mov", L"MOV", L"mov"},
    {L"b4webm", L"WEBM", L"webm"}, {L"c1png", L"PNG", L"png"}, {L"c2jpg", L"JPG", L"jpg"}, {L"c3webp", L"WEBP", L"webp"},
    {L"c4gif", L"GIF", L"gif"}};

fs::path known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = p;
    CoTaskMemFree(p);
    return out;
}

void say(const std::string& s) { std::fprintf(stderr, "lathe: %s\n", s.c_str()); std::fflush(stderr); }

std::wstring lower(std::wstring s) { for (wchar_t& c : s) c = (wchar_t)std::towlower(c); return s; }

// where things go: the real machine, or under a test root (setup_cli.h)
struct Where {
    fs::path exe, dir, root;
    bool test() const { return !root.empty(); }
    fs::path programData() const { return test() ? root / L"ProgramData" : known(FOLDERID_ProgramData); }
    fs::path localAppData() const { return test() ? root / L"LocalAppData" : known(FOLDERID_LocalAppData); }
    fs::path startMenu() const { return (test() ? root / L"StartMenu" : known(FOLDERID_CommonPrograms)) / L"Vacant Systems"; }
    fs::path desktop() const { return test() ? root / L"Desktop" : known(FOLDERID_PublicDesktop); }
    fs::path shared() const { return programData() / L"Vacant Systems" / L"Shared"; }
    fs::path core() const { return dir / L"coredist" / L"lathe.exe"; }
    HKEY entryHive() const { return test() ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE; }
    const wchar_t* entryKey() const { return test() ? kTestKey : kUninstKey; }
    // Explorer's verbs: HKLM\Software\Classes, a test root's at the same paths under HKCU kTestVerbs
    HKEY verbHive() const { return test() ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE; }
    std::wstring verbKey(const wchar_t* key) const { return test() ? std::wstring(kTestVerbs) + L"\\" + key : std::wstring(key); }
};

// ---- the registry (a 64-bit process: HKLM's 64-bit view, the installer's SetRegView 64) ----

bool setStr(HKEY k, const wchar_t* name, const std::wstring& v) {
    return RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}
bool setDword(HKEY k, const wchar_t* name, DWORD v) { return RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof v) == ERROR_SUCCESS; }

// a sub-key path that names a key under the hive, never the hive itself ("" or a leading backslash opens the root)
bool underHive(const std::wstring& path) { return path.size() > 9 && path[0] != L'\\' && path.find(L'\\') != std::wstring::npos; }

HKEY create(HKEY hive, const std::wstring& path) {
    if (!underHive(path)) { say("refused to write a registry key that names no key under the hive"); return nullptr; }
    HKEY k = nullptr;
    return RegCreateKeyExW(hive, path.c_str(), 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &k, nullptr) == ERROR_SUCCESS ? k : nullptr;
}

bool removeKey(HKEY hive, const std::wstring& path) {
    if (!underHive(path)) { say("refused to delete a registry key that names no key under the hive"); return false; }
    const LSTATUS r = RegDeleteTreeW(hive, path.c_str());
    if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND) return false;
    const LSTATUS r2 = RegDeleteKeyExW(hive, path.c_str(), KEY_WOW64_64KEY, 0);
    return r2 == ERROR_SUCCESS || r2 == ERROR_FILE_NOT_FOUND;
}

void removeKeyIfEmpty(HKEY hive, const std::wstring& path) {   // (DeleteRegKey /ifempty)
    if (!underHive(path)) return;
    HKEY k = nullptr;
    if (RegOpenKeyExW(hive, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return;
    DWORD subs = 0, values = 0;
    const bool empty = RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, &subs, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS
                    && subs == 0 && values == 0;
    RegCloseKey(k);
    if (empty) RegDeleteKeyExW(hive, path.c_str(), KEY_WOW64_64KEY, 0);
}

// ---- files ----

// a shortcut made anew (a kept .lnk keeps a stale cached icon)
bool shortcut(const fs::path& lnk, const fs::path& target) {
    std::error_code ec;
    fs::create_directories(lnk.parent_path(), ec);
    fs::remove(lnk, ec);
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(target.parent_path().c_str());
    sl->SetIconLocation(target.c_str(), 0);
    IPersistFile* pf = nullptr;
    bool ok = SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)));
    if (ok) { ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE)); pf->Release(); }
    sl->Release();
    return ok;
}

DWORD folderKB(const fs::path& dir) {
    unsigned long long n = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) n += it->file_size(e2);
    }
    return (DWORD)std::min<unsigned long long>(n / 1024, 0xFFFFFFFFull);
}

// a recursive delete only of a folder whose path ends with `suffix` (lathe.nsi SAFE_RMDIR_R)
void removeTree(const fs::path& p, const std::wstring& suffix) {
    const std::wstring s = lower(p.wstring()), x = lower(suffix);
    if (p.empty() || s.size() <= x.size() || s.compare(s.size() - x.size(), x.size(), x) != 0) return;
    std::error_code ec;
    fs::remove_all(p, ec);
}

void removeIfEmpty(const fs::path& p) { std::error_code ec; if (fs::is_empty(p, ec) && !ec) fs::remove(p, ec); }

// a program run to its end, no window (icacls, ie4uinit); its exit code, -1 when it did not start
int runQuiet(std::wstring cmd) {
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

void notifyShell() { SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr); }

// ---- --setup ----

bool setup(const Where& w) {
    bool ok = true;
    std::error_code ec;
    const fs::path shared = w.shared();
    fs::create_directories(shared, ec);
    if (!w.test())   // (writable by the unelevated apps: BUILTIN\Users by SID, inherited, Modify)
        if (runQuiet(L"icacls.exe \"" + shared.wstring() + L"\" /grant *S-1-5-32-545:(OI)(CI)M /T /Q") != 0) say("could not open the Shared folder to Users (icacls)");
    // Explorer's "Convert with Lathe" cascade on any file (a test root's under its own key, so a check runs this path too)
    {
        bool verbs = true;
        if (HKEY k = create(w.verbHive(), w.verbKey(kVerbKey))) {
            verbs &= setStr(k, L"MUIVerb", L"Convert with Lathe") && setStr(k, L"Icon", w.exe.wstring()) && setStr(k, L"ExtendedSubCommandsKey", L"Lathe.Convert");
            RegCloseKey(k);
        } else verbs = false;
        for (const Verb& v : kVerbs) {
            const std::wstring base = w.verbKey(kSubKey) + L"\\shell\\" + v.key;
            HKEY k = create(w.verbHive(), base), c = create(w.verbHive(), base + L"\\command");
            verbs &= k && c && setStr(k, L"MUIVerb", v.label) && setStr(c, nullptr, L"\"" + w.exe.wstring() + L"\" --convert \"%1\" --format " + v.fmt);
            if (k) RegCloseKey(k);
            if (c) RegCloseKey(c);
        }
        if (!verbs) { ok = false; say("could not write Explorer's Convert with Lathe verbs (Software\\Classes)"); }
    }
    if (!w.test()) {
        if (HKEY k = create(HKEY_LOCAL_MACHINE, kDirKey)) { ok &= setStr(k, nullptr, w.dir.wstring()); RegCloseKey(k); }
        else { ok = false; say("could not write HKLM\\Software\\vacantsystems\\Lathe"); }
    }
    const fs::path name = std::wstring(kProduct) + L".lnk";
    if (!shortcut(w.startMenu() / name, w.exe) || !shortcut(w.desktop() / name, w.exe)) { ok = false; say("could not make the shortcuts"); }
    // the uninstall entry (lathe.nsi's values; the program uninstalls itself)
    if (HKEY k = create(w.entryHive(), w.entryKey())) {
        const std::wstring exeQ = L"\"" + w.exe.wstring() + L"\"";
        const bool e = setStr(k, L"DisplayName", kProduct) && setStr(k, L"DisplayIcon", exeQ) && setStr(k, L"DisplayVersion", L"" LATHE_VERSION)
                    && setStr(k, L"Publisher", kPublisher) && setStr(k, L"InstallLocation", L"\"" + w.dir.wstring() + L"\"")
                    && setStr(k, L"UninstallString", exeQ + L" --uninstall") && setStr(k, L"QuietUninstallString", exeQ + L" --uninstall --quiet")
                    && setStr(k, L"MainBinaryName", kMainExe) && setDword(k, L"NoModify", 1) && setDword(k, L"NoRepair", 1)
                    && setDword(k, L"EstimatedSize", folderKB(w.dir));
        RegCloseKey(k);
        if (!e) { ok = false; say("could not write the uninstall entry"); }
    } else { ok = false; say("could not create the uninstall entry (not elevated?)"); }
    // the CLI registration WAVdesk finds this install's lathe through, before the first launch
    if (fs::is_regular_file(w.core(), ec)) registerSelfIn(shared.generic_u8string(), w.core().generic_u8string());
    if (!w.test()) {
        runQuiet(L"ie4uinit.exe -show");
        notifyShell();
    }
    return ok;
}

// ---- --uninstall ----

// what --setup made, gone (the folder, the user's data and Shared stay)
bool unsetup(const Where& w) {
    bool ok = true;
    if (!removeKey(w.entryHive(), w.entryKey())) { ok = false; say("could not remove the uninstall entry"); }
    std::error_code ec;
    const fs::path name = std::wstring(kProduct) + L".lnk";
    fs::remove(w.startMenu() / name, ec);
    removeIfEmpty(w.startMenu());
    fs::remove(w.desktop() / name, ec);
    if (!removeKey(w.verbHive(), w.verbKey(kVerbKey)) || !removeKey(w.verbHive(), w.verbKey(kSubKey))) { ok = false; say("could not remove Explorer's verbs"); }
    if (w.test()) {
        removeKey(HKEY_CURRENT_USER, kTestVerbs);
        removeKeyIfEmpty(HKEY_CURRENT_USER, kTestVerbsParent);
    } else {
        removeKey(HKEY_LOCAL_MACHINE, kDirKey);
        removeKeyIfEmpty(HKEY_LOCAL_MACHINE, kDirKeyParent);
    }
    unregisterSelfIn(w.shared().generic_u8string(), w.core().generic_u8string());
    if (!w.test()) notifyShell();
    return ok;
}

bool elevated() {
    HANDLE t = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    const bool yes = GetTokenInformation(t, TokenElevation, &e, sizeof e, &n) && e.TokenIsElevated;
    CloseHandle(t);
    return yes;
}

// the same launch again, elevated ("runas"), waited for: its exit code (1 when refused)
int relaunchElevated(const Where& w, const std::wstring& params) {
    SHELLEXECUTEINFOW sx{};
    sx.cbSize = sizeof sx;
    sx.fMask = SEE_MASK_NOCLOSEPROCESS;
    sx.lpVerb = L"runas";
    sx.lpFile = w.exe.c_str();
    sx.lpParameters = params.c_str();
    sx.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sx) || !sx.hProcess) { say("not elevated, and the elevation was refused"); return 1; }
    WaitForSingleObject(sx.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sx.hProcess, &code);
    CloseHandle(sx.hProcess);
    return (int)code;
}

// Lathe (another copy) or a conversion (lathe.exe: Lathe's, or WAVdesk using it) running: lathe.nsi WAIT_UNLOCKED
std::wstring runningWhat(const Where& w) {
    if (HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, kMutex)) { CloseHandle(m); return L"Lathe"; }
    const HANDLE f = CreateFileW(w.core().c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) { CloseHandle(f); return {}; }
    return GetLastError() == ERROR_SHARING_VIOLATION ? L"A Lathe conversion (Lathe, or WAVdesk using it)" : L"";
}

// the folder removed once this process has gone (the manager's setup.cpp removeAfterExit), then its parent if empty;
// only a folder named ...\Vacant Systems\Lathe (never a build tree a dev copy runs from)
void removeAfterExit(const fs::path& dir) {
    const std::wstring s = lower(dir.wstring()), x = L"\\vacant systems\\lathe";
    if (s.size() <= x.size() || s.compare(s.size() - x.size(), x.size(), x) != 0) return;
    std::wstring cmd = L"cmd.exe /c ping -n 4 127.0.0.1 >nul & rmdir /s /q \"" + dir.wstring() + L"\" & rmdir \"" + dir.parent_path().wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    const std::wstring cwd = known(FOLDERID_Windows).wstring();
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, cwd.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// Windows' installed apps (UninstallString / QuietUninstallString): lathe.nsi's uninstaller, in the app
int uninstall(const Where& w, bool quiet, const std::wstring& params) {
    if (!w.test() && !elevated()) return relaunchElevated(w, params);
    if (!quiet && MessageBoxW(nullptr, L"Remove Lathe from this computer?", L"Uninstall Lathe", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return 1;
    for (std::wstring what = runningWhat(w); !what.empty(); what = runningWhat(w)) {
        if (quiet) { say(fs::path(what).u8string() + " is running"); return 1; }
        if (MessageBoxW(nullptr, (what + L" is running. Quit it, then click Retry.").c_str(), L"Uninstall Lathe", MB_RETRYCANCEL | MB_ICONEXCLAMATION) != IDRETRY) return 1;
    }
    const bool deleteData = !quiet && MessageBoxW(nullptr,
        L"Also delete Lathe's settings and presets?\n\nYour media files and converted outputs are never touched. Click No to keep the settings for a later install.",
        L"Uninstall Lathe", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
    const bool ok = unsetup(w);
    // the last Vacant Systems app out takes the suite's Shared folders with it (never on --quiet: the contract keeps them)
    const fs::path suite = w.dir.parent_path();
    std::error_code ec;
    const bool last = !fs::exists(suite / L"WAVdesk" / L"wavdesk.exe", ec) && !fs::exists(suite / L"Latch" / L"latch-gui.exe", ec);
    const fs::path pd = w.programData() / L"Vacant Systems", la = w.localAppData();
    removeTree(pd / L"Lathe", L"\\Vacant Systems\\Lathe");
    if (last && !quiet) removeTree(pd / L"Shared", L"\\Vacant Systems\\Shared");
    removeIfEmpty(pd);
    removeTree(la / L"com.vacantsystems.lathe", L"\\com.vacantsystems.lathe");   // (the Tauri app's WebView2 profile)
    if (deleteData) {
        removeTree(la / L"Vacant Systems" / L"Lathe", L"\\Vacant Systems\\Lathe");
        if (last) removeTree(la / L"Vacant Systems" / L"Shared", L"\\Vacant Systems\\Shared");
        removeIfEmpty(la / L"Vacant Systems");
    }
    removeAfterExit(w.dir);
    return ok ? 0 : 1;
}

}  // namespace

bool setupRequested() {
    int n = 0;
    LPWSTR* a = CommandLineToArgvW(GetCommandLineW(), &n);
    bool yes = false;
    for (int i = 1; a && i < n; ++i) yes |= std::wcscmp(a[i], L"--setup") == 0 || std::wcscmp(a[i], L"--uninstall") == 0;
    if (a) LocalFree(a);
    return yes;
}

int setupCommand(const std::vector<std::string>& args) {
    bool setupArg = false, uninstallArg = false;
    for (const std::string& a : args) { setupArg |= a == "--setup"; uninstallArg |= a == "--uninstall"; }
    if (!setupArg && !uninstallArg) return -1;
    bool quiet = false, keep = false;
    Where w;
    std::wstring params;   // (for the elevated relaunch)
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--test-root" && i + 1 < args.size()) {
            w.root = fs::absolute(fs::u8path(args[++i]));
            params += L" --test-root \"" + w.root.wstring() + L"\"";
            continue;
        }
        if (a == "--setup" || a == "--uninstall" || a == "--quiet" || a == "--keep-files") {
            quiet |= a == "--quiet";
            keep |= a == "--keep-files";
            params += L" " + fs::u8path(a).wstring();
            continue;
        }
        say("unknown argument: " + a);
        return 2;
    }
    if (setupArg && uninstallArg) { say("--setup and --uninstall together"); return 2; }
    if (setupArg && (quiet || keep)) { say("--quiet / --keep-files belong to --uninstall"); return 2; }
    if (keep && !quiet) { say("--keep-files needs --quiet"); return 2; }
    wchar_t self[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, self, (DWORD)(sizeof self / sizeof self[0]));
    if (n == 0 || n >= sizeof self / sizeof self[0]) { say("cannot find this program's path"); return 1; }
    w.exe = fs::path(std::wstring(self, n));
    w.dir = w.exe.parent_path();
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int code;
    if (setupArg) code = setup(w) ? 0 : 1;
    else if (keep) code = unsetup(w) ? 0 : 1;   // (the manager's form: it removes the folder itself)
    else code = uninstall(w, quiet, params);
    if (SUCCEEDED(co)) CoUninitialize();
    return code;
}

}  // namespace lathe
#endif
