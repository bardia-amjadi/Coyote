// cpm: the Coyote package manager. Resolves the dependencies declared in packages.json, writes a
// deterministic lock file and installs the packages into .coyote/packages. See docs/cpm.md.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "manifest.hpp"
#include "process.hpp"
#include "resolver.hpp"

#ifndef COYOTE_VERSION
#define COYOTE_VERSION "0.0.0"
#endif

namespace fs = std::filesystem;
using namespace coyote::pkg;

namespace {

constexpr int kExitFailure = 1;  // resolution, provider, lock or install failure
constexpr int kExitUsage = 64;   // usage error or invalid packages.json

void usage(std::ostream& os) {
    os << "cpm " COYOTE_VERSION " - Coyote package manager\n"
       << "usage: cpm [-C dir] <command> [options]\n"
       << "commands:\n"
       << "  install [--locked]   install the packages in packages.lock.json (resolving and writing it if it\n"
       << "                       is missing or out of date; --locked fails instead of changing it)\n"
       << "  update               re-resolve everything, rewrite the lock file and install\n"
       << "  lock                 re-resolve and write the lock file without installing\n"
       << "  list                 list the locked packages\n"
       << "  verify               check the installed packages against the lock file\n"
       << "  init [name] [--structured|--library]  create packages.json and app.coy, or a library project (never overwrites)\n"
       << "  layout --structured [--apply]  show (or with --apply, perform) the move of root .coy files into src/\n"
       << "exit codes: 0 ok; 1 operation failed; 64 usage error or invalid packages.json\n";
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeFile(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    return out && (out << text) && out.flush();
}

struct Project {
    fs::path dir;
    Manifest manifest;
    fs::path lockPath() const { return dir / "packages.lock.json"; }
    fs::path packagesDir() const { return dir / ".coyote" / "packages"; }
};

// Returns 0 or the exit code to stop with.
int loadProject(const std::string& dir, Project& p) {
    p.dir = dir;
    fs::path file = p.dir / "packages.json";
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        std::cerr << "cpm: error: no packages.json in " << (dir == "." ? "the current directory" : "'" + dir + "'") << " (use 'cpm init' to create one)\n";
        return kExitUsage;
    }
    if (fs::file_size(file, ec) > 1024 * 1024) {
        std::cerr << "cpm: error: packages.json is larger than the 1 MiB limit\n";
        return kExitUsage;
    }
    const std::string shown = dir == "." ? "packages.json" : file.string();
    try {
        p.manifest = loadManifest(readFile(file));
    } catch (const ManifestError& e) {
        std::cerr << shown;
        if (e.line > 0) std::cerr << ":" << e.line << ":" << e.col;
        std::cerr << ": error: " << e.message << "\n";
        return kExitUsage;
    }
    return 0;
}

std::string describeSource(const SourceInfo& s) {
    switch (s.type) {
        case SourceInfo::Path: return "path " + s.path;
        case SourceInfo::Git: return "git " + s.url + " @ " + s.commit;
        case SourceInfo::Url: return "url " + s.url;
    }
    return "";
}

bool readLock(const Project& p, Lock& lock, bool& exists, std::string& text) {
    std::error_code ec;
    exists = fs::is_regular_file(p.lockPath(), ec);
    if (!exists) return true;
    text = readFile(p.lockPath());
    try {
        lock = lockFromJson(text);
    } catch (const LockError& e) {
        std::cerr << "cpm: error: packages.lock.json is invalid: " << e.message << " (delete it and run 'cpm lock' to recreate it)\n";
        return false;
    }
    return true;
}

// mode: Install uses an up-to-date lock as is; Update ignores the lock; LockOnly resolves and writes the lock only.
enum class Mode { Install, Update, LockOnly };

int runResolve(Project& p, Mode mode, bool locked, ProviderSet& providers) {
    Lock lock;
    bool haveLock = false;
    std::string oldText;
    if (!readLock(p, lock, haveLock, oldText)) return kExitFailure;

    std::map<std::string, std::unique_ptr<Fetched>> fetched;
    const bool noDeps = p.manifest.dependencies.empty();
    const bool matches = haveLock && lock.requested == requestedOf(p.manifest);
    bool wroteLock = false;

    try {
        if (mode == Mode::Install && matches) {
            fetchLocked(lock, p.dir, providers, fetched);
        } else {
            if (mode == Mode::Install && locked && !(noDeps && !haveLock)) {
                std::cerr << "cpm: error: packages.json and packages.lock.json are out of sync; run 'cpm lock' (or 'cpm install' without --locked)\n";
                return kExitFailure;
            }
            if (!(noDeps && !haveLock && mode != Mode::LockOnly)) {
                lock = resolve(p.manifest, p.dir, providers, fetched);
                std::string text = lockToJson(lock);
                if (!haveLock || text != oldText) {
                    if (!writeFile(p.lockPath(), text)) {
                        std::cerr << "cpm: error: cannot write packages.lock.json\n";
                        return kExitFailure;
                    }
                    wroteLock = true;
                }
            }
        }
    } catch (const ResolveError& e) {
        std::cerr << "cpm: error: " << e.message << "\n";
        return kExitFailure;
    } catch (const ProviderError& e) {
        std::cerr << "cpm: error: " << e.message << "\n";
        return kExitFailure;
    }

    if (mode != Mode::LockOnly) {
        std::error_code ec;
        fs::create_directories(p.packagesDir(), ec);
        try {
            for (const LockedPackage& lp : lock.packages) copyTree(fetched.at(lp.name)->dir, p.packagesDir() / lp.name);
        } catch (const ResolveError& e) {
            std::cerr << "cpm: error: " << e.message << "\n";
            return kExitFailure;
        }
        // Everything under .coyote/packages is managed by cpm: remove what the lock no longer lists.
        std::vector<std::string> keep;
        for (const LockedPackage& lp : lock.packages) keep.push_back(lp.name);
        for (const auto& entry : fs::directory_iterator(p.packagesDir(), ec)) {
            std::string n = entry.path().filename().string();
            if (std::find(keep.begin(), keep.end(), n) == keep.end()) fs::remove_all(entry.path(), ec);
        }
    }

    for (const LockedPackage& lp : lock.packages)
        std::cout << (mode == Mode::LockOnly ? "locked " : "installed ") << lp.name << " " << (lp.version.empty() ? "(no version)" : lp.version)
                  << " [" << describeSource(lp.source) << "]\n";
    std::cout << lock.packages.size() << " package(s); packages.lock.json " << (wroteLock ? "written" : "unchanged") << "\n";
    return 0;
}

int cmdList(const Project& p) {
    Lock lock;
    bool exists = false;
    std::string text;
    if (!readLock(p, lock, exists, text)) return kExitFailure;
    if (!exists) {
        std::cout << "no packages.lock.json (run 'cpm lock')\n";
        return 0;
    }
    for (const LockedPackage& lp : lock.packages)
        std::cout << lp.name << " " << (lp.version.empty() ? "-" : lp.version) << " [" << describeSource(lp.source) << "]\n";
    return 0;
}

int cmdVerify(const Project& p) {
    Lock lock;
    bool exists = false;
    std::string text;
    if (!readLock(p, lock, exists, text)) return kExitFailure;
    if (!exists) {
        std::cerr << "cpm: error: no packages.lock.json\n";
        return kExitFailure;
    }
    int bad = 0;
    for (const LockedPackage& lp : lock.packages) {
        fs::path dir = p.packagesDir() / lp.name;
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            std::cout << "missing   " << lp.name << "\n";
            ++bad;
            continue;
        }
        try {
            if (hashTree(dir) != lp.integrity) {
                std::cout << "modified  " << lp.name << "\n";
                ++bad;
                continue;
            }
        } catch (const ResolveError& e) {
            std::cout << "invalid   " << lp.name << ": " << e.message << "\n";
            ++bad;
            continue;
        }
        std::cout << "ok        " << lp.name << "\n";
    }
    return bad ? kExitFailure : 0;
}

