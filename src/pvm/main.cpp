// pvm: the Pixel Virtual Machine CLI. Loads, verifies and executes a textual ML program.
#include <cstdio>
#include <fstream>
#include <iostream>
#include <new>
#include <sstream>
#include <string>
#include <vector>

#include "debugger.hpp"
#include "heap.hpp"
#include "host.hpp"
#include "interpreter.hpp"
#include "loader.hpp"
#include "verifier.hpp"

#ifndef COYOTE_VERSION
#define COYOTE_VERSION "0.0.0"
#endif

namespace {

void usage(std::ostream& os) {
    os << "Pixel Virtual Machine " COYOTE_VERSION " (supports ML format versions 1 to 6)\n"
       << "usage: pvm [options] <program.ml>\n"
       << "  --verify-only        load and verify the program without running it\n"
       << "  --allow <permission> grant a host permission (default: console, time, random; also: file, system, net)\n"
       << "  --allow-all          grant every permission\n"
       << "  --unbuffered         flush output after every write\n"
       << "  -- <args...>         program arguments, readable with sys.arg\n"
       << "  --heap-limit <n>     maximum live array elements (default 8000000)\n"
       << "  --gc-stress          collect garbage before every allocation (testing)\n"
       << "  --gc-stats           print garbage collector statistics to stderr on exit\n"
       << "  --debug              run under the line debugger: commands on stdin, JSON events on stdout (docs/debugging.md)\n"
       << "exit codes: 0 ok, 1 ML/verification/runtime error, 2 usage or I/O error\n";
}

// Turns program output into debugger "output" events (flushed at every newline).
class EventBuf : public std::streambuf {
public:
    explicit EventBuf(pvm::StdioDebugger& d) : d_(d) {}
    void flushNow() { d_.output(buf_); buf_.clear(); }
protected:
    int_type overflow(int_type c) override {
        if (c == traits_type::eof()) return traits_type::not_eof(c);
        buf_ += static_cast<char>(c);
        if (c == '\n' || buf_.size() > 4096) flushNow();
        return c;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        for (std::streamsize i = 0; i < n; ++i) overflow(static_cast<unsigned char>(s[i]));
        return n;
    }
    int sync() override { return 0; }
private:
    pvm::StdioDebugger& d_;
    std::string buf_;
};

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) { usage(std::cerr); return 2; }

    bool verifyOnly = false;
    bool gcStats = false;
    bool debug = false;
    std::vector<std::string> progArgs;
    pvm::HeapOptions heapOptions;
    std::string input;
    pvm::HostRegistry registry;
    pvm::registerAllHosts(registry);
    pvm::grantDefaultPermissions(registry);
    registry.setInput(&std::cin);

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--help" || a == "-h") { usage(std::cout); return 0; }
        if (a == "--version") { std::cout << "pvm " COYOTE_VERSION "\n"; return 0; }
        if (a == "--verify-only") { verifyOnly = true; continue; }
        if (a == "--gc-stress") { heapOptions.stress = true; continue; }
        if (a == "--gc-stats") { gcStats = true; continue; }
        if (a == "--debug") { debug = true; continue; }
        if (a == "--unbuffered") { registry.setUnbuffered(true); continue; }
        if (a == "--allow-all") { pvm::grantAllPermissions(registry); continue; }
        if (a == "--") { progArgs.assign(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end()); break; }
        if (a == "--heap-limit") {
            if (i + 1 >= args.size()) { std::cerr << "pvm: error: '--heap-limit' needs a value\n"; return 2; }
            const std::string& v = args[++i];
            if (v.empty() || v.size() > 12 || v.find_first_not_of("0123456789") != std::string::npos) {
                std::cerr << "pvm: error: '--heap-limit' needs a non-negative integer, got '" << v << "'\n";
                return 2;
            }
            heapOptions.maxElements = static_cast<size_t>(std::stoull(v));
            continue;
        }
        if (a == "--allow") {
            if (i + 1 >= args.size()) { std::cerr << "pvm: error: '--allow' needs a value\n"; return 2; }
            registry.grant(args[++i]);
            continue;
        }
        if (!a.empty() && a[0] == '-') { std::cerr << "pvm: error: unknown option '" << a << "'\n"; return 2; }
        if (!input.empty()) { std::cerr << "pvm: error: only one program file is supported\n"; return 2; }
        input = a;
    }
    if (input.empty()) { std::cerr << "pvm: error: no program file\n"; return 2; }

    std::ifstream in(input, std::ios::binary);
    if (!in) { std::cerr << "pvm: error: cannot read '" << input << "'\n"; return 2; }
    // The loader keeps every instruction in memory (roughly ten times the file size).
    constexpr std::streamoff kMaxProgramBytes = 32 * 1024 * 1024;
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size > kMaxProgramBytes) {
        std::cerr << "pvm: error: '" << input << "' is larger than the " << kMaxProgramBytes / (1024 * 1024)
                  << " MiB program file limit\n";
        return 2;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();

    registry.setArgs(progArgs);
    pvm::Heap heap(heapOptions);
    auto printStats = [&]() {
        if (gcStats)
            std::cerr << "gc: " << heap.collections() << " collection(s), " << heap.freedObjects()
                      << " object(s) freed, " << heap.liveObjects() << " live at exit\n";
    };
    try {
        pvm::Module module = pvm::loadMl(text, input);
        pvm::verify(module, registry, input);
        if (verifyOnly) return 0;
        if (debug) {
            // stdin carries debugger commands; program input arrives as `stdin "<json>"` commands (docs/debugging.md)
            pvm::StdioDebugger dbg(std::cin, std::cout);
            pvm::DebugInput dbgInputBuf(dbg, std::cin);
            std::istream dbgInput(&dbgInputBuf);
            registry.setInput(&dbgInput);
            EventBuf buf(dbg);
            std::ostream progOut(&buf);
            int code = 0;
            try {
                pvm::run(module, registry, progOut, heap, &dbg);
            } catch (const pvm::HostExit& x) {
                code = x.code;
            } catch (const pvm::RuntimeError& e) {
                buf.flushNow();
                dbg.event("{\"event\":\"runtimeError\",\"file\":\"" + pvm::jsonEscape(e.file) + "\",\"line\":" + std::to_string(e.line) +
                          ",\"message\":\"" + pvm::jsonEscape(e.message) + "\"}");
                code = 1;
            }
            buf.flushNow();
            dbg.event("{\"event\":\"exited\",\"code\":" + std::to_string(code) + "}");
            printStats();
            return code;
        }
        try {
            pvm::run(module, registry, std::cout, heap);
        } catch (const pvm::HostExit& x) {
            std::cout.flush();
            printStats();
            return x.code;
        }
        std::cout.flush();
        printStats();
        if (!std::cout) { std::cerr << "pvm: error: failed to flush standard output\n"; return 1; }
    } catch (const pvm::MlError& e) {
        std::cerr << pvm::formatMlError(e) << "\n";
        return 1;
    } catch (const pvm::RuntimeError& e) {
        std::cout.flush();
        std::cerr << pvm::formatRuntimeError(e) << "\n";
        printStats();
        return 1;
    } catch (const std::bad_alloc&) {
        std::cout.flush();
        std::cerr << "pvm: error: out of memory\n";
        return 1;
    }
    return 0;
}
