#include "format.hpp"

#include <algorithm>
#include <sstream>
#include <vector>

#include "lexer.hpp"

namespace coyote {

namespace {

struct LineInfo {
    int leadingClosers = 0;  // '}' characters at the start of the line
    int delta = 0;           // opening minus closing braces outside strings and comments
};

LineInfo scanLine(const std::string& s) {
    LineInfo info;
    bool inString = false, atStart = true;
    char quote = '"';
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (inString) {
            if (c == '\\') ++i;
            else if (c == quote) inString = false;
            continue;
        }
        if (c == '#') break;
        if (c == '"' || c == '\'') { inString = true; quote = c; atStart = false; continue; }
        if (c == '{') { ++info.delta; atStart = false; continue; }
        if (c == '}') {
            --info.delta;
            if (atStart) ++info.leadingClosers;
            continue;
        }
        if (c != ' ' && c != '\t') atStart = false;
    }
    return info;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

bool sameTokens(const std::vector<Token>& a, const std::vector<Token>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].kind != b[i].kind || a[i].text != b[i].text || a[i].ival != b[i].ival || a[i].dval != b[i].dval || a[i].nlBefore != b[i].nlBefore) return false;
    return true;
}

} // namespace

std::string formatSource(const std::string& source, const std::string& file) {
    std::vector<Token> before = lex(source, file);  // rejects source that does not tokenize

    std::vector<std::string> lines;
    {
        std::string cur;
        for (size_t i = 0; i < source.size(); ++i) {
            if (source[i] == '\n') { lines.push_back(cur); cur.clear(); }
            else cur += source[i];
        }
        if (!cur.empty()) lines.push_back(cur);
    }

    std::string out;
    int depth = 0;
    bool pendingBlank = false, wroteAny = false;
    for (const std::string& raw : lines) {
        std::string t = trim(raw);
        if (t.empty()) {
            if (wroteAny) pendingBlank = true;
            continue;
        }
        LineInfo info = scanLine(t);
        int indent = std::max(0, depth - info.leadingClosers);
        if (pendingBlank) out += "\n";
        pendingBlank = false;
        out.append(static_cast<size_t>(indent) * 4, ' ');
        out += t;
        out += "\n";
        wroteAny = true;
        depth = std::max(0, depth + info.delta);
    }

    if (!sameTokens(before, lex(out, file)))
        throw CompileError{Diagnostic{file, SourceLoc{1, 1}, "internal error: formatting would change the program; the file was not modified"}};
    return out;
}

} // namespace coyote
