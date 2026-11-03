// coyote: the Coyote compiler CLI. .coy source -> lexer -> parser -> AST -> sema -> textual ML.
// It knows nothing about the PVM; the two communicate only through the ML text (docs/ml-spec.md).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "lexer.hpp"
#include "libpack.hpp"
#include "process.hpp"
#include "mlgen.hpp"
#include "modules.hpp"
#include "parser.hpp"
#include "sema.hpp"

#ifndef COYOTE_VERSION
#define COYOTE_VERSION "0.0.0"
#endif

namespace {

void usage(std::ostream& os) {
    os << "Coyote compiler " COYOTE_VERSION " (ML format version " << coyote::kMlVersion << ")\n"
       << "usage:\n"
       << "  coyote build <file.coy> [-o <out.ml>]   compile to textual ML ('-o -' writes to stdout)\n"
       << "  coyote check <file.coy>                 report diagnostics without writing output\n"
       << "  coyote run <file.coy> [-- args...]      compile and run on the PVM (all permissions; args via Environment.Arg)\n"
       << "  coyote lib pack <project-dir> [-o <file.lib>] | lib info <file.lib>   build / inspect a library (docs/libraries.md)\n"
       << "  coyote pvm|cpx|cpm|cdt|llvm <args...>  run the named tool of this toolchain\n"
       << "  (build and run: --keep-unused keeps methods that are never called; by default they are removed)\n"
       << "  coyote --version | --help\n"
       << "default output for 'build' is <name>.ml in the current directory.\n";
}

std::string defaultOutput(const std::string& src) {
    size_t slash = src.find_last_of("/\\");
    std::string base = slash == std::string::npos ? src : src.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) base = base.substr(0, dot);
    return base + ".ml";
}

// Returns 0 on success, 1 on compile errors, 2 on usage/IO errors.
bool g_keepUnused = false;  // --keep-unused: do not remove unreachable methods

int compile(const std::string& path, bool write, const std::string& outPath) {
    coyote::Program prog;
    std::vector<coyote::Diagnostic> diags;
    std::string readError;
    if (!coyote::loadProgram(path, prog, diags, readError)) {
        if (!readError.empty()) {
            std::cerr << "coyote: error: " << readError << "\n";
            return 2;
        }
        for (const auto& d : diags) std::cerr << coyote::formatDiagnostic(d) << "\n";
        if (diags.size() > 1) std::cerr << diags.size() << " error(s)\n";
        return 1;
    }
    if (!coyote::analyze(prog, path, diags)) {
        for (const auto& d : diags) std::cerr << coyote::formatDiagnostic(d) << "\n";
        std::cerr << diags.size() << " error(s)\n";
        return 1;
    }
    if (!write) return 0;

    std::string ml = coyote::generateMl(prog, path, !g_keepUnused);
    if (outPath == "-") {
        std::cout << ml;
        std::cout.flush();
        return std::cout ? 0 : 2;
    }
    std::ofstream out(outPath, std::ios::binary);
    if (!out || !(out << ml)) {
        std::cerr << "coyote: error: cannot write '" << outPath << "'\n";
        return 2;
    }
    return 0;
}

} // namespace

// `coyote run`: compiles to a temporary ML file and runs it with the sibling `pvm`. The PVM's exit
// code is returned. The compiler and the PVM still share nothing but the ML file.
int runSource(const std::string& input, const std::string& selfDir, const std::vector<std::string>& progArgs) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec);
    if (ec) { std::cerr << "coyote: error: no temporary directory available\n"; return 2; }
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path ml = dir / ("coyote-run-" + std::to_string(stamp) + "-" + std::to_string(std::rand()) + ".ml");
    int rc = compile(input, true, ml.string());
    if (rc != 0) { fs::remove(ml, ec); return rc; }
    coyote::pkg::ProcessResult r = [&] {
        // `run` is for programs the user wrote and started: every permission, output shown as it is produced.
        std::vector<std::string> cmd = {"pvm", "--allow-all", "--unbuffered", ml.string()};
        if (!progArgs.empty()) { cmd.push_back("--"); cmd.insert(cmd.end(), progArgs.begin(), progArgs.end()); }
        return coyote::pkg::runProcess(cmd, selfDir);
    }();
    fs::remove(ml, ec);
    if (!r.started) { std::cerr << "coyote: error: " << r.error << "\n"; return 127; }
    return r.exitCode;
}

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) { usage(std::cerr); return 2; }
    if (args[0] == "--help" || args[0] == "-h") { usage(std::cout); return 0; }
    if (args[0] == "--version") { std::cout << "coyote " COYOTE_VERSION "\n"; return 0; }

    const std::string& cmd = args[0];
    if (cmd == "pvm" || cmd == "cpx" || cmd == "cpm" || cmd == "cdt" || cmd == "llvm") {
        std::vector<std::string> sub(args.begin(), args.end());
        if (cmd == "llvm") sub[0] = "coyote-llvm";
        coyote::pkg::ProcessResult r = coyote::pkg::runProcess(sub, coyote::pkg::executableDir(argv[0]));
        if (!r.started) { std::cerr << "coyote: error: " << r.error << "\n"; return 127; }
        return r.exitCode;
    }
    if (cmd == "lib") return coyote::libCommand(argc - 2, argv + 2, std::cout, std::cerr);
    if (cmd != "build" && cmd != "check" && cmd != "run") {
        std::cerr << "coyote: error: unknown command '" << cmd << "'\n";
        usage(std::cerr);
        return 2;
    }
    std::string input, output;
    std::vector<std::string> progArgs;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--" && cmd == "run") { progArgs.assign(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end()); break; }
        if (args[i] == "-o" && cmd == "build") {
            if (i + 1 >= args.size()) { std::cerr << "coyote: error: '-o' needs a value\n"; return 2; }
            output = args[++i];
        } else if (args[i] == "--keep-unused") {
            g_keepUnused = true;
        } else if (!args[i].empty() && args[i][0] == '-' && args[i] != "-") {
            std::cerr << "coyote: error: unknown option '" << args[i] << "'\n";
            return 2;
        } else if (input.empty()) {
            input = args[i];
        } else {
            std::cerr << "coyote: error: only one input file is supported\n";
            return 2;
        }
    }
    if (input.empty()) { std::cerr << "coyote: error: no input file\n"; return 2; }
    if (output.empty()) output = defaultOutput(input);
    if (cmd == "run") return runSource(input, coyote::pkg::executableDir(argv[0]), progArgs);
    return compile(input, cmd == "build", output);
}
