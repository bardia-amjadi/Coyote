#include "loader.hpp"

#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>

namespace pvm {

namespace {

constexpr int kMinVersion = 1;
constexpr int kMaxVersion = 8;
constexpr int kMaxTypeDepth = 32;  // nesting limit for array<array<...>>, so hostile input cannot recurse deeply

struct Word {
    std::string text;
    bool quoted = false;
};

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Splits one ML line into words. '(' ')' ',' and '->' are their own words; "..." is a quoted
// string with escapes. Returns an empty string on success or an error message.
std::string tokenize(const std::string& line, std::vector<Word>& out) {
    size_t i = 0;
    while (i < line.size()) {
        char c = line[i];
        if (c == ' ' || c == '\t') { ++i; continue; }
        if (c == '(' || c == ')' || c == ',') {
            out.push_back(Word{std::string(1, c), false});
            ++i;
        } else if (c == '"') {
            std::string v;
            ++i;
            bool closed = false;
            while (i < line.size()) {
                char ch = line[i];
                if (ch == '"') { ++i; closed = true; break; }
                if (ch != '\\') { v += ch; ++i; continue; }
                if (i + 1 >= line.size()) break;
                char e = line[i + 1];
                i += 2;
                switch (e) {
                    case 'n': v += '\n'; break;
                    case 't': v += '\t'; break;
                    case 'r': v += '\r'; break;
                    case '\\': v += '\\'; break;
                    case '"': v += '"'; break;
                    case 'x': {
                        if (i + 1 >= line.size() || hexVal(line[i]) < 0 || hexVal(line[i + 1]) < 0)
                            return "invalid \\x escape in string";
                        v += static_cast<char>(hexVal(line[i]) * 16 + hexVal(line[i + 1]));
                        i += 2;
                        break;
                    }
                    default: return std::string("unknown escape sequence '\\") + e + "' in string";
                }
            }
            if (!closed) return "unterminated string";
            out.push_back(Word{v, true});
        } else {
            std::string w;
            int angle = 0;  // commas inside <...> belong to the word (map<K,V>)
            int par = 0;    // parentheses of a function type fn(T,U)->R belong to the word too
            while (i < line.size()) {
                char ch = line[i];
                if (ch == ' ' || ch == '\t' || ch == '"') break;
                if (ch == '(' && par == 0) {
                    const size_t n = w.size();
                    bool fnStart = n >= 2 && w.compare(n - 2, 2, "fn") == 0 &&
                                   (n == 2 || !(std::isalnum(static_cast<unsigned char>(w[n - 3])) || w[n - 3] == '_'));
                    if (!fnStart) break;
                }
                if (ch == ')' && par == 0) break;
                if (ch == ',' && angle == 0 && par == 0) break;
                if (ch == '(') ++par;
                else if (ch == ')') --par;
                else if (ch == '<') ++angle;
                else if (ch == '>' && angle > 0 && !(w.size() > 0 && w.back() == '-')) --angle;
                w += ch;
                ++i;
            }
            out.push_back(Word{w, false});
        }
    }
    return "";
}

// Map keys must be hashable scalars.
bool isMapKey(const Type& k) {
    return k.kind == Type::Int || k.kind == Type::String || k.kind == Type::Char || k.kind == Type::Bool || k.kind == Type::Guid;
}

bool parseInt(const std::string& s, long long& out) {
    if (s.empty()) return false;
    size_t i = (s[0] == '-') ? 1 : 0;
    if (i == s.size() || s.size() - i > 10) return false;
    for (size_t k = i; k < s.size(); ++k)
        if (s[k] < '0' || s[k] > '9') return false;
    out = std::strtoll(s.c_str(), nullptr, 10);
    return true;
}

// A class, field or global name: letters, digits, '_', '$' and '.', not starting with a digit or '.'.
bool isClassName(const std::string& s) {
    if (s.empty() || s.size() > 256 || std::isdigit(static_cast<unsigned char>(s[0])) || s[0] == '.') return false;
    for (unsigned char c : s)
        if (!(std::isalnum(c) || c == '_' || c == '.' || c == '$')) return false;
    return true;
}

// Parses a type word: int, string, void, boolean (v2+), array<T> (v3+), double, char, Guid, obj,
// map<K,V> and T? (v4+), class names (v5+).
bool parseType(const std::string& s, int version, Type& t, int depth = 0) {
    if (depth > kMaxTypeDepth) return false;
    if (s.empty()) return false;
    if (version >= 6 && s.size() > 4 && s.compare(0, 3, "fn(") == 0) {  // fn(T,U)->R; R may itself be nullable
        int paren = 0;
        size_t close = std::string::npos;
        for (size_t k = 2; k < s.size(); ++k) {
            if (s[k] == '(') ++paren;
            else if (s[k] == ')' && --paren == 0) { close = k; break; }
        }
        if (close == std::string::npos || s.compare(close, 3, ")->") != 0 || close + 3 >= s.size()) return false;
        std::vector<Type> ps;
        std::string body = s.substr(3, close - 3);
        int depthAngle = 0, depthParen = 0;
        size_t start = 0;
        for (size_t k = 0; k <= body.size(); ++k) {
            char c = k < body.size() ? body[k] : ',';
            if (c == '<') ++depthAngle;
            else if (c == '>' && !(k > 0 && body[k - 1] == '-')) --depthAngle;
            else if (c == '(') ++depthParen;
            else if (c == ')') --depthParen;
            else if (c == ',' && depthAngle == 0 && depthParen == 0) {
                if (k == start && body.empty()) break;  // fn()->R
                Type p;
                if (!parseType(body.substr(start, k - start), version, p, depth + 1) || p.kind == Type::Void) return false;
                ps.push_back(std::move(p));
                start = k + 1;
            }
        }
        Type ret;
        if (!parseType(s.substr(close + 3), version, ret, depth + 1)) return false;
        t = Type::fnOf(std::move(ps), ret);
        return true;
    }
    if (version >= 4 && s.back() == '?') {
        Type inner;
        if (!parseType(s.substr(0, s.size() - 1), version, inner, depth + 1)) return false;
        if (inner.kind == Type::Void || inner.kind == Type::Nullable || inner.kind == Type::Obj || inner.kind == Type::Fn) return false;
        t = Type::nullableOf(inner);
        return true;
    }
    if (s == "int") { t = Type::Int; return true; }
    if (s == "string") { t = Type::String; return true; }
    if (s == "void") { t = Type::Void; return true; }
    if (s == "boolean" && version >= 2) { t = Type::Bool; return true; }
    if (version >= 4) {
        if (s == "double") { t = Type::Double; return true; }
        if (s == "char") { t = Type::Char; return true; }
        if (s == "Guid") { t = Type::Guid; return true; }
        if (s == "obj") { t = Type::Obj; return true; }
        if (s.size() > 6 && s.compare(0, 4, "map<") == 0 && s.back() == '>') {
            std::string body = s.substr(4, s.size() - 5);
            int angle = 0;
            size_t comma = std::string::npos;
            for (size_t k = 0; k < body.size(); ++k) {
                if (body[k] == '<') ++angle;
                else if (body[k] == '>') --angle;
                else if (body[k] == ',' && angle == 0) { comma = k; break; }
            }
            if (comma == std::string::npos) return false;
            Type key, value;
            if (!parseType(body.substr(0, comma), version, key, depth + 1)) return false;
            if (!parseType(body.substr(comma + 1), version, value, depth + 1) || value.kind == Type::Void) return false;
            if (!isMapKey(key)) return false;
            t = Type::mapOf(key, value);
            return true;
        }
    }
    if (version >= 3 && s.size() > 7 && s.compare(0, 6, "array<") == 0 && s.back() == '>') {
        Type inner;
        if (!parseType(s.substr(6, s.size() - 7), version, inner, depth + 1) || inner.kind == Type::Void) return false;
        t = Type::arrayOf(inner);
        return true;
    }
    if (version >= 5 && isClassName(s)) {  // a class type; the verifier checks that the class exists
        t = Type::classOf(s);
        return true;
    }
    return false;
}

const char* typeHint(int version) {
    if (version >= 6) return "int, string, boolean, double, char, Guid, obj, array<T>, map<K,V>, T?, fn(T,U)->R or a class name";
    if (version >= 5) return "int, string, boolean, double, char, Guid, obj, array<T>, map<K,V>, T? or a class name";
    if (version >= 4) return "int, string, boolean, double, char, Guid, obj, array<T>, map<K,V> or T?";
    if (version >= 3) return "int, string, boolean or array<T>";
    if (version >= 2) return "int, string or boolean";
    return "int or string";
}

class Loader {
public:
    Loader(const std::string& text, std::string file) : text_(text), file_(std::move(file)) {}

