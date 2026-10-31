#pragma once

#include <string>
#include <vector>

#include "ast.hpp"
#include "lexer.hpp"

namespace coyote {

// Parses a token stream into an AST. Throws CompileError on the first syntax error.
Program parse(const std::vector<Token>& tokens, const std::string& file);

} // namespace coyote
