#include "resolver.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <random>
#include <set>
#include <sstream>

#include "json.hpp"
#include "process.hpp"
#include "sha256.hpp"

namespace coyote::pkg {

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxFiles = 20000;
constexpr uintmax_t kMaxTreeBytes = 256ull * 1024 * 1024;

bool skipped(const fs::path& name) { return name == ".git" || name == ".coyote"; }

// Regular files below `dir` (relative generic paths, sorted). Rejects symlinks and oversized trees.
std::vector<std::string> listFiles(const fs::path& dir) {
    std::vector<std::string> files;
    uintmax_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::none, ec), end; it != end; it.increment(ec)) {
        if (ec) throw ResolveError{"cannot read directory '" + dir.string() + "': " + ec.message()};
        const fs::path rel = it->path().lexically_relative(dir);
        if (skipped(*rel.begin())) {
            if (it->is_directory()) it.disable_recursion_pending();
            continue;
        }
        if (it->is_symlink()) throw ResolveError{"package file '" + rel.generic_string() + "' is a symbolic link, which packages must not contain"};
        if (it->is_regular_file()) {
            if (files.size() >= kMaxFiles) throw ResolveError{"package '" + dir.string() + "' has more than 20000 files"};
            total += it->file_size();
            if (total > kMaxTreeBytes) throw ResolveError{"package '" + dir.string() + "' is larger than 256 MiB"};
            files.push_back(rel.generic_string());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

fs::path makeTempDir(const std::string& prefix) {
    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec);
    if (ec) throw ProviderError{"no temporary directory available"};
    std::random_device rd;
    for (int attempt = 0; attempt < 20; ++attempt) {
        fs::path p = base / (prefix + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(rd()));
        if (fs::create_directory(p, ec)) return p;
    }
    throw ProviderError{"cannot create a temporary directory"};
}

std::string relNormal(const std::string& base, const std::string& rel) {
    std::string s = (fs::path(base) / rel).lexically_normal().generic_string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s.empty() ? "." : s;
}

// ---- providers ----

class PathProvider : public Provider {
public:
    const char* name() const override { return "path"; }
    Fetched fetch(const Dependency& dep, const fs::path& baseDir, const std::string& baseRel, const std::string&) override {
        Fetched f;
        f.dir = (baseDir / dep.path).lexically_normal();
        std::error_code ec;
        if (!fs::is_directory(f.dir, ec))
            throw ProviderError{"path dependency '" + dep.name + "': directory '" + dep.path + "' does not exist"};
        f.source.type = SourceInfo::Path;
        f.source.path = relNormal(baseRel, dep.path);
        return f;
    }
};

class GitProvider : public Provider {
public:
    explicit GitProvider(std::string toolDir) : toolDir_(std::move(toolDir)) {}
    const char* name() const override { return "git"; }

    Fetched fetch(const Dependency& dep, const fs::path&, const std::string&, const std::string& lockedCommit) override {
        Fetched f;
        f.tempRoot = makeTempDir("coyote-git-");
        f.dir = f.tempRoot / "pkg";
        std::error_code ec;
        fs::create_directory(f.dir, ec);
        const std::string dir = f.dir.string();
        const std::string rev = lockedCommit.empty() ? dep.rev : lockedCommit;

        std::string sink;
        git({"-C", dir, "init", "-q"}, sink, "initialize a git repository");
        sink.clear();
        git({"-C", dir, "fetch", "-q", "--depth", "1", "--", dep.git, rev}, sink,
            "fetch '" + rev + "' from " + dep.git + " (dependency '" + dep.name + "')");
        std::string head;
        git({"-C", dir, "rev-parse", "FETCH_HEAD"}, head, "read the fetched commit");
        head = trim(head);
        if (head.size() != 40 || head.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw ProviderError{"git returned an unexpected commit id for dependency '" + dep.name + "'"};
        if (!lockedCommit.empty() && head != lockedCommit)
            throw ProviderError{"git dependency '" + dep.name + "': fetched commit " + head + " is not the locked commit " + lockedCommit};
        sink.clear();
        git({"-C", dir, "-c", "advice.detachedHead=false", "checkout", "-q", "FETCH_HEAD"}, sink, "check out the fetched commit");
        fs::remove_all(f.dir / ".git", ec);  // only the files are the package

        f.source.type = SourceInfo::Git;
        f.source.url = dep.git;
        f.source.rev = dep.rev;
        f.source.commit = head;
        return f;
    }

private:
    std::string toolDir_;

    void git(std::vector<std::string> args, std::string& out, const std::string& what) {
        args.insert(args.begin(), "git");
        ProcessResult r = runProcessCapture(args, toolDir_, out);
        if (!r.started) throw ProviderError{"cannot " + what + ": " + r.error + " (is git installed?)"};
        if (r.exitCode != 0) throw ProviderError{"git failed to " + what + " (exit code " + std::to_string(r.exitCode) + ")"};
    }
};

class UnsupportedProvider : public Provider {
public:
    UnsupportedProvider(const char* n, std::string why) : name_(n), why_(std::move(why)) {}
    const char* name() const override { return name_; }
    Fetched fetch(const Dependency& dep, const fs::path&, const std::string&, const std::string&) override {
        throw ProviderError{"dependency '" + dep.name + "': " + why_};
    }
private:
    const char* name_;
    std::string why_;
};

std::string readManifestFile(const fs::path& dir, const std::string& pkg) {
    fs::path p = dir / "packages.json";
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) throw ResolveError{"package '" + pkg + "' has no packages.json"};
    if (fs::file_size(p, ec) > 1024 * 1024) throw ResolveError{"package '" + pkg + "' has a packages.json larger than 1 MiB"};
    return readAll(p);
}

std::string jsonStr(const std::string& s) {
    std::string r = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\t': r += "\\t"; break;
            case '\r': r += "\\r"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    r += buf;
                } else {
                    r += static_cast<char>(c);
                }
        }
    }
    return r + "\"";
}

} // namespace

