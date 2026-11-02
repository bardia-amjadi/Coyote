#include "debugger.hpp"

#include <istream>
#include <ostream>
#include <sstream>

namespace pvm {

std::string jsonEscape(const std::string& s) {
    std::string r;
    for (unsigned char c : s) {
        switch (c) {
            case '"': r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    r += b;
                } else {
                    r += static_cast<char>(c);
                }
        }
    }
    return r;
}

std::string normalizePath(std::string p) {
    for (char& c : p)
        if (c == '\\') c = '/';
    return p;
}

bool samePath(const std::string& a, const std::string& b) {
    std::string x = normalizePath(a), y = normalizePath(b);
    if (x == y) return true;
    auto suffix = [](const std::string& longer, const std::string& shorter) {
        return longer.size() > shorter.size() && longer.compare(longer.size() - shorter.size(), shorter.size(), shorter) == 0 &&
               longer[longer.size() - shorter.size() - 1] == '/';
    };
    return suffix(x, y) || suffix(y, x);
}

void StdioDebugger::event(const std::string& json) { out_ << json << "\n" << std::flush; }

void StdioDebugger::output(const std::string& text) {
    if (!text.empty()) event("{\"event\":\"output\",\"text\":\"" + jsonEscape(text) + "\"}");
}

bool StdioDebugger::isBreak(const std::string& file, int line) const {
    for (const auto& b : breaks_)
        if (b.second == line && samePath(b.first, file)) return true;
    return false;
}

static std::string varsJson(const std::vector<DebugVar>& vars) {
    std::string r = "[";
    for (size_t i = 0; i < vars.size(); ++i)
        r += (i ? "," : "") + std::string("{\"name\":\"") + jsonEscape(vars[i].name) + "\",\"type\":\"" + jsonEscape(vars[i].type) +
             "\",\"value\":\"" + jsonEscape(vars[i].value) + "\"}";
    return r + "]";
}

bool StdioDebugger::onLine(const DebugView& view, const std::string& file, int line, size_t depth) {
    const char* reason = nullptr;
    if (mode_ == Mode::Entry) reason = "entry";
    else if (isBreak(file, line)) reason = "breakpoint";
    else if (mode_ == Mode::Step) reason = "step";
    else if (mode_ == Mode::Next && depth <= startDepth_) reason = "step";
    else if (mode_ == Mode::Out && depth < startDepth_) reason = "step";
    if (!reason) return true;

    std::vector<DebugFrame> st = view.stack();
    event(std::string("{\"event\":\"stopped\",\"reason\":\"") + reason + "\",\"file\":\"" + jsonEscape(file) + "\",\"line\":" +
          std::to_string(line) + ",\"function\":\"" + jsonEscape(st.empty() ? "" : st[0].function) + "\"}");
    startDepth_ = depth;
    return commandLoop(view);
}

