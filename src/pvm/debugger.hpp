// Line-oriented debug protocol over stdin/stdout (`pvm --debug`). See docs/debugging.md.
#pragma once

#include <iosfwd>
#include <streambuf>
#include <set>
#include <string>
#include <utility>

#include "interpreter.hpp"

namespace pvm {

// Escapes a string as the body of a JSON string literal (no surrounding quotes).
std::string jsonEscape(const std::string& s);

// Normalizes a path for breakpoint matching: backslashes become '/'.
std::string normalizePath(std::string p);

// True if two paths name the same file: equal after normalization, or one is a '/'-bounded suffix of the other.
bool samePath(const std::string& a, const std::string& b);

// Drives a program from commands read from `in`; reports events as JSON lines on `out`.
// The program starts paused at its first source line.
class StdioDebugger : public DebugHook {
public:
    StdioDebugger(std::istream& in, std::ostream& out) : in_(in), out_(out) {}
    bool onLine(const DebugView& view, const std::string& file, int line, size_t depth) override;

    // Writes {"event":"output","text":...} for program output.
    void output(const std::string& text);

    void event(const std::string& json);  // one line, flushed

private:
    enum class Mode { Entry, Continue, Step, Next, Out };
    std::istream& in_;
    std::ostream& out_;
    Mode mode_ = Mode::Entry;
    size_t startDepth_ = 0;
    std::set<std::pair<std::string, int>> breaks_;

    bool isBreak(const std::string& file, int line) const;
    bool commandLoop(const DebugView& view);  // false = quit
};

// Input for the program while it runs under the debugger. When the program reads and nothing is buffered, an
// {"event":"inputRequested"} line is written and debugger-protocol lines are read until `stdin <json string>`
// (one line of input, the newline is added) or `stdineof` (end of input) arrives.
class DebugInput : public std::streambuf {
public:
    DebugInput(StdioDebugger& dbg, std::istream& in) : dbg_(dbg), in_(in) {}

protected:
    int_type underflow() override;

private:
    StdioDebugger& dbg_;
    std::istream& in_;
    std::string line_;
    bool eof_ = false;
};

// Decodes the body of a JSON string literal (with its surrounding quotes) into UTF-8; false when malformed.
bool jsonUnquote(const std::string& quoted, std::string& out);

} // namespace pvm
