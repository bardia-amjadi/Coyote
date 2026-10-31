#pragma once

#include <string>
#include <vector>

namespace coyote::pkg {

struct ProcessResult {
    bool started = false;   // false: the program could not be launched (see `error`)
    int exitCode = 0;       // exit status; 128 + signal number if the program was killed by a signal
    std::string error;
};

// Directory containing the running executable (empty if unknown). Used to find sibling tools
// (`coyote`, `pvm`) of an installed toolchain before falling back to PATH.
std::string executableDir(const char* argv0);

// Runs a program without a shell, with the parent's stdin/stdout/stderr and working directory.
// `argv[0]` is looked up first in `siblingDir` (if non-empty), then on PATH. Waits for it to finish.
ProcessResult runProcess(const std::vector<std::string>& argv, const std::string& siblingDir);

// Like runProcess, but the program's stdout is captured into `out` (stderr is inherited). Used for
// tools whose output cpm parses (git).
ProcessResult runProcessCapture(const std::vector<std::string>& argv, const std::string& siblingDir,
                                std::string& out);

} // namespace coyote::pkg
