// Lathe's own folders and its place in the suite (host_stubs.cpp, self_register.cpp).
#pragma once
#include <string>

namespace lathe {

std::string dataRoot();          // Lathe's data folder, '/' separated (settings.json, native_ui.json)
void setWritable(bool on);       // false: settings.json is never written (headless runs)
bool isWritable();

// This app's own lathe CLI: <exe dir>/coredist/lathe (the installed layout on both platforms), then beside the exe;
// in a dev run (the exe in a CMake build tree) the repo's own build (build/Release, then build/, build/Debug).
// "" when none is there.
std::string ownCore();
// Shared/registry.json's "lathe" entry pointed at `core` (registry.rs register_self: WAVdesk finds this install's CLI
// there); siblings kept, written to a temp file and renamed over
void registerSelf(const std::string& core);

// The Tauri Lathe's saved settings and presets (its webview's localStorage) copied into native_ui.json, once
// (settings.json "tauri_imported"); never over a value already there, never in a headless run. Returns how many keys.
int importTauriSettings();

}  // namespace lathe
