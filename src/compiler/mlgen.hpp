#pragma once

#include <string>

#include "ast.hpp"

namespace coyote {

// ML format version emitted by this compiler (see docs/ml-spec.md).
constexpr int kMlVersion = 5;

// Generates textual ML for a semantically valid program. The compiler and the PVM share only
// this textual format, never code.
// With `removeUnused` (the default), methods that cannot be reached from the program's top-level code are not
// emitted (dead-code removal: unused library and class methods cost nothing).
std::string generateMl(const Program& program, const std::string& sourcePath, bool removeUnused = true);

} // namespace coyote
