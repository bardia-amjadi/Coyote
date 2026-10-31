#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "manifest.hpp"

namespace coyote::pkg {

// Where a resolved package came from, exactly as recorded in the lock file.
struct SourceInfo {
    enum Type { Path, Git, Url } type = Path;
    std::string path;    // Path: project-relative directory ('/' separators)
    std::string url;     // Git/Url
    std::string rev;     // Git: the requested branch/tag/commit
    std::string commit;  // Git: the exact commit that was resolved
    std::string sha256;  // Url: archive hash

    // Identity used to detect two different packages claiming the same name.
    std::string identity() const;
};

struct LockedPackage {
    std::string name;
    std::string version;
    SourceInfo source;
    std::string integrity;  // "sha256-<hex>" over the package's files
    std::vector<std::string> dependencies;  // names, sorted
};

struct Lock {
    int lockVersion = 1;
    std::map<std::string, std::string> requested;  // root dependency name -> canonical spec
    std::vector<LockedPackage> packages;           // sorted by name
};

struct LockError {
    std::string message;
};

struct ProviderError {
    std::string message;
};

struct ResolveError {
    std::string message;
};

// A fetched package: a directory holding its files. Temporary directories are removed on destruction.
struct Fetched {
    std::filesystem::path dir;
    SourceInfo source;
    std::filesystem::path tempRoot;  // non-empty: removed in the destructor
    Fetched() = default;
    Fetched(const Fetched&) = delete;
    Fetched& operator=(const Fetched&) = delete;
    Fetched(Fetched&& o) noexcept : dir(std::move(o.dir)), source(std::move(o.source)), tempRoot(std::move(o.tempRoot)) { o.tempRoot.clear(); }
    ~Fetched();
};

// A way to obtain a package. Each dependency kind has its own provider; unimplemented providers
// exist as explicit interfaces that fail with a clear message.
class Provider {
public:
    virtual ~Provider() = default;
    virtual const char* name() const = 0;
    // `lockedCommit` (git) pins the exact revision when installing from a lock file.
    virtual Fetched fetch(const Dependency& dep, const std::filesystem::path& baseDir, const std::string& baseRel,
                          const std::string& lockedCommit) = 0;
};

std::unique_ptr<Provider> makePathProvider();
std::unique_ptr<Provider> makeGitProvider(const std::string& toolDir);
std::unique_ptr<Provider> makeUrlProvider();       // interface only: not implemented
std::unique_ptr<Provider> makeRegistryProvider();  // interface only: not implemented

struct ProviderSet {
    std::unique_ptr<Provider> path, git, url, registry;
    Provider& forKind(Dependency::Kind k) {
        switch (k) {
            case Dependency::Path: return *path;
            case Dependency::Git: return *git;
            case Dependency::Url: return *url;
            case Dependency::Registry: return *registry;
        }
        return *path;
    }
};

ProviderSet makeProviders(const std::string& toolDir);

// SHA-256 over every file of a directory (sorted relative paths, sizes and contents). `.git` and
// `.coyote` are skipped. Symlinks and oversized trees are rejected (throws ResolveError).
std::string hashTree(const std::filesystem::path& dir);

// Copies a package tree (same exclusions) to `dest`, replacing it.
void copyTree(const std::filesystem::path& src, const std::filesystem::path& dest);

// Resolves the root manifest's dependencies (and theirs, transitively) without a lock file.
// `fetched` receives each package's files, by name.
Lock resolve(const Manifest& root, const std::filesystem::path& projectDir, ProviderSet& providers,
             std::map<std::string, std::unique_ptr<Fetched>>& fetched);

// Fetches exactly what `lock` says and verifies each package's integrity.
void fetchLocked(const Lock& lock, const std::filesystem::path& projectDir, ProviderSet& providers,
                 std::map<std::string, std::unique_ptr<Fetched>>& fetched);

// Lock file (de)serialization. The output is deterministic: no timestamps, sorted keys.
std::string lockToJson(const Lock& lock);
Lock lockFromJson(const std::string& text);  // throws LockError

// The lock's `requested` map for a manifest.
std::map<std::string, std::string> requestedOf(const Manifest& m);

// Platform helper: sets environment variables for the child processes cpm starts (git).
void setChildEnvironment();

} // namespace coyote::pkg
