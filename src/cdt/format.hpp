#pragma once

#include <string>

namespace coyote {

// Re-indents Coyote source by brace depth (4 spaces), strips trailing whitespace, collapses runs of
// blank lines to one, removes blank lines at the start and end, normalizes line endings to '\n' and
// ends the file with a newline. Text inside a line is never changed, so the token stream stays the
// same; comments are kept as they are. Throws CompileError (from the lexer) when `source` does not
// tokenize, and also if the result would tokenize differently (a formatter bug, never silently written).
std::string formatSource(const std::string& source, const std::string& file);

} // namespace coyote
