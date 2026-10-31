// Coyote compiler diagnostics. Deterministic "file:line:col: error: message" format.
#pragma once

#include <string>

namespace coyote {

struct SourceLoc {
    int line = 1;
    int col = 1;
};

struct Diagnostic {
    std::string file;
    SourceLoc loc;
    std::string message;
};

// Thrown by the lexer and parser, which stop at the first error.
struct CompileError {
    Diagnostic diag;
};

inline std::string formatDiagnostic(const Diagnostic& d) {
    return d.file + ":" + std::to_string(d.loc.line) + ":" + std::to_string(d.loc.col) +
           ": error: " + d.message;
}

} // namespace coyote