    Module run() {
        size_t pos = 0;
        bool sawHeader = false;
        Function* cur = nullptr;
        ClassDecl* curClass = nullptr;
        int srcLine = 0, srcFile = 0;
        int funcStartLine = 0;

        while (pos <= text_.size()) {
            size_t nlPos = text_.find('\n', pos);
            std::string raw = text_.substr(pos, nlPos == std::string::npos ? std::string::npos : nlPos - pos);
            pos = (nlPos == std::string::npos) ? text_.size() + 1 : nlPos + 1;
            ++lineNo_;
            if (!raw.empty() && raw.back() == '\r') raw.pop_back();

            std::vector<Word> w;
            std::string terr = tokenize(raw, w);
            if (!terr.empty()) fail(terr);
            if (w.empty() || (!w[0].quoted && !w[0].text.empty() && w[0].text[0] == ';')) continue;
            if (w[0].quoted) fail("a line cannot start with a string");
            const std::string& head = w[0].text;

            if (!sawHeader) {
                if (head != "ml" || w.size() != 2) fail("expected 'ml <version>' header as the first line");
                long long v;
                if (!parseInt(w[1].text, v)) fail("invalid ML version '" + w[1].text + "'");
                if (v < kMinVersion || v > kMaxVersion)
                    fail("unsupported ML version " + std::to_string(v) + " (this PVM supports versions " +
                         std::to_string(kMinVersion) + " to " + std::to_string(kMaxVersion) + ")");
                mod_.version = static_cast<int>(v);
                sawHeader = true;
                continue;
            }

            if (cur) {
                if (head == "func" || head == "entry" || head == "host" || head == "source" || head == "class" || head == "global") {
                    fail("function '" + cur->name + "' is not closed (missing 'end') before '" + head + "'");
                }
                if (head == "end") {
                    needArgs(w, 1);
                    cur = nullptr;
                } else if (head == "local") {
                    needVersion(2, "'local'");
                    needArgs(w, 2);
                    if (!cur->code.empty()) fail("'local' declarations must come before the first instruction");
                    Type t;
                    if (w[1].quoted || !parseType(w[1].text, mod_.version, t) || t == Type::Void)
                        fail(std::string("'local' needs a type (") + typeHint(mod_.version) + ")");
                    cur->locals.push_back(t);
                } else if (head == "label") {
                    needVersion(2, "'label'");
                    needArgs(w, 2);
                    if (w[1].quoted) fail("'label' needs a name");
                    if (!cur->labels.emplace(w[1].text, std::make_pair(static_cast<int>(cur->code.size()), lineNo_)).second)
                        fail("duplicate label '" + w[1].text + "' in function '" + cur->name + "'");
                } else if (head == ".file") {
                    needVersion(5, "'.file'");
                    needArgs(w, 2);
                    if (!w[1].quoted || w[1].text.empty()) fail("'.file' needs a quoted path");
                    size_t k = 0;
                    while (k < mod_.files.size() && mod_.files[k] != w[1].text) ++k;
                    if (k == mod_.files.size()) mod_.files.push_back(w[1].text);
                    srcFile = static_cast<int>(k);
                    srcLine = 0;
                } else if (head == ".var") {
                    needVersion(5, "'.var'");
                    needArgs(w, 3);
                    long long slot;
                    if (w[1].quoted || !parseInt(w[1].text, slot) || slot < 0 || !w[2].quoted || w[2].text.empty())
                        fail("'.var' needs a slot number and a quoted name");
                    if (cur->varNames.size() < 100000) cur->varNames[static_cast<int>(slot)] = w[2].text;
                } else if (head == ".line") {
                    needArgs(w, 2);
                    long long n;
                    if (!parseInt(w[1].text, n) || n < 1) fail("'.line' needs a positive integer");
                    srcLine = static_cast<int>(n);
                } else {
                    cur->code.push_back(instruction(head, w, srcLine, srcFile));
                }
                continue;
            }

            if (curClass) {
                if (head == "end") {
                    needArgs(w, 1);
                    curClass = nullptr;
                } else if (head == "field") {
                    needArgs(w, 3);
                    FieldDecl f;
                    Type ft;
                    if (w[1].quoted || !isClassName(w[1].text) || w[1].text.find('.') != std::string::npos)
                        fail("'field' needs a field name");
                    if (w[2].quoted || !parseType(w[2].text, mod_.version, ft) || ft.kind == Type::Void)
                        fail(std::string("'field' needs a type (") + typeHint(mod_.version) + " or a class name)");
                    for (const FieldDecl& other : curClass->fields)
                        if (other.name == w[1].text) fail("duplicate field '" + w[1].text + "' in class '" + curClass->name + "'");
                    if (curClass->fields.size() >= 4096) fail("class '" + curClass->name + "' has too many fields (limit 4096)");
                    f.name = w[1].text;
                    f.type = ft;
                    curClass->fields.push_back(std::move(f));
                } else {
                    fail("only 'field' declarations are allowed inside a class (found '" + head + "'); a class ends with 'end'");
                }
                continue;
            }

            if (head == "class") {
                needVersion(5, "'class'");
                if (w.size() < 2) fail("'class' needs a name");
                if (w[1].quoted || !isClassName(w[1].text)) fail("'class' needs a name");
                for (const ClassDecl& other : mod_.classes)
                    if (other.name == w[1].text) fail("duplicate class '" + w[1].text + "'");
                ClassDecl c;
                c.name = w[1].text;
                c.mlLine = lineNo_;
                if (w.size() > 2) {
                    needVersion(7, "'class ... extends'");
                    if (w[2].quoted || w[2].text != "extends" || w.size() < 4) fail("expected 'extends <class> ...' after the class name");
                    for (size_t k = 3; k < w.size(); ++k) {
                        if (w[k].quoted || !isClassName(w[k].text)) fail("'extends' needs class names");
                        c.supers.push_back(w[k].text);
                    }
                }
                mod_.classes.push_back(std::move(c));
                curClass = &mod_.classes.back();
            } else if (head == "global") {
                needVersion(5, "'global'");
                needArgs(w, 3);
                GlobalDecl g;
                if (w[1].quoted || !isClassName(w[1].text)) fail("'global' needs a name");
                if (w[2].quoted || !parseType(w[2].text, mod_.version, g.type) || g.type.kind == Type::Void)
                    fail(std::string("'global' needs a type (") + typeHint(mod_.version) + " or a class name)");
                for (const GlobalDecl& other : mod_.globals)
                    if (other.name == w[1].text) fail("duplicate global '" + w[1].text + "'");
                g.name = w[1].text;
                g.mlLine = lineNo_;
                mod_.globals.push_back(std::move(g));
            } else if (head == "source") {
                if (mod_.hasSource) fail("duplicate 'source' directive");
                needArgs(w, 2);
                if (!w[1].quoted) fail("'source' needs a quoted path");
                mod_.source = w[1].text;
                mod_.files.assign(1, w[1].text);
                mod_.hasSource = true;
            } else if (head == "host") {
                HostDecl h;
                h.mlLine = lineNo_;
                parseDecl(w, h.name, h.sig, "host");
                mod_.hosts.push_back(std::move(h));
            } else if (head == "func") {
                Function f;
                f.mlLine = lineNo_;
                parseDecl(w, f.name, f.sig, "func");
                mod_.funcs.push_back(std::move(f));
                cur = &mod_.funcs.back();
                funcStartLine = lineNo_;
                srcLine = 0;
                srcFile = 0;
            } else if (head == "entry") {
                if (!mod_.entry.empty()) fail("duplicate 'entry' directive");
                needArgs(w, 2);
                mod_.entry = w[1].text;
                mod_.entryLine = lineNo_;
            } else {
                fail("unknown directive '" + head + "'");
            }
        }

        if (!sawHeader) {
            lineNo_ = 1;
            fail("empty ML file: missing 'ml <version>' header");
        }
        if (cur) {
            lineNo_ = funcStartLine;
            fail("function '" + cur->name + "' is not closed (missing 'end')");
        }
        if (curClass) {
            lineNo_ = curClass->mlLine;
            fail("class '" + curClass->name + "' is not closed (missing 'end')");
        }
        return std::move(mod_);
    }

private:
    const std::string& text_;
    std::string file_;
    Module mod_;
    int lineNo_ = 0;

