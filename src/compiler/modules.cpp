#include "modules.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "lexer.hpp"
#include "lib.hpp"
#include "manifest.hpp"
#include "process.hpp"
#include "parser.hpp"

namespace coyote {

namespace {

namespace fs = std::filesystem;

constexpr std::streamoff kMaxSourceBytes = 4 * 1024 * 1024;
constexpr size_t kMaxModules = 1000;

bool readSource(const std::string& path, std::string& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read '" + path + "'";
        return false;
    }
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size > kMaxSourceBytes) {
        error = "'" + path + "' is larger than the " + std::to_string(kMaxSourceBytes / (1024 * 1024)) + " MiB source file limit";
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

std::vector<std::string>& extraLibDirs() {
    static std::vector<std::string> d;
    return d;
}

bool isPathSpec(const std::string& s) {
    return s.find('/') != std::string::npos || s.find('\\') != std::string::npos ||
           (s.size() > 4 && s.compare(s.size() - 4, 4, ".coy") == 0);
}

class Loader {
public:
    Loader(Program& out, std::vector<Diagnostic>& diags) : out_(out), diags_(diags) {}

    bool run(const std::string& entry, std::string& readError) {
        std::string text;
        if (!readSource(entry, text, readError)) return false;
        add(entry, text);
        // Modules are appended while this loop runs; indexes stay stable.
        for (size_t i = 0; i < out_.modules.size(); ++i) {
            if (diags_.size() > before_) break;
            resolveImports(i);
        }
        return diags_.size() == before_;
    }

private:
    Program& out_;
    std::vector<Diagnostic>& diags_;
    size_t before_ = diags_.size();
    std::map<std::string, int> byCanonical_;
    std::map<std::string, std::vector<int>> libs_;  // canonical .lib path -> its modules
    std::map<int, std::string> baseDirs_;            // module -> directory its relative imports resolve against (libs: the .lib's folder)

    static std::string canonical(const std::string& path) {
        std::error_code ec;
        fs::path p = fs::weakly_canonical(path, ec);
        return ec ? path : p.string();
    }

    void error(const std::string& file, SourceLoc loc, std::string msg) {
        diags_.push_back(Diagnostic{file, loc, std::move(msg)});
    }

    // Parses one file and merges its declarations. Returns its module index, or -1 on a syntax error.
    int add(const std::string& path, const std::string& text, bool ignoreStatements = false) {
        Program p;
        try {
            std::vector<Diagnostic> found;
            p = parseRecovering(lex(text, path), path, found);
            if (!found.empty()) {
                for (Diagnostic& d : found) diags_.push_back(std::move(d));
                return -1;
            }
        } catch (const CompileError& e) {
            diags_.push_back(e.diag);
            return -1;
        }
        int idx = static_cast<int>(out_.modules.size());
        byCanonical_[canonical(path)] = idx;
        Module m;
        m.path = path;
        m.ns = p.ns;
        m.imports = std::move(p.imports);
        out_.modules.push_back(std::move(m));
        for (HandDecl& h : p.hands) {
            h.unit = idx;
            out_.hands.push_back(std::move(h));
        }
        for (MethodDecl& md : p.methods) {
            md.unit = idx;
            out_.methods.push_back(std::move(md));
        }
        for (ClassDecl& cd : p.classes) {
            int ci = static_cast<int>(out_.classes.size());
            cd.unit = idx;
            for (MethodDecl& md : cd.methods) {
                md.unit = idx;
                md.classIndex = ci;
                out_.methods.push_back(std::move(md));
            }
            cd.methods.clear();
            out_.classes.push_back(std::move(cd));
        }
        if (idx == 0) {
            out_.stmts = std::move(p.stmts);
        } else if (!p.stmts.empty() && !ignoreStatements) {
            error(path, p.stmts.front()->loc,
                  "only the entry file may contain top-level statements; this file may only contain 'use', 'hand' declarations, methods and classes");
        }
        return idx;
    }

    // Finds <dir or ancestor>/.coyote/packages/<name>/ and returns the package's entry file.
    bool findPackage(const std::string& fromFile, const std::string& name, std::string& entryPath, std::string& why, bool* notInstalled = nullptr) {
        if (notInstalled) *notInstalled = false;
        if (name.find("..") != std::string::npos) {
            why = "invalid package name '" + name + "'";
            return false;
        }
        std::error_code ec;
        fs::path dir = fs::absolute(fs::path(fromFile), ec).parent_path();
        for (;;) {
            fs::path pkg = dir / ".coyote" / "packages" / name;
            if (fs::is_directory(pkg, ec)) {
                std::string text, err;
                const std::string manifestFile = pkg::findManifestFile(pkg.string());
                if (manifestFile.empty() || !readSource(manifestFile, text, err)) {
                    why = "package '" + name + "' has no readable packages.json (" + err + ")";
                    return false;
                }
                try {
                    pkg::Manifest m = pkg::loadManifest(text);
                    entryPath = (pkg / m.entry).string();
                    return true;
                } catch (const pkg::ManifestError& e) {
                    why = "package '" + name + "' has an invalid packages.json: " + e.message;
                    return false;
                }
            }
            if (dir == dir.parent_path()) break;
            dir = dir.parent_path();
        }
        if (notInstalled) *notInstalled = true;
        why = "package '" + name + "' is not installed (looked for .coyote/packages/" + name + " next to " + fromFile +
              " and in the directories above it; run 'cpm install')";
        return false;
    }

    // ---- libraries (`use`) ----

    static std::vector<std::string> splitPathList(const std::string& v) {
        std::vector<std::string> out;
#ifdef _WIN32
        const char sep = ';';
#else
        const char sep = ':';
#endif
        size_t start = 0;
        while (start <= v.size()) {
            size_t end = v.find(sep, start);
            if (end == std::string::npos) end = v.size();
            if (end > start) out.push_back(v.substr(start, end - start));
            start = end + 1;
        }
        return out;
    }

    // Where `use "Name"` looks, in order: $COYOTE_LIBS, <project>/.coyote/libs (this file's directory and
    // every ancestor), the toolchain's own libs folder, then ~/coyote/libs.
    std::vector<std::string> libSearchDirs(const std::string& fromFile) {
        std::vector<std::string> dirs = extraLibDirs();
        if (const char* env = std::getenv("COYOTE_LIBS")) for (const std::string& d : splitPathList(env)) dirs.push_back(d);
        std::error_code ec;
        fs::path dir = fs::absolute(fs::path(baseDirOf(fromFile)), ec);
        for (;;) {
            dirs.push_back((dir / ".coyote" / "libs").string());
            if (dir == dir.parent_path()) break;
            dir = dir.parent_path();
        }
        std::string exeDir = pkg::executableDir("");
        if (!exeDir.empty()) {
            dirs.push_back((fs::path(exeDir) / "libs").string());
            dirs.push_back((fs::path(exeDir).parent_path() / "libs").string());
        }
        const char* home = std::getenv("USERPROFILE");
        if (!home || !*home) home = std::getenv("HOME");
        if (home && *home) dirs.push_back((fs::path(home) / "coyote" / "libs").string());
        return dirs;
    }

    static std::string lowered(std::string s) {
        for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        return s;
    }

    bool findLib(const std::string& fromFile, const ImportDecl& imp, std::string& libPath, std::string& why) {
        std::error_code ec;
        if (isPathSpec(imp.spec) || (imp.spec.size() > 4 && lowered(imp.spec.substr(imp.spec.size() - 4)) == ".lib")) {
            fs::path rel(imp.spec);
            if (lowered(rel.extension().string()) != ".lib") rel += ".lib";
            libPath = (fs::path(baseDirOf(fromFile)) / rel).lexically_normal().string();
            if (!fs::is_regular_file(libPath, ec)) {
                why = "cannot use '" + imp.spec + "': no such library file '" + libPath + "'";
                return false;
            }
            return true;
        }
        if (imp.spec.find("..") != std::string::npos) { why = "invalid library name '" + imp.spec + "'"; return false; }
        std::vector<std::string> tried;
        for (const std::string& d : libSearchDirs(fromFile)) {
            for (const std::string& n : {imp.spec, lowered(imp.spec)}) {
                fs::path cand = fs::path(d) / (n + ".lib");
                if (fs::is_regular_file(cand, ec)) { libPath = cand.string(); return true; }
            }
            tried.push_back(d);
        }
        why = "library '" + imp.spec + "' not found (looked for " + lowered(imp.spec) + ".lib in:";
        for (const std::string& d : tried) why += " " + d + ";";
        why += " install it there, set COYOTE_LIBS, or write use \"./path/to/" + lowered(imp.spec) + ".lib\")";
        return false;
    }

    // Loads every file of a library as a module. Returns false (after reporting) on failure.
    bool useLib(const std::string& file, const ImportDecl& imp, std::vector<int>& idxs) {
        std::string libPath, why;
        if (!findLib(file, imp, libPath, why)) { error(file, imp.loc, why); return false; }
        const std::string key = canonical(libPath);
        auto known = libs_.find(key);
        if (known != libs_.end()) { idxs = known->second; return true; }
        std::string text, err;
        if (!readSource(libPath, text, err)) { error(file, imp.loc, "cannot use '" + imp.spec + "': " + err); return false; }
        pkg::Lib lib;
        try {
            lib = pkg::parseLib(text);
        } catch (const pkg::LibError& e) {
            error(file, imp.loc, "cannot use '" + imp.spec + "' (" + libPath + "): " + e.message);
            return false;
        }
        if (out_.modules.size() + lib.files.size() > kMaxModules) {
            error(file, imp.loc, "too many files (limit " + std::to_string(kMaxModules) + ")");
            return false;
        }
        libs_[key] = {};  // guards against a library that (indirectly) uses itself
        std::vector<int> mine;
        const std::string libDir = fs::path(libPath).parent_path().string();
        for (const pkg::LibFile& f : lib.files) {
            int idx = add(libPath + "/" + f.path, f.text);
            if (idx < 0) return false;
            out_.modules[static_cast<size_t>(idx)].libName = lib.name;
            baseDirs_[idx] = libDir;
            mine.push_back(idx);
        }
        for (int i : mine)
            for (int j : mine)
                if (i != j) out_.modules[static_cast<size_t>(i)].libSiblings.push_back(j);
        libs_[key] = mine;  // provisional: a library that (indirectly) requires itself sees only its own files
        idxs = mine;
        for (int i : mine) out_.modules[static_cast<size_t>(i)].libRequires = lib.requires_;
        // `requires`: the required libraries load with this one and are visible to whoever uses it.
        for (const std::string& req : lib.requires_) {
            ImportDecl r;
            r.loc = imp.loc;
            r.spec = req;
            r.isUse = true;
            std::vector<int> more;
            if (!useLib(libPath, r, more)) return false;
            for (int m : more)
                if (std::find(idxs.begin(), idxs.end(), m) == idxs.end()) idxs.push_back(m);
        }
        libs_[key] = idxs;
        return true;
    }

    std::string baseDirOf(const std::string& file) {
        for (const auto& kv : baseDirs_)
            if (kv.first < static_cast<int>(out_.modules.size()) && out_.modules[static_cast<size_t>(kv.first)].path == file) return kv.second;
        return fs::path(file).parent_path().string();
    }

    // ---- namespaces ----

    struct NsIndex { std::map<std::string, std::vector<std::string>> files; };  // namespace -> its files (sorted)
    std::map<std::string, NsIndex> nsIndex_;                                    // project root -> index

    // The folder whose .coy files form "the project" of `file`: the nearest ancestor holding a project file
    // (packages.json / *.cyproj), otherwise the file's own folder.
    static std::string projectRoot(const std::string& file) {
        std::error_code ec;
        fs::path dir = fs::absolute(fs::path(file), ec).parent_path();
        for (fs::path d = dir;;) {
            if (!pkg::findManifestFile(d.string()).empty()) return d.string();
            if (d == d.parent_path()) break;
            d = d.parent_path();
        }
        return dir.string();
    }

    // The namespace named by the first declaration of a file ("" when it has none). Reads only the file's head.
    static std::string declaredNamespace(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return "";
        std::string head(8192, '\0');
        in.read(&head[0], static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<size_t>(in.gcount()));
        size_t i = head.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
        while (i < head.size()) {
            size_t e = head.find('\n', i);
            if (e == std::string::npos) e = head.size();
            size_t a = i;
            while (a < e && (head[a] == ' ' || head[a] == '\t' || head[a] == '\r')) ++a;
            i = e + 1;
            if (a >= e || head[a] == '#') continue;  // blank line or comment
            if (head.compare(a, 9, "namespace") != 0 || a + 9 >= e || (head[a + 9] != ' ' && head[a + 9] != '\t')) return "";
            a += 9;
            while (a < e && (head[a] == ' ' || head[a] == '\t')) ++a;
            std::string ns;
            while (a < e && (std::isalnum(static_cast<unsigned char>(head[a])) || head[a] == '_' || head[a] == '.')) ns += head[a++];
            return ns;
        }
        return "";
    }

    const NsIndex& indexFor(const std::string& root) {
        auto known = nsIndex_.find(root);
        if (known != nsIndex_.end()) return known->second;
        NsIndex idx;
        std::error_code ec;
        size_t seen = 0;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), endIt;
        for (; !ec && it != endIt; it.increment(ec)) {
            const fs::path& p = it->path();
            const std::string name = p.filename().string();
            std::error_code ec2;
            if (it->is_directory(ec2)) {
                if (name.empty() || name[0] == '.' || name == "bin" || name == "obj" || name == "node_modules" || name == "dist") it.disable_recursion_pending();
                continue;
            }
            if (p.extension() != ".coy") continue;
            if (++seen > 5000) break;
            const std::string ns = declaredNamespace(p.string());
            if (!ns.empty()) idx.files[ns].push_back(p.string());
        }
        for (auto& kv : idx.files) std::sort(kv.second.begin(), kv.second.end());
        return nsIndex_.emplace(root, std::move(idx)).first->second;
    }

    // Files of namespace `ns` in the project of `fromFile` (empty when there is none).
    std::vector<std::string> namespaceFiles(const std::string& fromFile, const std::string& ns) {
        const NsIndex& idx = indexFor(projectRoot(fromFile));
        auto it = idx.files.find(ns);
        return it == idx.files.end() ? std::vector<std::string>{} : it->second;
    }

    // Loads (once) every file of a namespace; pushes the module index of each (-1 after a reported error).
    // `sibling`: the files are implicit same-namespace neighbours, so another program's top-level statements are ignored, not an error.
    void loadNamespace(const std::string& fromFile, const ImportDecl& imp, const std::vector<std::string>& files, std::vector<int>& idxs, bool sibling = false) {
        for (const std::string& f : files) {
            auto known = byCanonical_.find(canonical(f));
            if (known != byCanonical_.end()) { idxs.push_back(known->second); continue; }
            if (out_.modules.size() >= kMaxModules) {
                error(fromFile, imp.loc, "too many files (limit " + std::to_string(kMaxModules) + ")");
                idxs.push_back(-1);
                return;
            }
            std::string text, err;
            if (!readSource(f, text, err)) { error(fromFile, imp.loc, "cannot use \"" + imp.spec + "\": " + err); idxs.push_back(-1); continue; }
            idxs.push_back(add(f, text, sibling));
        }
    }

    static bool endsWith(const std::string& s, const std::string& suffix) {
        return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    void resolveImports(size_t mi) {
        // Copy: `out_.modules` may grow (and reallocate) while imports are resolved.
        const std::string file = out_.modules[mi].path;
        const std::vector<ImportDecl> imports = out_.modules[mi].imports;
        const std::vector<int> siblings = out_.modules[mi].libSiblings;
        const std::string libName = out_.modules[mi].libName;
        const std::string myNs = out_.modules[mi].ns;
        const std::vector<std::string> libRequires = out_.modules[mi].libRequires;
        std::vector<int> resolved;
        std::vector<ImportDecl> shown;  // parallel to `resolved`; a `use` expands to one entry per file
        auto push = [&](const ImportDecl& imp, int idx) { shown.push_back(imp); resolved.push_back(idx); };
        for (const ImportDecl& imp : imports) {
            if (endsWith(imp.spec, ".coy")) {
                error(file, imp.loc, "'use' no longer takes a file ('" + imp.spec + "'): put 'namespace Name' at the top of that file and write use \"Name\"");
                push(imp, -1);
                continue;
            }
            if (endsWith(lowered(imp.spec), ".lib")) {  // an explicit library file
                std::vector<int> idxs;
                if (!useLib(file, imp, idxs)) { push(imp, -1); continue; }
                for (int i : idxs) push(imp, i);
                continue;
            }
            // `use "Name"` is a namespace of this project, an installed package or a library.
            const bool plain = imp.spec.find('/') == std::string::npos && imp.spec.find('\\') == std::string::npos && imp.spec.find("..") == std::string::npos;
            std::vector<std::string> nsFiles = plain ? namespaceFiles(file, imp.spec) : std::vector<std::string>{};
            std::string pkgEntry, pkgWhy, libPath, libWhy;
            bool pkgMissing = true;
            const bool isPkg = plain && findPackage(file, imp.spec, pkgEntry, pkgWhy, &pkgMissing);
            const bool isLib = plain && findLib(file, imp, libPath, libWhy);
            const int kinds = (nsFiles.empty() ? 0 : 1) + (isPkg ? 1 : 0) + (isLib ? 1 : 0);
            if (kinds == 0) {
                if (plain && !pkgMissing) { error(file, imp.loc, pkgWhy); }
                else error(file, imp.loc, "cannot use \"" + imp.spec + "\": it is not a namespace of this project, an installed package or a library. " + libWhy);
                push(imp, -1);
                continue;
            }
            if (kinds > 1) {
                error(file, imp.loc, "use \"" + imp.spec + "\" is ambiguous: it names " + std::string(nsFiles.empty() ? "" : "a namespace of this project") +
                                         std::string(!nsFiles.empty() && isPkg ? " and " : "") + std::string(isPkg ? "an installed package" : "") +
                                         std::string((!nsFiles.empty() || isPkg) && isLib ? " and " : "") + std::string(isLib ? "a library" : "") +
                                         "; rename one of them");
                push(imp, -1);
                continue;
            }
            std::vector<int> idxs;
            if (!nsFiles.empty()) {
                loadNamespace(file, imp, nsFiles, idxs);
            } else if (isPkg) {
                loadNamespace(file, imp, {fs::path(pkgEntry).lexically_normal().string()}, idxs);
            } else if (!useLib(file, imp, idxs)) {
                idxs.assign(1, -1);
            }
            for (int i : idxs) push(imp, i);
        }
        // A library file also sees the libraries its .lib requires.
        for (const std::string& req : libRequires) {
            ImportDecl r;
            r.loc = SourceLoc{};
            r.spec = req;
            r.isUse = true;
            std::vector<int> idxs;
            if (!useLib(file, r, idxs)) { push(r, -1); continue; }
            for (int i : idxs) push(r, i);
        }
        // The files of one library see each other's exports.
        for (int sib : siblings) {
            ImportDecl self;
            self.spec = libName;
            push(self, sib);
        }
        // The files of one namespace see each other's exports (no `use` needed for your own namespace).
        if (!myNs.empty() && libName.empty()) {
            ImportDecl self;
            self.spec = myNs;
            std::vector<int> idxs;
            loadNamespace(file, self, namespaceFiles(file, myNs), idxs, true);
            for (int i : idxs)
                if (i != static_cast<int>(mi)) push(self, i);
        }
        out_.modules[mi].imports = std::move(shown);
        out_.modules[mi].importIndex = std::move(resolved);
    }
};

} // namespace

void setExtraLibDirs(std::vector<std::string> dirs) { extraLibDirs() = std::move(dirs); }

bool loadProgram(const std::string& entryPath, Program& out, std::vector<Diagnostic>& diagnostics,
                 std::string& readError) {
    return Loader(out, diagnostics).run(entryPath, readError);
}

} // namespace coyote
