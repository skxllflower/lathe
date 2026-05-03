#pragma once

#include <functional>
#include <string>
#include <vector>

namespace lathe {

// Spawns a child with the given UTF-8 argv and invokes on_line for every
// line of merged stdout+stderr. Returns the child's exit code, or -1 on
// spawn failure (call last_subprocess_error() for diagnostics).
//
// On Windows: uses CreateProcessW so non-ASCII paths survive, and assigns
// the child to a Job Object with KILL_ON_JOB_CLOSE so the child dies if
// the wrapper is killed (TerminateProcess from a parent), or if the
// wrapper exits, or if Ctrl+C is delivered.
int run_subprocess(const std::vector<std::string>& argv,
                   const std::function<void(const std::string&)>& on_line);

// True if Ctrl+C / Ctrl+Break / console-close fired during the most
// recent run_subprocess. Cleared at the start of each run_subprocess.
bool was_cancelled();

// Error message captured on spawn failure (return -1) from the most
// recent run_subprocess. Empty otherwise.
std::string last_subprocess_error();

// Absolute path to the directory containing this executable, UTF-8.
std::string exe_dir();

#ifdef _WIN32
std::wstring utf8_to_utf16(const std::string& s);
std::string  utf16_to_utf8(const std::wstring& s);
#endif

}
