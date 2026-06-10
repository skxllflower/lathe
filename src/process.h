#pragma once

#include <cstddef>
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

// Like run_subprocess, but keeps the child's stdout and stderr on SEPARATE
// pipes: stdout bytes are delivered raw (binary-safe, never line-split) to
// on_stdout; stderr is delivered line-by-line to on_stderr_line. This is for
// streaming a decoded rawvideo pipe, where stdout is binary frame data that
// must not be merged with stderr text. on_stdout returns false to stop early
// (e.g. the downstream consumer closed the pipe), which terminates the child.
// Same Job Object kill-on-close lifetime guard as run_subprocess. Returns the
// child exit code, or -1 on spawn failure (last_subprocess_error() for detail).
int run_subprocess_streaming(
    const std::vector<std::string>& argv,
    const std::function<bool(const char*, std::size_t)>& on_stdout,
    const std::function<void(const std::string&)>& on_stderr_line);

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
