#include "json.hpp"

#include <cmath>
#include <cstdlib>

namespace coyote::pkg {

namespace {

constexpr int kMaxDepth = 64;

class Parser {
public:
    explicit Parser(const std::string& t) : s_(t) {}

    Json run() {
        // A UTF-8 byte order mark is tolerated (some editors write one).
        if (s_.compare(0, 3, "\xEF\xBB\xBF") == 0) { i_ = 3; }
        skipWs();
        Json v = value(0);
        skipWs();
        if (i_ < s_.size()) fail("unexpected text after the end of the JSON value");
        return v;
    }

private:
    const std::string& s_;
    size_t i_ = 0;
    int line_ = 1, col_ = 1;

    [[noreturn]] void fail(const std::string& m) const { throw JsonError{line_, col_, m}; }

    bool eof() const { return i_ >= s_.size(); }
    char cur() const { return s_[i_]; }

    void advance() {
        if (s_[i_] == '\n') { ++line_; col_ = 1; } else { ++col_; }
        ++i_;
    }

    void skipWs() {
        while (!eof() && (cur() == ' ' || cur() == '\t' || cur() == '\n' || cur() == '\r')) advance();
    }

    Json value(int depth) {
        if (depth > kMaxDepth) fail("JSON is nested too deeply (more than 64 levels)");
        if (eof()) fail("unexpected end of file, expected a value");
        Json v;
        v.line = line_;
        v.col = col_;
        char c = cur();
        if (c == '{') return object(depth, v);
        if (c == '[') return array(depth, v);
        if (c == '"') { v.kind = Json::String; v.string = str(); return v; }
        if (c == '-' || (c >= '0' && c <= '9')) return number(v);
        if (literal("true")) { v.kind = Json::Bool; v.boolean = true; return v; }
        if (literal("false")) { v.kind = Json::Bool; return v; }
        if (literal("null")) { v.kind = Json::Null; return v; }
        fail(std::string("unexpected character '") + c + "', expected a value");
    }

    bool literal(const char* word) {
        size_t n = std::char_traits<char>::length(word);
        if (s_.compare(i_, n, word) != 0) return false;
        for (size_t k = 0; k < n; ++k) advance();
        return true;
    }

    Json number(Json v) {
        size_t start = i_;
        bool integer = true;
        if (cur() == '-') advance();
        if (eof() || cur() < '0' || cur() > '9') fail("expected a digit in number");
        if (cur() == '0') {
            advance();
            if (!eof() && cur() >= '0' && cur() <= '9') fail("numbers cannot have leading zeros");
        } else {
            while (!eof() && cur() >= '0' && cur() <= '9') advance();
        }
        if (!eof() && cur() == '.') {
            integer = false;
            advance();
            if (eof() || cur() < '0' || cur() > '9') fail("expected a digit after '.' in number");
            while (!eof() && cur() >= '0' && cur() <= '9') advance();
        }
        if (!eof() && (cur() == 'e' || cur() == 'E')) {
            integer = false;
            advance();
            if (!eof() && (cur() == '+' || cur() == '-')) advance();
            if (eof() || cur() < '0' || cur() > '9') fail("expected a digit in number exponent");
            while (!eof() && cur() >= '0' && cur() <= '9') advance();
        }
        v.kind = Json::Number;
        v.number = std::strtod(s_.substr(start, i_ - start).c_str(), nullptr);
        v.isInteger = integer && std::fabs(v.number) <= 9007199254740992.0;
        return v;
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    unsigned hex4() {
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            if (eof()) fail("unterminated \\u escape");
            char c = cur();
            unsigned d;
            if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
            else fail("invalid hex digit in \\u escape");
            v = v * 16 + d;
            advance();
        }
        return v;
    }

    std::string str() {
        advance();  // opening quote
        std::string out;
        for (;;) {
            if (eof()) fail("unterminated string");
            unsigned char c = static_cast<unsigned char>(cur());
            if (c == '"') { advance(); return out; }
            if (c < 0x20) fail("control character in string (use an escape such as \\n)");
            if (c != '\\') { out += static_cast<char>(c); advance(); continue; }
            advance();
            if (eof()) fail("unterminated string");
            char e = cur();
            advance();
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (s_.compare(i_, 2, "\\u") != 0) fail("a high surrogate must be followed by a low surrogate escape");
                        advance(); advance();
                        unsigned lo = hex4();
                        if (lo < 0xDC00 || lo > 0xDFFF) fail("invalid low surrogate in \\u escape");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        fail("unpaired low surrogate in \\u escape");
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: fail(std::string("invalid escape '\\") + e + "'");
            }
        }
    }

    Json array(int depth, Json v) {
        v.kind = Json::Array;
        advance();
        skipWs();
        if (!eof() && cur() == ']') { advance(); return v; }
        for (;;) {
            v.items.push_back(value(depth + 1));
            skipWs();
            if (eof()) fail("unterminated array");
            if (cur() == ',') {
                advance();
                skipWs();
                if (!eof() && cur() == ']') fail("trailing comma in array");
                continue;
            }
            if (cur() == ']') { advance(); return v; }
            fail("expected ',' or ']' in array");
        }
    }

    Json object(int depth, Json v) {
        v.kind = Json::Object;
        advance();
        skipWs();
        if (!eof() && cur() == '}') { advance(); return v; }
        for (;;) {
            skipWs();
            if (eof()) fail("unterminated object");
            if (cur() != '"') fail("expected a string key in object");
            int kl = line_, kc = col_;
            std::string key = str();
            for (const auto& m : v.members)
                if (m.first == key) throw JsonError{kl, kc, "duplicate key '" + key + "'"};
            skipWs();
            if (eof() || cur() != ':') fail("expected ':' after object key");
            advance();
            skipWs();
            v.members.emplace_back(std::move(key), value(depth + 1));
            skipWs();
            if (eof()) fail("unterminated object");
            if (cur() == ',') {
                advance();
                skipWs();
                if (!eof() && cur() == '}') fail("trailing comma in object");
                continue;
            }
            if (cur() == '}') { advance(); return v; }
            fail("expected ',' or '}' in object");
        }
    }
};

} // namespace

Json parseJson(const std::string& text) { return Parser(text).run(); }

} // namespace coyote::pkg