std::string SourceInfo::identity() const {
    switch (type) {
        case Path: return "path:" + path;
        case Git: return "git:" + url + "@" + rev;
        case Url: return "url:" + url + "#" + sha256;
    }
    return "";
}

Fetched::~Fetched() {
    if (!tempRoot.empty()) {
        std::error_code ec;
        fs::remove_all(tempRoot, ec);
    }
}

std::unique_ptr<Provider> makePathProvider() { return std::make_unique<PathProvider>(); }
std::unique_ptr<Provider> makeGitProvider(const std::string& toolDir) { return std::make_unique<GitProvider>(toolDir); }
std::unique_ptr<Provider> makeUrlProvider() {
    return std::make_unique<UnsupportedProvider>("url", "url dependencies are not implemented yet (cpm has no HTTPS download support); use 'path' or 'git'");
}
std::unique_ptr<Provider> makeRegistryProvider() {
    return std::make_unique<UnsupportedProvider>("registry", "registry dependencies are not implemented yet (there is no package registry); use 'path' or 'git'");
}

ProviderSet makeProviders(const std::string& toolDir) {
    ProviderSet s;
    s.path = makePathProvider();
    s.git = makeGitProvider(toolDir);
    s.url = makeUrlProvider();
    s.registry = makeRegistryProvider();
    return s;
}

std::string hashTree(const fs::path& dir) {
    Sha256 h;
    for (const std::string& rel : listFiles(dir)) {
        std::string data = readAll(dir / rel);
        h.update(rel);
        h.update(std::string(1, '\0'));
        h.update(std::to_string(data.size()));
        h.update(std::string(1, '\0'));
        h.update(data);
    }
    return "sha256-" + h.hexDigest();
}

void copyTree(const fs::path& src, const fs::path& dest) {
    std::error_code ec;
    std::vector<std::string> files = listFiles(src);  // also validates (no symlinks, size limits)
    fs::path tmp = dest.parent_path() / ("." + dest.filename().string() + ".tmp");
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    if (ec) throw ResolveError{"cannot create '" + tmp.string() + "': " + ec.message()};
    for (const std::string& rel : files) {
        fs::path to = tmp / rel;
        fs::create_directories(to.parent_path(), ec);
        fs::copy_file(src / rel, to, fs::copy_options::overwrite_existing, ec);
        if (ec) throw ResolveError{"cannot copy '" + rel + "': " + ec.message()};
    }
    fs::remove_all(dest, ec);
    fs::rename(tmp, dest, ec);
    if (ec) throw ResolveError{"cannot install '" + dest.string() + "': " + ec.message()};
}

std::map<std::string, std::string> requestedOf(const Manifest& m) {
    std::map<std::string, std::string> r;
    for (const Dependency& d : m.dependencies) r[d.name] = d.canonical();
    return r;
}

namespace {

struct Item {
    Dependency dep;
    fs::path baseDir;
    std::string baseRel;     // project-relative directory of the requester (path packages), "" for the root
    bool baseIsPath = true;  // false: requester came from git/url, so path dependencies are not allowed
    std::string requester;
};

std::string specIdentity(const Item& it) {
    if (it.dep.kind == Dependency::Path) return "path:" + relNormal(it.baseRel, it.dep.path) + "|" + it.dep.version;
    return it.dep.canonical();
}

} // namespace