bool StdioDebugger::commandLoop(const DebugView& view) {
    std::string cmdLine;
    while (std::getline(in_, cmdLine)) {
        if (!cmdLine.empty() && cmdLine.back() == '\r') cmdLine.pop_back();
        std::istringstream ss(cmdLine);
        std::string cmd;
        ss >> cmd;
        if (cmd.empty()) continue;
        if (cmd == "break" || cmd == "unbreak") {
            int line = 0;
            std::string rest;
            std::getline(ss, rest);
            size_t sp = rest.find_last_of(' ');
            if (sp == std::string::npos || (line = std::atoi(rest.c_str() + sp + 1)) <= 0) {
                event("{\"event\":\"error\",\"message\":\"usage: " + cmd + " <file> <line>\"}");
                continue;
            }
            size_t start = rest.find_first_not_of(' ');
            std::string file = rest.substr(start == std::string::npos ? 0 : start, sp - (start == std::string::npos ? 0 : start));
            if (cmd == "break") breaks_.insert({file, line});
            else breaks_.erase({file, line});
            event("{\"event\":\"breakpoint\",\"file\":\"" + jsonEscape(file) + "\",\"line\":" + std::to_string(line) + ",\"set\":" +
                  (cmd == "break" ? "true" : "false") + "}");
        } else if (cmd == "clearbreaks") {
            breaks_.clear();
            event("{\"event\":\"breakpoints-cleared\"}");
        } else if (cmd == "continue") {
            mode_ = Mode::Continue;
            return true;
        } else if (cmd == "step") {
            mode_ = Mode::Step;
            return true;
        } else if (cmd == "next") {
            mode_ = Mode::Next;
            return true;
        } else if (cmd == "out") {
            mode_ = Mode::Out;
            return true;
        } else if (cmd == "stack") {
            std::string r = "{\"event\":\"stack\",\"frames\":[";
            std::vector<DebugFrame> st = view.stack();
            for (size_t i = 0; i < st.size(); ++i)
                r += (i ? "," : "") + std::string("{\"function\":\"") + jsonEscape(st[i].function) + "\",\"file\":\"" +
                     jsonEscape(st[i].file) + "\",\"line\":" + std::to_string(st[i].line) + "}";
            event(r + "]}");
        } else if (cmd == "vars") {
            size_t frame = 0;
            ss >> frame;
            event("{\"event\":\"vars\",\"frame\":" + std::to_string(frame) + ",\"vars\":" + varsJson(view.variables(frame)) + "}");
        } else if (cmd == "globals") {
            event("{\"event\":\"globals\",\"vars\":" + varsJson(view.globals()) + "}");
        } else if (cmd == "quit") {
            return false;
        } else {
            event("{\"event\":\"error\",\"message\":\"unknown command '" + jsonEscape(cmd) + "'\"}");
        }
    }
    return false;  // stdin closed: stop the program
}

bool jsonUnquote(const std::string& q, std::string& out) {
    out.clear();
    size_t i = 0;
    while (i < q.size() && q[i] == ' ') ++i;
    if (i >= q.size() || q[i] != '"') return false;
    ++i;
    auto hex4 = [&](size_t at, unsigned& v) {
        if (at + 4 > q.size()) return false;
        v = 0;
        for (size_t k = 0; k < 4; ++k) {
            char c = q[at + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    };
    auto utf8 = [&](unsigned cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    };
    for (; i < q.size(); ++i) {
        char c = q[i];
        if (c == '"') return true;
        if (c != '\\') { out += c; continue; }
        if (++i >= q.size()) return false;
        switch (q[i]) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case '/': out += '/'; break;
            case '\\': out += '\\'; break;
            case '"': out += '"'; break;
            case 'u': {
                unsigned v = 0;
                if (!hex4(i + 1, v)) return false;
                i += 4;
                if (v >= 0xD800 && v < 0xDC00 && i + 6 < q.size() + 0 && q[i + 1] == '\\' && q[i + 2] == 'u') {
                    unsigned lo = 0;
                    if (hex4(i + 3, lo) && lo >= 0xDC00 && lo < 0xE000) { v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
                }
                utf8(v);
                break;
            }
            default: return false;
        }
    }
    return false;
}

DebugInput::int_type DebugInput::underflow() {
    if (eof_) return traits_type::eof();
    dbg_.event("{\"event\":\"inputRequested\"}");
    std::string cmdLine;
    while (std::getline(in_, cmdLine)) {
        if (!cmdLine.empty() && cmdLine.back() == '\r') cmdLine.pop_back();
        if (cmdLine.rfind("stdin ", 0) == 0) {
            std::string text;
            if (!jsonUnquote(cmdLine.substr(6), text)) {
                dbg_.event("{\"event\":\"error\",\"message\":\"stdin needs a JSON string\"}");
                continue;
            }
            line_ = text + "\n";
            setg(&line_[0], &line_[0], &line_[0] + line_.size());
            return traits_type::to_int_type(line_[0]);
        }
        if (cmdLine == "stdineof" || cmdLine == "quit") break;
    }
    eof_ = true;
    return traits_type::eof();
}

} // namespace pvm