    [[noreturn]] void fail(const std::string& msg) const { throw MlError{"ml error", file_, lineNo_, msg}; }

    void needArgs(const std::vector<Word>& w, size_t count) const {
        if (w.size() != count) {
            fail("'" + w[0].text + "' takes " + std::to_string(count - 1) + " operand(s), got " +
                 std::to_string(w.size() - 1));
        }
    }

    // <kw> <name> ( [type {, type}] ) -> <type>
    void parseDecl(const std::vector<Word>& w, std::string& name, Sig& sig, const char* kw) const {
        if (w.size() < 5 || w[1].quoted) fail(std::string("malformed '") + kw + "' declaration");
        name = w[1].text;
        size_t i = 2;
        if (w[i].text != "(" || w[i].quoted) fail(std::string("expected '(' after the ") + kw + " name");
        ++i;
        if (i < w.size() && w[i].text == ")" && !w[i].quoted) {
            ++i;
        } else {
            for (;;) {
                Type t;
                if (i >= w.size() || w[i].quoted || !parseType(w[i].text, mod_.version, t) || t == Type::Void)
                    fail(std::string("expected a parameter type (") + typeHint(mod_.version) + ")");
                sig.params.push_back(t);
                ++i;
                if (i < w.size() && w[i].text == ",") { ++i; continue; }
                if (i < w.size() && w[i].text == ")") { ++i; break; }
                fail("expected ',' or ')' in the parameter list");
            }
        }
        if (i >= w.size() || w[i].text != "->") fail("expected '->' before the return type");
        ++i;
        if (i >= w.size() || !parseType(w[i].text, mod_.version, sig.ret))
            fail(std::string("expected a return type (") + typeHint(mod_.version) + " or void)");
        ++i;
        if (i != w.size()) fail("unexpected text after the return type");
    }

