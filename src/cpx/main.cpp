// cpx: the Coyote project executer. Runs the workflows declared in packages.json.
// See docs/cpx.md for the exact semantics.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "manifest.hpp"
#include "process.hpp"

#ifndef COYOTE_VERSION
#define COYOTE_VERSION "0.0.0"
#endif

namespace {

using namespace coyote::pkg;

// cpx's own errors use 64 so they are not confused with the exit code of a step (which cpx passes
// through unchanged).
constexpr int kExitUsage = 64;
constexpr int kExitCannotRun = 127;

constexpr std::uintmax_t kMaxManifestBytes = 1024 * 1024;

void usage(std::ostream& os) {
    os << "cpx " COYOTE_VERSION " - Coyote project executer\n"
       << "usage:\n"
       << "  cpx [options] <workflow>   run a workflow from packages.json (for example: start, build)\n"
       << "  cpx [options] run <workflow>\n"
       << "  cpx [options] list         list the workflows\n"
       << "  cpx [options] check        validate packages.json\n"
       << "options:\n"
       << "  -C <dir>   use the project in <dir> instead of the current directory\n"
       << "  -q         do not print the steps being run\n"
       << "exit codes: 0 ok; a failing step's own exit code; 64 usage or packages.json error;\n"
       << "            127 a step's program could not be started\n";
}

std::string quoteForDisplay(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string r = "\"";
    for (char c : a) {
        if (c == '"') r += '\\';
        r += c;
    }
    return r + "\"";
}

struct Runner {
    const Manifest& manifest;
    std::string siblingDir;
    bool quiet;

    // Returns 0, or the exit code to stop with.
    int run(const std::string& name) {
        const Workflow& wf = manifest.workflows.at(name);
        for (size_t i = 0; i < wf.steps.size(); ++i) {
            const Step& st = wf.steps[i];
            int rc = 0;
            switch (st.kind) {
                case Step::Ref:
                    rc = run(st.ref);
                    break;
                case Step::Mkdir: {
                    std::error_code ec;
                    std::filesystem::create_directories(st.argv[0], ec);
                    if (ec) {
                        std::cerr << "cpx: error: cannot create directory '" << st.argv[0] << "': " << ec.message() << "\n";
                        return kExitCannotRun;
                    }
                    break;
                }
                case Step::Run: {
                    if (!quiet) {
                        std::cerr << "cpx: " << name << "> ";
                        for (size_t k = 0; k < st.argv.size(); ++k) std::cerr << (k ? " " : "") << quoteForDisplay(st.argv[k]);
                        std::cerr << "\n";
                    }
                    ProcessResult pr = runProcess(st.argv, siblingDir);
                    if (!pr.started) {
                        std::cerr << "cpx: error: workflow '" << name << "' step " << (i + 1) << ": " << pr.error << "\n";
                        return kExitCannotRun;
                    }
                    rc = pr.exitCode;
                    if (rc != 0)
                        std::cerr << "cpx: workflow '" << name << "' step " << (i + 1) << " failed with exit code " << rc << "\n";
                    break;
                }
            }
            if (rc != 0) return rc;
        }
        return 0;
    }
};

bool readManifestText(const std::string& dir, std::string& out, std::string& path) {
    path = dir == "." ? "packages.json" : (std::filesystem::path(dir) / "packages.json").string();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        std::cerr << "cpx: error: no packages.json in " << (dir == "." ? "the current directory" : "'" + dir + "'")
                  << " (use -C <dir> to choose another project)\n";
        return false;
    }
    if (std::filesystem::file_size(path, ec) > kMaxManifestBytes) {
        std::cerr << "cpx: error: '" << path << "' is larger than the 1 MiB limit\n";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    if (!in || !(ss << in.rdbuf())) {
        std::cerr << "cpx: error: cannot read '" << path << "'\n";
        return false;
    }
    out = ss.str();
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string dir = ".";
    bool quiet = false;
    std::vector<std::string> rest;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--help" || a == "-h") { usage(std::cout); return 0; }
        if (a == "--version") { std::cout << "cpx " COYOTE_VERSION "\n"; return 0; }
        if (a == "-q") { quiet = true; continue; }
        if (a == "-C") {
            if (i + 1 >= args.size()) { std::cerr << "cpx: error: '-C' needs a directory\n"; return kExitUsage; }
            dir = args[++i];
            continue;
        }
        if (!a.empty() && a[0] == '-') { std::cerr << "cpx: error: unknown option '" << a << "'\n"; return kExitUsage; }
        rest.push_back(a);
    }
    if (rest.empty()) { usage(std::cerr); return kExitUsage; }

    std::string command = rest[0], workflow;
    if (command == "run") {
        if (rest.size() != 2) { std::cerr << "cpx: error: 'run' needs exactly one workflow name\n"; return kExitUsage; }
        workflow = rest[1];
    } else if (command == "list" || command == "check") {
        if (rest.size() != 1) { std::cerr << "cpx: error: '" << command << "' takes no arguments\n"; return kExitUsage; }
    } else {
        if (rest.size() != 1) { std::cerr << "cpx: error: unexpected argument '" << rest[1] << "'\n"; return kExitUsage; }
        workflow = command;
        command = "run";
    }

    std::string text, path;
    if (!readManifestText(dir, text, path)) return kExitUsage;
    Manifest manifest;
    try {
        manifest = loadManifest(text);
    } catch (const ManifestError& e) {
        std::cerr << path;
        if (e.line > 0) std::cerr << ":" << e.line << ":" << e.col;
        std::cerr << ": error: " << e.message << "\n";
        return kExitUsage;
    }

    if (command == "check") {
        std::cout << "packages.json ok: " << manifest.name << (manifest.version.empty() ? "" : " " + manifest.version)
                  << ", entry " << manifest.entry << ", " << manifest.workflows.size() << " workflow(s)\n";
        return 0;
    }
    if (command == "list") {
        for (const auto& w : manifest.workflows)
            std::cout << w.first << (w.second.synthesized ? "  (default)" : "") << "\n";
        return 0;
    }

    if (!manifest.workflows.count(workflow)) {
        std::cerr << "cpx: error: no workflow named '" << workflow << "' (see 'cpx list')\n";
        return kExitUsage;
    }
    std::error_code ec;
    std::filesystem::current_path(dir, ec);
    if (ec) { std::cerr << "cpx: error: cannot enter '" << dir << "': " << ec.message() << "\n"; return kExitUsage; }

    Runner r{manifest, executableDir(argv[0]), quiet};
    return r.run(workflow);
}
