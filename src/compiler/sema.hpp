#pragma once

#include <string>
#include <vector>

#include "ast.hpp"

namespace coyote {

// Resolves calls against hand declarations and checks argument counts and types.
// Appends diagnostics (sorted by position) and returns true when there were no errors.
bool analyze(Program& program, const std::string& file, std::vector<Diagnostic>& diagnostics);

} // namespace coyote