    void needVersion(int v, const std::string& what) const {
        if (mod_.version < v)
            fail(what + " requires ML version " + std::to_string(v) + " (this file declares version " +
                 std::to_string(mod_.version) + ")");
    }

    Instr instruction(const std::string& head, const std::vector<Word>& w, int srcLine, int srcFile) const {
        const OpInfo* info = nullptr;
        for (const OpInfo& i : opTable())
            if (head == i.name) info = &i;
        if (!info) fail("unknown instruction '" + head + "'");
        needVersion(info->since, "instruction '" + head + "'");

        Instr in;
        in.op = info->op;
        in.mlLine = lineNo_;
        in.srcLine = srcLine;
        in.srcFile = srcFile;
        switch (in.op) {
            case Op::PushStr:
                needArgs(w, 2);
                if (!w[1].quoted) fail("'push.str' needs a quoted string");
                in.str = w[1].text;
                break;
            case Op::PushInt: {
                needArgs(w, 2);
                long long v;
                if (!parseInt(w[1].text, v) || v < INT_MIN || v > INT_MAX)
                    fail("'push.int' needs a 32-bit integer, got '" + w[1].text + "'");
                in.ival = v;
                break;
            }
            case Op::PushBool:
                needArgs(w, 2);
                if (w[1].quoted || (w[1].text != "true" && w[1].text != "false"))
                    fail("'push.bool' needs 'true' or 'false'");
                in.ival = w[1].text == "true" ? 1 : 0;
                break;
            case Op::Load:
            case Op::Store: {
                needArgs(w, 2);
                long long n;
                if (w[1].quoted || !parseInt(w[1].text, n) || n < 0)
                    fail(std::string("'") + head + "' needs a non-negative local index");
                in.ival = n;
                break;
            }
            case Op::NewArray:
                needArgs(w, 2);
                if (w[1].quoted || !parseType(w[1].text, mod_.version, in.type) || in.type.kind == Type::Void)
                    fail("'newarray' needs an element type (" + std::string(typeHint(mod_.version)) + ")");
                in.objType = std::make_shared<const Type>(Type::arrayOf(in.type));
                break;
            case Op::ArrayLit: {
                needArgs(w, 3);
                long long n;
                if (w[1].quoted || !parseType(w[1].text, mod_.version, in.type) || in.type.kind == Type::Void)
                    fail("'arraylit' needs an element type (" + std::string(typeHint(mod_.version)) + ")");
                if (w[2].quoted || !parseInt(w[2].text, n) || n < 0)
                    fail("'arraylit' needs a non-negative element count");
                in.ival = n;
                in.objType = std::make_shared<const Type>(Type::arrayOf(in.type));
                break;
            }
            case Op::PushDbl: {
                needArgs(w, 2);
                const std::string& t = w[1].text;
                if (w[1].quoted || t.empty()) fail("'push.dbl' needs a number");
                if (t == "inf") in.dval = HUGE_VAL;
                else if (t == "-inf") in.dval = -HUGE_VAL;
                else if (t == "nan") in.dval = std::nan("");
                else {
                    char* end = nullptr;
                    in.dval = std::strtod(t.c_str(), &end);
                    if (end != t.c_str() + t.size() || t.find_first_of("xXpP") != std::string::npos || std::isnan(in.dval) || std::isinf(in.dval))
                        fail("'push.dbl' needs a decimal number (or inf, -inf, nan), got '" + t + "'");
                }
                break;
            }
            case Op::PushChr: {
                needArgs(w, 2);
                long long v;
                if (w[1].quoted || !parseInt(w[1].text, v) || v < 0 || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF))
                    fail("'push.chr' needs a Unicode code point (0 to 1114111, not a surrogate), got '" + w[1].text + "'");
                in.ival = v;
                break;
            }
            case Op::PushNull:
            case Op::NewMap:
            case Op::Unbox:
            case Op::IsType: {
                needArgs(w, 2);
                if (w[1].quoted || !parseType(w[1].text, mod_.version, in.type) || in.type.kind == Type::Void)
                    fail(std::string("'") + head + "' needs a type (" + typeHint(mod_.version) + ")");
                if (in.op == Op::NewMap && in.type.kind != Type::Map) fail("'newmap' needs a map type such as map<string,int>");
                if (in.op == Op::PushNull && in.type.kind != Type::Nullable && in.type.kind != Type::Obj)
                    fail("'push.null' needs a nullable type (T?) or obj");
                if (in.op == Op::NewMap) in.objType = std::make_shared<const Type>(in.type);
                if (in.op == Op::IsType && in.type.kind == Type::Nullable) fail("'istype' needs a type that is not nullable");
                break;
            }
            case Op::NewObj:
            case Op::Upcast:
            case Op::Downcast:
            case Op::IsInst:
            case Op::GetField:
            case Op::SetField:
            case Op::GetGlobal:
            case Op::SetGlobal:
                needArgs(w, 2);
                if (w[1].quoted || !isClassName(w[1].text)) fail(std::string("'") + head + "' needs a " + (in.op == Op::NewObj ? "class" : in.op == Op::GetField || in.op == Op::SetField ? "Class.field" : "global") + " name");
                in.str = w[1].text;
                if (in.op == Op::NewObj) {
                    in.type = Type::classOf(in.str);
                    in.objType = std::make_shared<const Type>(in.type);
                }
                if (in.op == Op::Upcast || in.op == Op::Downcast || in.op == Op::IsInst) in.type = Type::classOf(in.str);
                break;
            case Op::DynMember:
                needArgs(w, 2);
                if (!w[1].quoted || w[1].text.empty()) fail("'dyn.member' needs a quoted member name");
                in.str = w[1].text;
                break;
            case Op::DynInvoke: {
                needArgs(w, 3);
                long long n;
                if (!w[1].quoted || w[1].text.empty() || w[2].quoted || !parseInt(w[2].text, n) || n < 0 || n > 8)
                    fail("'dyn.invoke' needs a quoted method name and an argument count (0 to 8)");
                in.str = w[1].text;
                in.ival = n;
                break;
            }
            case Op::Jump:
            case Op::JumpFalse:
            case Op::Try:
                needArgs(w, 2);
                if (w[1].quoted) fail(std::string("'") + head + "' needs a label name");
                in.str = w[1].text;
                break;
            case Op::CallFn: {
                needArgs(w, 2);
                long long n;
                if (w[1].quoted || !parseInt(w[1].text, n) || n < 0) fail("'callfn' needs an argument count");
                in.ival = n;
                break;
            }
            case Op::MkClosure:
            case Op::Call:
            case Op::HostCall: {
                needArgs(w, 3);
                long long n;
                if (w[1].quoted || !parseInt(w[2].text, n) || n < 0)
                    fail(std::string("'") + head + "' needs a name and an argument count");
                in.str = w[1].text;
                in.ival = n;
                break;
            }
            default:
                needArgs(w, 1);  // no operands
                break;
        }
        return in;
    }
};

} // namespace

Module loadMl(const std::string& text, const std::string& file) { return Loader(text, file).run(); }

} // namespace pvm
