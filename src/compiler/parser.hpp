#pragma once

#include <string>
#include <vector>

#include "ast.hpp"
#include "lexer.hpp"

namespace coyote {

// Parses a token stream into an AST. Throws CompileError on the first syntax error.
Program parse(const std::vector<Token>& tokens, const std::string& file);

// Like parse, but keeps going after a syntax error: every error found is appended to `errors` (at most 50) and the
// returned Program is incomplete when `errors` grew.
Program parseRecovering(const std::vector<Token>& tokens, const std::string& file, std::vector<Diagnostic>& errors);

} // namespace coyote