std::string sanitizeName(const std::string& s) {
    std::string r;
    for (char c : s) r += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') ? c : '-';
    if (r.empty() || r.size() > 64) r = "app";
    return r;
}

int cmdInit(const std::string& dir, std::vector<std::string> args) {
    bool structured = false, library = false;
    std::string name;
    for (const std::string& a : args) {
        if (a == "--structured") structured = true;
        else if (a == "--library") library = true;
        else if (!a.empty() && a[0] == '-') { std::cerr << "cpm: error: unknown option '" << a << "'\n"; return kExitUsage; }
        else if (name.empty()) name = a;
        else { std::cerr << "cpm: error: only one project name is allowed\n"; return kExitUsage; }
    }
    if (library && structured) { std::cerr << "cpm: error: --library and --structured cannot be combined\n"; return kExitUsage; }
    fs::path root = dir;
    std::error_code ec;
    if (name.empty()) name = sanitizeName(fs::absolute(root, ec).lexically_normal().filename().string());

    std::string manifest;
    if (structured) {
        manifest = "{\n  \"schemaVersion\": 1,\n  \"name\": \"" + name + "\",\n  \"version\": \"0.1.0\",\n  \"entry\": \"app.coy\",\n"
                   "  \"workflows\": {\n    \"build\": [[\"coyote\", \"build\", \"app.coy\", \"-o\", \"bin/" + name + ".ml\"]],\n"
                   "    \"start\": [\"@build\", [\"pvm\", \"bin/" + name + ".ml\"]]\n  },\n  \"dependencies\": {}\n}\n";
    } else if (library) {
        manifest = "{\n  \"schemaVersion\": 1,\n  \"name\": \"" + name + "\",\n  \"version\": \"0.1.0\",\n  \"type\": \"library\"\n}\n";
    } else {
        manifest = "{\n  \"schemaVersion\": 1,\n  \"name\": \"" + name + "\",\n  \"version\": \"0.1.0\"\n}\n";
    }
    try {
        loadManifest(manifest);
    } catch (const ManifestError& e) {
        std::cerr << "cpm: error: invalid project name '" << name << "': " << e.message << "\n";
        return kExitUsage;
    }

    // The class name of a library template: the project name with every non-alphanumeric character removed.
    std::string cls;
    for (char c : name) if (std::isalnum(static_cast<unsigned char>(c))) cls += c;
    if (cls.empty() || std::isdigit(static_cast<unsigned char>(cls[0]))) cls = "Lib" + cls;
    cls[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(cls[0])));
    std::vector<std::pair<std::string, std::string>> files = {{"packages.json", manifest}};
    if (library)
        files.push_back({"src/" + cls + ".coy",
                         "# Library '" + name + "': build it with `coyote lib pack .`, then `use \"" + name + "\"` (or `use \"./path/" + name + ".lib\"`).\n"
                         "use \"Standard\"\n\n"
                         "export class " + cls + " {\n"
                         "    public static method Greet(string who) {\n"
                         "        Shell.PrintLine(\"Hello, \" + who + \"!\")\n"
                         "    }\n"
                         "}\n"});
    else
        files.push_back({"app.coy", "use \"Standard\"\n\nShell.PrintLine(\"Hello from " + name + "\")\n"});
    if (structured) files.push_back({"appsettings.json", "{\n  \"appName\": \"" + name + "\"\n}\n"});
    for (const auto& f : files)
        if (fs::exists(root / f.first, ec)) {
            std::cerr << "cpm: error: '" << (root / f.first).string() << "' already exists; init never overwrites files\n";
            return kExitFailure;
        }
    fs::create_directories(root, ec);
    if (structured)
        for (const char* d : {"src", "modules", "bin"}) {
            fs::create_directories(root / d, ec);
            if (!fs::exists(root / d / ".gitkeep", ec)) writeFile(root / d / ".gitkeep", "");
        }
    for (const auto& f : files) {
        fs::create_directories((root / f.first).parent_path(), ec);
        if (!writeFile(root / f.first, f.second)) {
            std::cerr << "cpm: error: cannot write '" << (root / f.first).string() << "'\n";
            return kExitFailure;
        }
        std::cout << "created " << f.first << "\n";
    }
    return 0;
}

