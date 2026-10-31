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

} // namespace pvm
