// The Tauri Lathe's settings carried over once: its webview kept them in localStorage (ConvertApp.tsx SETTINGS_KEY /
// PRESETS_KEY), which WebView2 stores as a LevelDB under %LOCALAPPDATA%\com.vacantsystems.lathe\EBWebView\Default\
// Local Storage\leveldb. The native window reads the same keys, in the same JSON shapes, from native_ui.json
// (lathe_window.cpp), so each value is copied as it is. Read only: the write-ahead logs (*.log) and the tables (*.ldb,
// Snappy blocks), the newest sequence number winning per key. settings.json's "tauri_imported" marks it done, found or
// not, and a key the native window already has is never replaced.
#include "data_root.h"
#include "settings_store.h"
#include "ui_state.h"
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace lathe {

namespace {

const char* const kKeys[] = {"wd-lathe-settings-v2", "wd-lathe-presets"};
const std::string kPrefix = std::string("_http://tauri.localhost") + '\0' + '\x01';   // (origin, NUL, Latin-1 key)

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool varint(const std::string& s, size_t& i, uint64_t& v) {
    v = 0;
    for (int shift = 0; shift < 64 && i < s.size(); shift += 7) {
        const uint8_t b = (uint8_t)s[i++];
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) return true;
    }
    return false;
}

uint32_t le32(const std::string& s, size_t i) { return (uint8_t)s[i] | (uint8_t)s[i + 1] << 8 | (uint8_t)s[i + 2] << 16 | (uint32_t)(uint8_t)s[i + 3] << 24; }
uint64_t le64(const std::string& s, size_t i) { return le32(s, i) | (uint64_t)le32(s, i + 4) << 32; }

bool snappy(const std::string& in, std::string& out) {
    size_t i = 0;
    uint64_t len = 0;
    if (!varint(in, i, len) || len > (64u << 20)) return false;
    out.clear();
    out.reserve((size_t)len);
    while (i < in.size()) {
        const uint8_t tag = (uint8_t)in[i++];
        size_t n = 0, off = 0;
        switch (tag & 3) {
            case 0: {   // a literal
                n = tag >> 2;
                if (n >= 60) {
                    const size_t bytes = n - 59;
                    if (i + bytes > in.size()) return false;
                    n = 0;
                    for (size_t b = 0; b < bytes; ++b) n |= (size_t)(uint8_t)in[i + b] << (8 * b);
                    i += bytes;
                }
                ++n;
                if (i + n > in.size()) return false;
                out.append(in, i, n);
                i += n;
                continue;
            }
            case 1:
                if (i + 1 > in.size()) return false;
                n = 4 + ((tag >> 2) & 7);
                off = (size_t)(tag >> 5) << 8 | (uint8_t)in[i++];
                break;
            case 2:
                if (i + 2 > in.size()) return false;
                n = 1 + (tag >> 2);
                off = (uint8_t)in[i] | (size_t)(uint8_t)in[i + 1] << 8;
                i += 2;
                break;
            default:
                if (i + 4 > in.size()) return false;
                n = 1 + (tag >> 2);
                off = le32(in, i);
                i += 4;
        }
        if (off == 0 || off > out.size()) return false;
        for (size_t k = 0; k < n; ++k) out.push_back(out[out.size() - off]);   // (a copy may overlap itself)
    }
    return out.size() == len;
}

struct Val { uint64_t seq = 0; bool del = false; std::string v; };
using Found = std::map<std::string, Val>;

void offer(Found& f, const std::string& userKey, uint64_t seq, bool del, const std::string& v) {
    if (userKey.compare(0, kPrefix.size(), kPrefix) != 0) return;
    const std::string name = userKey.substr(kPrefix.size());
    bool wanted = false;
    for (const char* k : kKeys) wanted |= name == k;
    if (!wanted) return;
    Val& cur = f[name];
    if (seq < cur.seq) return;
    cur = {seq, del, v};
}

// a write-ahead log: 32 KiB blocks of records (crc, length, type: full / first / middle / last), each whole record a
// WriteBatch (sequence, count, then put or delete entries)
void readLog(const std::string& s, Found& f) {
    std::string rec;
    for (size_t i = 0; i + 7 <= s.size();) {
        const size_t left = 32768 - i % 32768;
        if (left < 7) { i += left; continue; }
        const size_t n = (uint8_t)s[i + 4] | (size_t)(uint8_t)s[i + 5] << 8;
        const uint8_t type = (uint8_t)s[i + 6];
        if (i + 7 + n > s.size()) break;
        const std::string frag = s.substr(i + 7, n);
        i += 7 + n;
        if (type == 1 || type == 2) rec = frag;
        else if (type == 3 || type == 4) rec += frag;
        else continue;
        if (type == 2 || type == 3) continue;
        if (rec.size() < 12) continue;
        uint64_t seq = le64(rec, 0);
        const uint32_t count = le32(rec, 8);
        size_t p = 12;
        for (uint32_t c = 0; c < count && p < rec.size(); ++c, ++seq) {
            const uint8_t tag = (uint8_t)rec[p++];
            uint64_t kl = 0, vl = 0;
            if (!varint(rec, p, kl) || p + kl > rec.size()) break;
            const std::string key = rec.substr(p, (size_t)kl);
            p += (size_t)kl;
            std::string val;
            if (tag == 1) {
                if (!varint(rec, p, vl) || p + vl > rec.size()) break;
                val = rec.substr(p, (size_t)vl);
                p += (size_t)vl;
            }
            offer(f, key, seq, tag == 0, val);
        }
    }
}

// a block of a table: entries with shared-prefix keys, then the restart array
template <typename Fn> void blockEntries(const std::string& b, Fn fn) {
    if (b.size() < 4) return;
    const uint32_t restarts = le32(b, b.size() - 4);
    if ((uint64_t)restarts * 4 + 4 > b.size()) return;
    const size_t end = b.size() - 4 - restarts * 4;
    std::string key;
    for (size_t p = 0; p < end;) {
        uint64_t shared = 0, unshared = 0, vl = 0;
        if (!varint(b, p, shared) || !varint(b, p, unshared) || !varint(b, p, vl)) return;
        if (shared > key.size() || p + unshared + vl > end) return;
        key.resize((size_t)shared);
        key.append(b, p, (size_t)unshared);
        p += (size_t)unshared;
        fn(key, b.substr(p, (size_t)vl));
        p += (size_t)vl;
    }
}

bool block(const std::string& s, uint64_t off, uint64_t size, std::string& out) {
    if (off + size + 5 > s.size()) return false;
    const std::string raw = s.substr((size_t)off, (size_t)size);
    const uint8_t type = (uint8_t)s[(size_t)(off + size)];
    if (type == 0) { out = raw; return true; }
    return type == 1 && snappy(raw, out);
}

// a table: the footer's index handle, the index block's data block handles, each data block's internal keys (the user
// key, then 8 bytes: sequence << 8 | type)
void readTable(const std::string& s, Found& f) {
    if (s.size() < 48) return;
    size_t p = s.size() - 48;
    uint64_t mo = 0, ms = 0, io = 0, is = 0;
    if (!varint(s, p, mo) || !varint(s, p, ms) || !varint(s, p, io) || !varint(s, p, is)) return;
    std::string index;
    if (!block(s, io, is, index)) return;
    blockEntries(index, [&](const std::string&, const std::string& handle) {
        size_t q = 0;
        uint64_t bo = 0, bs = 0;
        std::string data;
        if (!varint(handle, q, bo) || !varint(handle, q, bs) || !block(s, bo, bs, data)) return;
        blockEntries(data, [&](const std::string& ikey, const std::string& v) {
            if (ikey.size() < 8) return;
            const uint64_t tail = le64(ikey, ikey.size() - 8);
            offer(f, ikey.substr(0, ikey.size() - 8), tail >> 8, (tail & 0xff) == 0, v);
        });
    });
}

// a stored value: 0x01 then Latin-1, or 0x00 then UTF-16LE; returned as UTF-8
std::string utf8Value(const std::string& v) {
    if (v.empty()) return {};
    std::string out;
    auto put = [&](uint32_t c) {
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xc0 | c >> 6); out += (char)(0x80 | (c & 0x3f)); }
        else if (c < 0x10000) { out += (char)(0xe0 | c >> 12); out += (char)(0x80 | (c >> 6 & 0x3f)); out += (char)(0x80 | (c & 0x3f)); }
        else { out += (char)(0xf0 | c >> 18); out += (char)(0x80 | (c >> 12 & 0x3f)); out += (char)(0x80 | (c >> 6 & 0x3f)); out += (char)(0x80 | (c & 0x3f)); }
    };
    if (v[0] == '\x01') {
        for (size_t i = 1; i < v.size(); ++i) put((uint8_t)v[i]);
    } else {
        for (size_t i = 1; i + 1 < v.size(); i += 2) {
            uint32_t c = (uint8_t)v[i] | (uint32_t)(uint8_t)v[i + 1] << 8;
            if (c >= 0xd800 && c < 0xdc00 && i + 3 < v.size()) {
                const uint32_t lo = (uint8_t)v[i + 2] | (uint32_t)(uint8_t)v[i + 3] << 8;
                if (lo >= 0xdc00 && lo < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00); i += 2; }
            }
            put(c);
        }
    }
    return out;
}