Lock resolve(const Manifest& root, const fs::path& projectDir, ProviderSet& providers,
             std::map<std::string, std::unique_ptr<Fetched>>& fetched) {
    Lock lock;
    lock.requested = requestedOf(root);

    std::deque<Item> queue;
    for (const Dependency& d : root.dependencies) queue.push_back({d, projectDir, "", true, "packages.json"});

    std::map<std::string, std::pair<std::string, std::string>> seen;  // name -> (spec identity, requester)
    std::map<std::string, LockedPackage> done;

    while (!queue.empty()) {
        Item it = std::move(queue.front());
        queue.pop_front();
        const std::string ident = specIdentity(it);
        auto s = seen.find(it.dep.name);
        if (s != seen.end()) {
            if (s->second.first != ident)
                throw ResolveError{"conflicting requirements for package '" + it.dep.name + "': " + s->second.second + " needs [" +
                                   s->second.first + "] but " + it.requester + " needs [" + ident + "]"};
            continue;
        }
        seen[it.dep.name] = {ident, it.requester};

        if (it.dep.kind == Dependency::Path && !it.baseIsPath)
            throw ResolveError{it.requester + " uses a path dependency ('" + it.dep.name + "'), which only packages that are themselves local paths may do"};

        auto f = std::make_unique<Fetched>(providers.forKind(it.dep.kind).fetch(it.dep, it.baseDir, it.baseRel, ""));
        Manifest pm;
        try {
            pm = loadManifest(readManifestFile(f->dir, it.dep.name));
        } catch (const ManifestError& e) {
            throw ResolveError{"package '" + it.dep.name + "' has an invalid packages.json: " + e.message +
                               (e.line ? " (line " + std::to_string(e.line) + ")" : "")};
        }
        if (pm.name != it.dep.name)
            throw ResolveError{"dependency '" + it.dep.name + "' (" + it.requester + ") points at a package named '" + pm.name + "'; the names must match"};
        if (!it.dep.version.empty() && it.dep.kind != Dependency::Registry && pm.version != it.dep.version)
            throw ResolveError{"dependency '" + it.dep.name + "' requires version " + it.dep.version + " but the package declares " +
                               (pm.version.empty() ? "no version" : pm.version)};

        LockedPackage lp;
        lp.name = pm.name;
        lp.version = pm.version;
        lp.source = f->source;
        lp.integrity = hashTree(f->dir);
        for (const Dependency& d : pm.dependencies) {
            lp.dependencies.push_back(d.name);
            queue.push_back({d, f->dir, f->source.type == SourceInfo::Path ? f->source.path : "", f->source.type == SourceInfo::Path,
                             "package '" + pm.name + "'"});
        }
        std::sort(lp.dependencies.begin(), lp.dependencies.end());
        done[lp.name] = std::move(lp);
        fetched[it.dep.name] = std::move(f);
    }
    for (auto& kv : done) lock.packages.push_back(std::move(kv.second));
    return lock;
}

void fetchLocked(const Lock& lock, const fs::path& projectDir, ProviderSet& providers,
                 std::map<std::string, std::unique_ptr<Fetched>>& fetched) {
    for (const LockedPackage& lp : lock.packages) {
        Dependency d;
        d.name = lp.name;
        std::string pin;
        switch (lp.source.type) {
            case SourceInfo::Path: d.kind = Dependency::Path; d.path = lp.source.path; break;
            case SourceInfo::Git: d.kind = Dependency::Git; d.git = lp.source.url; d.rev = lp.source.rev; pin = lp.source.commit; break;
            case SourceInfo::Url: d.kind = Dependency::Url; d.url = lp.source.url; d.sha256 = lp.source.sha256; break;
        }
        auto f = std::make_unique<Fetched>(providers.forKind(d.kind).fetch(d, projectDir, "", pin));
        std::string actual = hashTree(f->dir);
        if (actual != lp.integrity)
            throw ResolveError{"package '" + lp.name + "' does not match packages.lock.json (expected " + lp.integrity + ", found " + actual +
                               "); if the change is intended, run 'cpm update'"};
        fetched[lp.name] = std::move(f);
    }
}

