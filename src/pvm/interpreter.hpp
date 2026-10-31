#pragma once

#include <ostream>
#include <string>
#include <vector>

#include "heap.hpp"
#include "host.hpp"
#include "ml.hpp"

namespace pvm {

// Runtime failure, mapped back to the original .coy file and line when known.
struct RuntimeError {
    std::string file;
    int line = 0;
    std::string message;
};

inline std::string formatRuntimeError(const RuntimeError& e) {
    std::string where = e.file;
    if (e.line > 0) where += ":" + std::to_string(e.line);
    return where + ": runtime error: " + e.message;
}

// ---- debugging support (used by `pvm --debug`; see docs/debugging.md) ----

struct DebugFrame {
    std::string function;
    std::string file;
    int line = 0;
};

struct DebugVar {
    std::string name;
    std::string type;
    std::string value;  // short human-readable text (arrays, maps and objects are summarized)
};

// Read-only access to the paused program.
class DebugView {
public:
    virtual ~DebugView() = default;
    virtual std::vector<DebugFrame> stack() const = 0;               // innermost frame first
    virtual std::vector<DebugVar> variables(size_t frame) const = 0;  // frame 0 = innermost; hidden compiler locals are left out
    virtual std::vector<DebugVar> globals() const = 0;
};

class DebugHook {
public:
    virtual ~DebugHook() = default;
    // Called before the first instruction of every new source line (a line event: the line, file or
    // call depth differs from the previous instruction's). The hook may block. Return false to stop the program.
    virtual bool onLine(const DebugView& view, const std::string& file, int line, size_t depth) = 0;
};

// Executes the entry function of a *verified* module. Throws RuntimeError. With a hook, the program
// runs under the debugger; without one there is no overhead beyond a null check.
void run(const Module& module, const HostRegistry& registry, std::ostream& out, Heap& heap, DebugHook* hook = nullptr);

} // namespace pvm
