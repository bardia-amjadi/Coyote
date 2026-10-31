#include "libpack.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "lib.hpp"
#include "manifest.hpp"
#include "modules.hpp"
#include "sema.hpp"

namespace coyote {

namespace {

namespace fs = std::filesystem;

bool readFile(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

void usage(std::ostream& os) {
    os << "usage:\n"
       << "  coyote lib pack <project-dir> [-o <file.lib>]   pack a library project (packages.json with \"type\": \"library\")\n"
       << "  coyote lib info <file.lib>                      show a library's name, version and files\n";
}

int info(const std::string& path, std::ostream& out, std::ostream& err) {
    std::string text;
    if (!readFile(path, text)) { err << "coyote: error: cannot read '" << path << "'\n"; return 2; }
    try {
        pkg::Lib lib = pkg::parseLib(text);
        out << "name:    " << lib.name << "\n";
        out << "version: " << (lib.version.empty() ? "(none)" : lib.version) << "\n";
        out << "files:   " << lib.files.size() << "\n";
        for (const pkg::LibFile& f : lib.files) out << "  " << f.path << " (" << f.text.size() << " bytes)\n";
        return 0;
    } catch (const pkg::LibError& e) {
        err << "coyote: error: " << path << ": " << e.message << "\n";
        return 1;
    }
}

int pack(const std::string& dirArg, std::string outPath, std::ostream& out, std::ostream& err) {
    std::error_code ec;
    fs::path dir = dirArg;
    if (!fs::is_directory(dir, ec)) { err << "coyote: error: '" << dirArg << "' is not a directory\n"; return 2; }
    std::string manifestText;
    if (!readFile(dir / "packages.json", manifestText)) { err << "coyote: error: " << dirArg << " has no packages.json\n"; return 2; }
    pkg::Manifest m;
    try {
        m = pkg::loadManifest(manifestText);
    } catch (const pkg::ManifestError& e) {
        err << "coyote: error: packages.json:" << e.line << ":" << e.col << ": " << e.message << "\n";
        return 1;
    }
    if (m.type != "library") {
        err << "coyote: error: packages.json must contain \"type\": \"library\" to be packed (it is \"" << m.type << "\")\n";
        return 1;
    }

    fs::path root = fs::is_directory(dir / "src", ec) ? dir / "src" : dir;
    pkg::Lib lib;
    lib.name = m.name;
    lib.version = m.version;
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory() && (name == ".coyote" || name == ".git" || name == "out")) { it.disable_recursion_pending(); continue; }
        if (it->is_regular_file() && it->path().extension() == ".coy") files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files) {
        std::string text;
        if (!readFile(f, text)) { err << "coyote: error: cannot read '" << f.string() << "'\n"; return 2; }
        lib.files.push_back(pkg::LibFile{fs::relative(f, root).generic_string(), std::move(text)});
    }
    std::string packed;
    try {
        packed = pkg::writeLib(lib);
    } catch (const pkg::LibError& e) {
        err << "coyote: error: " << e.message << " (looked in " << root.string() << ")\n";
        return 1;
    }

    // Validate: compile a tiny program that uses the library; every file must parse and type-check.
    fs::path tmp = fs::temp_directory_path(ec) / ("coyote-pack-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tmp, ec);
    std::string libName = "check.lib";
    { std::ofstream(tmp / libName, std::ios::binary) << packed; }
    { std::ofstream(tmp / "check.coy", std::ios::binary) << "use \"./check.lib\"\n"; }
    Program prog;
    std::vector<Diagnostic> diags;
    std::string readError;
    bool ok = loadProgram((tmp / "check.coy").string(), prog, diags, readError);
    if (ok) ok = analyze(prog, (tmp / "check.coy").string(), diags);
    if (!ok) {
        const std::string prefix = (tmp / libName).string() + "/";
        for (Diagnostic d : diags) {
            if (d.file.compare(0, prefix.size(), prefix) == 0) d.file = (root / d.file.substr(prefix.size())).string();
            err << formatDiagnostic(d) << "\n";
        }
        err << "coyote: error: library '" << lib.name << "' has " << diags.size() << " error(s); nothing was written\n";
        fs::remove_all(tmp, ec);
        return 1;
    }
    fs::remove_all(tmp, ec);

    if (outPath.empty()) {
        std::string lower = lib.name;
        for (char& c : lower) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        outPath = lower + ".lib";
    }
    std::ofstream o(outPath, std::ios::binary);
    if (!o || !(o << packed)) { err << "coyote: error: cannot write '" << outPath << "'\n"; return 2; }
    out << "packed " << lib.name << (lib.version.empty() ? "" : " " + lib.version) << " (" << lib.files.size() << " file(s)) -> " << outPath << "\n";
    return 0;
}

} // namespace

int libCommand(int argc, char** argv, std::ostream& out, std::ostream& err) {
    std::vector<std::string> a(argv, argv + argc);  // a[0] is the subcommand
    if (a.empty()) { usage(err); return 2; }
    if (a[0] == "info" && a.size() == 2) return info(a[1], out, err);
    if (a[0] == "pack") {
        std::string dir, output;
        for (size_t i = 1; i < a.size(); ++i) {
            if (a[i] == "-o") {
                if (i + 1 >= a.size()) { err << "coyote: error: '-o' needs a value\n"; return 2; }
                output = a[++i];
            } else if (dir.empty() && !a[i].empty() && a[i][0] != '-') {
                dir = a[i];
            } else {
                err << "coyote: error: unexpected argument '" << a[i] << "'\n";
                usage(err);
                return 2;
            }
        }
        if (dir.empty()) { usage(err); return 2; }
        return pack(dir, output, out, err);
    }
    usage(err);
    return 2;
}

} // namespace coyote