std::string lockToJson(const Lock& lock) {
    std::ostringstream o;
    o << "{\n  \"lockVersion\": " << lock.lockVersion << ",\n  \"requested\": {";
    bool first = true;
    for (const auto& kv : lock.requested) {
        o << (first ? "\n    " : ",\n    ") << jsonStr(kv.first) << ": " << jsonStr(kv.second);
        first = false;
    }
    o << (first ? "},\n" : "\n  },\n");
    o << "  \"packages\": [";
    for (size_t i = 0; i < lock.packages.size(); ++i) {
        const LockedPackage& p = lock.packages[i];
        o << (i ? ",\n" : "\n") << "    {\n      \"name\": " << jsonStr(p.name) << ",\n      \"version\": " << jsonStr(p.version)
          << ",\n      \"source\": {";
        switch (p.source.type) {
            case SourceInfo::Path: o << "\"type\": \"path\", \"path\": " << jsonStr(p.source.path); break;
            case SourceInfo::Git:
                o << "\"type\": \"git\", \"url\": " << jsonStr(p.source.url) << ", \"rev\": " << jsonStr(p.source.rev)
                  << ", \"commit\": " << jsonStr(p.source.commit);
                break;
            case SourceInfo::Url: o << "\"type\": \"url\", \"url\": " << jsonStr(p.source.url) << ", \"sha256\": " << jsonStr(p.source.sha256); break;
        }
        o << "},\n      \"integrity\": " << jsonStr(p.integrity) << ",\n      \"dependencies\": [";
        for (size_t k = 0; k < p.dependencies.size(); ++k) o << (k ? ", " : "") << jsonStr(p.dependencies[k]);
        o << "]\n    }";
    }
    o << (lock.packages.empty() ? "]\n}\n" : "\n  ]\n}\n");
    return o.str();
}

Lock lockFromJson(const std::string& text) {
    Json root;
    try {
        root = parseJson(text);
    } catch (const JsonError& e) {
        throw LockError{"invalid JSON at line " + std::to_string(e.line) + ": " + e.message};
    }
    auto need = [](const Json* v, Json::Kind k, const std::string& what) -> const Json& {
        if (!v || v->kind != k) throw LockError{what + " is missing or has the wrong type"};
        return *v;
    };
    if (root.kind != Json::Object) throw LockError{"the lock file must be a JSON object"};
    Lock lock;
    const Json& ver = need(root.find("lockVersion"), Json::Number, "'lockVersion'");
    if (ver.number != 1) throw LockError{"unsupported lockVersion (this cpm supports 1)"};
    for (const auto& kv : need(root.find("requested"), Json::Object, "'requested'").members) {
        if (kv.second.kind != Json::String) throw LockError{"'requested' values must be strings"};
        lock.requested[kv.first] = kv.second.string;
    }
    std::set<std::string> names;
    for (const Json& pj : need(root.find("packages"), Json::Array, "'packages'").items) {
        if (pj.kind != Json::Object) throw LockError{"each package must be an object"};
        LockedPackage p;
        p.name = need(pj.find("name"), Json::String, "package 'name'").string;
        p.version = need(pj.find("version"), Json::String, "package 'version'").string;
        p.integrity = need(pj.find("integrity"), Json::String, "package 'integrity'").string;
        if (!names.insert(p.name).second) throw LockError{"package '" + p.name + "' appears twice"};
        const Json& src = need(pj.find("source"), Json::Object, "package 'source'");
        const std::string type = need(src.find("type"), Json::String, "source 'type'").string;
        if (type == "path") {
            p.source.type = SourceInfo::Path;
            p.source.path = need(src.find("path"), Json::String, "source 'path'").string;
        } else if (type == "git") {
            p.source.type = SourceInfo::Git;
            p.source.url = need(src.find("url"), Json::String, "source 'url'").string;
            p.source.rev = need(src.find("rev"), Json::String, "source 'rev'").string;
            p.source.commit = need(src.find("commit"), Json::String, "source 'commit'").string;
            if (p.source.commit.size() != 40 || p.source.commit.find_first_not_of("0123456789abcdef") != std::string::npos)
                throw LockError{"package '" + p.name + "' has an invalid commit"};
        } else if (type == "url") {
            p.source.type = SourceInfo::Url;
            p.source.url = need(src.find("url"), Json::String, "source 'url'").string;
            p.source.sha256 = need(src.find("sha256"), Json::String, "source 'sha256'").string;
        } else {
            throw LockError{"package '" + p.name + "' has unknown source type '" + type + "'"};
        }
        for (const Json& d : need(pj.find("dependencies"), Json::Array, "package 'dependencies'").items) {
            if (d.kind != Json::String) throw LockError{"dependency names must be strings"};
            p.dependencies.push_back(d.string);
        }
        lock.packages.push_back(std::move(p));
    }
    if (!std::is_sorted(lock.packages.begin(), lock.packages.end(),
                        [](const LockedPackage& a, const LockedPackage& b) { return a.name < b.name; }))
        throw LockError{"packages must be sorted by name"};
    return lock;
}

void setChildEnvironment() {
    // Never prompt for credentials, and never let a dependency URL select a command-running git
    // transport (for example "ext::sh -c ...").
#ifdef _WIN32
    _putenv_s("GIT_TERMINAL_PROMPT", "0");
    _putenv_s("GIT_ALLOW_PROTOCOL", "file:https:ssh:git");
#else
    setenv("GIT_TERMINAL_PROMPT", "0", 1);
    setenv("GIT_ALLOW_PROTOCOL", "file:https:ssh:git", 1);
#endif
}

} // namespace coyote::pkg
