// registry.rs register_self and tools.rs find_tool_binary's own-core half (data_root.h).
#include "data_root.h"
#include "dev_run.h"
#include "external_tools.h"
#include <cctype>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace fs = std::filesystem;

namespace lathe {

namespace {
bool isFile(const fs::path& p) { std::error_code ec; return fs::is_regular_file(p, ec); }
#if defined(_WIN32)
const char* const kCore = "lathe.exe";
#else
const char* const kCore = "lathe";
#endif
}  // namespace

std::string ownCore() {
    const std::string own = wdn::vendorExeDir();
    if (own.empty()) return {};
    const fs::path dir = fs::u8path(own);
    if (isFile(dir / "coredist" / kCore)) return (dir / "coredist" / kCore).generic_u8string();
#if !defined(__APPLE__)   // (on the Mac's case-insensitive disk Contents/MacOS/lathe is this GUI itself, .../MacOS/Lathe)
    if (isFile(dir / kCore)) return (dir / kCore).generic_u8string();
#endif
    if (!wdn::dev::devRun()) return {};
    // native/build/bin/Release/lathe-gui.exe (or native/build/bin/lathe-gui): the repo is the folder over native/
    for (fs::path d = dir; !d.empty() && d != d.parent_path(); d = d.parent_path()) {
        if (d.filename() != "native") continue;
        const fs::path b = d.parent_path() / "build";
        for (const fs::path& c : {b / "Release" / kCore, b / kCore, b / "Debug" / kCore})   // (Release first: a Debug demosaic is ~7 s a RAW)
            if (isFile(c)) return c.generic_u8string();
        break;
    }
    return {};
}

void registerSelf(const std::string& core) {
    if (core.empty() || !isWritable()) return;
    const std::string shared = wdn::vendorSharedDir();
    if (!shared.empty()) registerSelfIn(shared, core);
}

namespace {
nlohmann::json readRegistry(const fs::path& p) {
    nlohmann::json root;
    std::ifstream in(p, std::ios::binary);
    if (in) { std::ostringstream ss; ss << in.rdbuf(); root = nlohmann::json::parse(ss.str(), nullptr, false); }
    return root.is_object() ? root : nlohmann::json::object();
}

// written to a temp file and renamed over (a reader never sees half a file)
void writeRegistry(const std::string& shared, const nlohmann::json& root) {
    const fs::path p = fs::u8path(shared + "/registry.json"), tmp = fs::u8path(shared + "/registry.json.tmp");
    std::error_code ec;
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << root.dump(2);
        if (!out) return;
    }
    fs::rename(tmp, p, ec);
    if (ec) fs::remove(tmp, ec);
}

std::string nativeSeparators(std::string s) {
#if defined(_WIN32)
    for (char& c : s) if (c == '/') c = '\\';   // (the Tauri app wrote the OS's own separators)
#endif
    return s;
}

std::string folded(std::string s) {   // (a path compared as Windows does: case and separators aside)
    for (char& c : s) { if (c == '\\') c = '/'; c = (char)std::tolower((unsigned char)c); }
    return s;
}
}  // namespace

void registerSelfIn(const std::string& shared, const std::string& core) {
    if (shared.empty() || core.empty()) return;
    std::error_code ec;
    fs::create_directories(fs::u8path(shared), ec);
    nlohmann::json root = readRegistry(fs::u8path(shared + "/registry.json"));
    root["lathe"] = {{"path", nativeSeparators(core)}, {"version", LATHE_VERSION}};
    writeRegistry(shared, root);
}

void unregisterSelfIn(const std::string& shared, const std::string& core) {
    if (shared.empty() || core.empty()) return;
    const fs::path p = fs::u8path(shared + "/registry.json");
    std::error_code ec;
    if (!fs::exists(p, ec)) return;
    nlohmann::json root = readRegistry(p);
    if (!root.contains("lathe") || !root["lathe"].is_object()) return;
    const std::string was = root["lathe"].value("path", std::string());
    if (folded(was) != folded(core)) return;   // (another install's: left alone)
    root.erase("lathe");
    writeRegistry(shared, root);
}

}  // namespace lathe
