#pragma once

#include <string>
#include <vector>

#include "diag.hpp"

namespace coyote {

enum class Tok { Ident, Int, Double, Char, String, Punct, Eof };

struct Token {
    Tok kind = Tok::Eof;
    std::string text;       // identifier name, punctuation char, or decoded string value
    long long ival = 0;     // value of Int tokens, code point of Char tokens
    double dval = 0;        // value of Double tokens
    SourceLoc loc;
    bool nlBefore = false;  // a newline separates this token from the previous one
    bool isLong = false;    // Int tokens: written with an L suffix or too large for int (the literal is a long)
};

// Tokenizes a .coy source. Throws CompileError on the first lexical error.
std::vector<Token> lex(const std::string& source, const std::string& file);

} // namespace coyote
