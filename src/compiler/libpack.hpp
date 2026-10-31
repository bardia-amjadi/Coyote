#pragma once

#include <iosfwd>
#include <string>

namespace coyote {

// `coyote lib pack <project-dir> [-o file.lib]` and `coyote lib info <file.lib>`; returns the exit code.
int libCommand(int argc, char** argv, std::ostream& out, std::ostream& err);

} // namespace coyote