int cmdLayout(Project& p, const std::vector<std::string>& args) {
    bool structured = false, apply = false;
    for (const std::string& a : args) {
        if (a == "--structured") structured = true;
        else if (a == "--apply") apply = true;
        else { std::cerr << "cpm: error: unknown argument '" << a << "'\n"; return kExitUsage; }
    }
    if (!structured) {
        std::cerr << "cpm: error: 'layout' needs --structured (the only layout there is)\n";
        return kExitUsage;
    }
    std::vector<fs::path> moves;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(p.dir, ec))
        if (e.is_regular_file() && e.path().extension() == ".coy" && e.path().filename().generic_string() != p.manifest.entry)
            moves.push_back(e.path().filename());
    std::sort(moves.begin(), moves.end());
    for (const fs::path& m : moves)
        if (fs::exists(p.dir / "src" / m, ec)) {
            std::cerr << "cpm: error: src/" << m.generic_string() << " already exists; nothing was moved\n";
            return kExitFailure;
        }
    for (const fs::path& m : moves) std::cout << (apply ? "move " : "would move ") << m.generic_string() << " -> src/" << m.generic_string() << "\n";
    if (moves.empty()) std::cout << "no root .coy files other than the entry '" << p.manifest.entry << "' to move\n";
    if (!apply) {
        std::cout << "dry run: nothing was changed (pass --apply to move the files)\n";
        return 0;
    }
    for (const char* d : {"src", "modules", "bin"}) fs::create_directories(p.dir / d, ec);
    for (const fs::path& m : moves) {
        fs::rename(p.dir / m, p.dir / "src" / m, ec);
        if (ec) {
            std::cerr << "cpm: error: cannot move " << m.generic_string() << ": " << ec.message() << "\n";
            return kExitFailure;
        }
    }
    std::cout << "moved " << moves.size() << " file(s); imports are not rewritten (there are no imports yet)\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string dir = ".";
    std::vector<std::string> rest;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--help" || args[i] == "-h") { usage(std::cout); return 0; }
        if (args[i] == "--version") { std::cout << "cpm " COYOTE_VERSION "\n"; return 0; }
        if (args[i] == "-C") {
            if (i + 1 >= args.size()) { std::cerr << "cpm: error: '-C' needs a directory\n"; return kExitUsage; }
            dir = args[++i];
            continue;
        }
        rest.push_back(args[i]);
    }
    if (rest.empty()) { usage(std::cerr); return kExitUsage; }
    const std::string cmd = rest[0];
    std::vector<std::string> opts(rest.begin() + 1, rest.end());

    setChildEnvironment();

    if (cmd == "init") return cmdInit(dir, opts);

    bool locked = false;
    if (cmd == "install") {
        for (const std::string& o : opts) {
            if (o == "--locked") locked = true;
            else { std::cerr << "cpm: error: unknown option '" << o << "'\n"; return kExitUsage; }
        }
    } else if ((cmd == "update" || cmd == "lock" || cmd == "list" || cmd == "verify") && !opts.empty()) {
        std::cerr << "cpm: error: '" << cmd << "' takes no arguments\n";
        return kExitUsage;
    } else if (cmd != "update" && cmd != "lock" && cmd != "list" && cmd != "verify" && cmd != "layout") {
        std::cerr << "cpm: error: unknown command '" << cmd << "'\n";
        usage(std::cerr);
        return kExitUsage;
    }

    Project p;
    if (int rc = loadProject(dir, p)) return rc;

    if (cmd == "list") return cmdList(p);
    if (cmd == "verify") return cmdVerify(p);
    if (cmd == "layout") return cmdLayout(p, opts);

    ProviderSet providers = makeProviders(executableDir(argv[0]));
    return runResolve(p, cmd == "install" ? Mode::Install : cmd == "update" ? Mode::Update : Mode::LockOnly, locked, providers);
}