fs::path tauriStore() {
#if defined(_WIN32)
    const wchar_t* la = _wgetenv(L"LOCALAPPDATA");
    if (!la || !*la) return {};
    return fs::path(la) / L"com.vacantsystems.lathe" / L"EBWebView" / L"Default" / L"Local Storage" / L"leveldb";
#else
    return {};   // (the Mac's WKWebView keeps it elsewhere, in a SQLite file: not read yet)
#endif
}

}  // namespace

int importTauriSettings() {
    if (!isWritable() || wdn::settings::flag("tauri_imported", false)) return 0;
    Found found;
    std::error_code ec;
    const fs::path dir = tauriStore();
    if (!dir.empty() && fs::is_directory(dir, ec)) {
        std::map<std::string, fs::path> logs, tables;   // (by name: older files first)
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const std::string ext = e.path().extension().u8string();
            if (ext == ".log") logs[e.path().filename().u8string()] = e.path();
            else if (ext == ".ldb" || ext == ".sst") tables[e.path().filename().u8string()] = e.path();
        }
        for (const auto& t : tables) readTable(readFile(t.second), found);
        for (const auto& l : logs) readLog(readFile(l.second), found);
    }
    fs::create_directories(fs::u8path(dataRoot()), ec);   // (a first launch: ui_state writes into it, never makes it)
    int n = 0;
    for (const auto& kv : found) {
        if (kv.second.del || kv.second.v.empty()) continue;
        if (!wdn::ui_state::get(kv.first, "").empty()) continue;
        const std::string v = utf8Value(kv.second.v);
        if (nlohmann::json::parse(v, nullptr, false).is_discarded()) continue;
        wdn::ui_state::set(kv.first, v);
        ++n;
    }
    wdn::settings::set("tauri_imported", true);
    return n;
}

}  // namespace lathe
