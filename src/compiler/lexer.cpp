#include "lexer.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace coyote {

namespace {

bool isIdentStart(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool isDigit(unsigned char c) { return c >= '0' && c <= '9'; }
bool isIdentPart(unsigned char c) { return isIdentStart(c) || isDigit(c); }

std::string describeChar(unsigned char c) {
    if (c >= 0x20 && c < 0x7f) return std::string("'") + static_cast<char>(c) + "'";
    char buf[16];
    std::snprintf(buf, sizeof buf, "'\\x%02X'", c);
    return buf;
}

} // namespace

std::vector<Token> lex(const std::string& s, const std::string& file) {
    std::vector<Token> out;
    size_t i = 0;
    int line = 1, col = 1;
    bool nl = false;

    auto fail = [&](SourceLoc loc, std::string msg) {
        throw CompileError{Diagnostic{file, loc, std::move(msg)}};
    };
    auto advance = [&]() {
        if (s[i] == '\n') {
            ++line;
            col = 1;
        } else {
            ++col;
        }
        ++i;
    };

    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\n') { nl = true; advance(); continue; }
        if (c == ' ' || c == '\t' || c == '\r') { advance(); continue; }
        if (c == '#') {
            while (i < s.size() && s[i] != '\n') advance();
            continue;
        }

        Token t;
        t.loc = SourceLoc{line, col};
        t.nlBefore = nl;
        nl = false;

        if (isIdentStart(c)) {
            while (i < s.size() && isIdentPart(static_cast<unsigned char>(s[i]))) {
                t.text += s[i];
                advance();
            }
            t.kind = Tok::Ident;
        } else if (isDigit(c)) {
            std::string digits;
            while (i < s.size() && isDigit(static_cast<unsigned char>(s[i]))) {
                digits += s[i];
                advance();
            }
            if (i < s.size() && isIdentStart(static_cast<unsigned char>(s[i]))) {
                fail(t.loc, "invalid integer literal '" + digits + s[i] + "...'");
            }
            if (i + 1 < s.size() && s[i] == '.' && isDigit(static_cast<unsigned char>(s[i + 1]))) {
                digits += '.';
                advance();
                while (i < s.size() && isDigit(static_cast<unsigned char>(s[i]))) {
                    digits += s[i];
                    advance();
                }
                if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
                    size_t j = i + 1;
                    if (j < s.size() && (s[j] == '+' || s[j] == '-')) ++j;
                    if (j < s.size() && isDigit(static_cast<unsigned char>(s[j]))) {
                        while (i < j) { digits += s[i]; advance(); }
                        while (i < s.size() && isDigit(static_cast<unsigned char>(s[i]))) { digits += s[i]; advance(); }
                    }
                }
                if (i < s.size() && isIdentStart(static_cast<unsigned char>(s[i])))
                    fail(t.loc, "invalid double literal '" + digits + s[i] + "...'");
                double v = std::strtod(digits.c_str(), nullptr);
                if (!std::isfinite(v)) fail(t.loc, "double literal '" + digits + "' is out of range");
                t.kind = Tok::Double;
                t.text = digits;
                t.dval = v;
                out.push_back(std::move(t));
                continue;
            }
            // 'int' is a 32-bit signed integer (see docs/language.md).
            if (digits.size() > 10 || std::stoll(digits) > 2147483647LL) {
                fail(t.loc, "integer literal '" + digits + "' is out of range for int");
            }
            t.kind = Tok::Int;
            t.text = digits;
            t.ival = std::stoll(digits);
        } else if (c == '\'') {
            SourceLoc start = t.loc;
            advance();  // opening quote
            if (i >= s.size() || s[i] == '\n') fail(start, "unterminated character literal");
            long long cp = 0;
            unsigned char ch = static_cast<unsigned char>(s[i]);
            if (ch == '\'') fail(start, "empty character literal");
            if (ch == '\\') {
                SourceLoc esc{line, col};
                advance();
                if (i >= s.size() || s[i] == '\n') fail(start, "unterminated character literal");
                char e = s[i];
                switch (e) {
                    case 'n': cp = '\n'; break;
                    case 't': cp = '\t'; break;
                    case 'r': cp = '\r'; break;
                    case '0': cp = 0; break;
                    case '\\': cp = '\\'; break;
                    case '\'': cp = '\''; break;
                    case '"': cp = '"'; break;
                    default: fail(esc, std::string("unknown escape sequence '\\") + e + "'");
                }
                advance();
            } else if (ch < 0x80) {
                cp = ch;
                advance();
            } else {  // UTF-8 sequence
                int n = ch >= 0xF0 ? 4 : ch >= 0xE0 ? 3 : ch >= 0xC0 ? 2 : 0;
                if (n == 0 || ch >= 0xF8) fail(start, "invalid UTF-8 in character literal");
                cp = ch & (0xFF >> (n + 1));
                advance();
                for (int k = 1; k < n; ++k) {
                    if (i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80)
                        fail(start, "invalid UTF-8 in character literal");
                    cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
                    advance();
                }
                if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) fail(start, "invalid Unicode code point in character literal");
            }
            if (i >= s.size() || s[i] != '\'') fail(start, "a character literal must contain exactly one character (use double quotes for strings)");
            advance();  // closing quote
            t.kind = Tok::Char;
            t.ival = cp;
        } else if (c == '"' || (c == '$' && i + 1 < s.size() && s[i + 1] == '"')) {
            SourceLoc start = t.loc;
            if (c == '$') { t.ival = 1; advance(); }  // $"...": an interpolated string (the parser splits it)
            advance();  // opening quote
            bool closed = false;
            while (i < s.size() && s[i] != '\n') {
                char ch = s[i];
                if (ch == '"') { advance(); closed = true; break; }
                if (ch == '\\') {
                    SourceLoc esc{line, col};
                    advance();
                    if (i >= s.size() || s[i] == '\n') break;
                    char e = s[i];
                    switch (e) {
                        case 'n': t.text += '\n'; break;
                        case 't': t.text += '\t'; break;
                        case 'r': t.text += '\r'; break;
                        case '\\': t.text += '\\'; break;
                        case '"': t.text += '"'; break;
                        default:
                            fail(esc, std::string("unknown escape sequence '\\") + e + "'");
                    }
                    advance();
                } else {
                    t.text += ch;
                    advance();
                }
            }
            if (!closed) fail(start, "unterminated string literal");
            t.kind = Tok::String;
        } else if (i + 1 < s.size() &&
                   ((c == '=' && (s[i + 1] == '=' || s[i + 1] == '>')) || (c == '!' && s[i + 1] == '=') ||
                    (c == '<' && s[i + 1] == '=') || (c == '>' && s[i + 1] == '=') ||
                    (c == '&' && s[i + 1] == '&') || (c == '+' && s[i + 1] == '+') || (c == '-' && s[i + 1] == '-') || (c == '?' && s[i + 1] == '?') || (c == '|' && s[i + 1] == '|') ||
                    ((c == '+' || c == '-' || c == '*' || c == '/' || c == '%') && s[i + 1] == '='))) {
            t.kind = Tok::Punct;
            t.text = s.substr(i, 2);
            advance();
            advance();
        } else if (c == '(' || c == ')' || c == '{' || c == '}' || c == ',' || c == ':' ||
                   c == ';' || c == '.' || c == '=' || c == '[' || c == ']' || c == '<' ||
                   c == '>' || c == '+' || c == '-' || c == '*' || c == '/' || c == '?' ||
                   c == '%' || c == '!') {
            // Operators and brackets are tokenized so the parser can report "not supported yet"
            // for constructs that use them, instead of a confusing lexical error.
            t.kind = Tok::Punct;
            t.text = std::string(1, static_cast<char>(c));
            advance();
        } else {
            fail(t.loc, "unexpected character " + describeChar(c));
        }
        out.push_back(std::move(t));
    }

    Token eof;
    eof.kind = Tok::Eof;
    eof.loc = SourceLoc{line, col};
    eof.nlBefore = true;
    out.push_back(std::move(eof));
    return out;
}

} // namespace coyote
