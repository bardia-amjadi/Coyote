#pragma once

#include <map>
#include <string>
#include <vector>

namespace coyote::pkg {

// One step of a workflow.
struct Step {
    enum Kind {
        Run,    // run a program with arguments (no shell)
        Ref,    // run another workflow ("@name")
        Mkdir   // create a directory (only synthesized for the default workflows)
    };
    Kind kind = Run;
    std::vector<std::string> argv;  // Run; Mkdir: argv[0] is the directory
    std::string ref;                // Ref
    int line = 0, col = 0;          // position in packages.json (0 for synthesized steps)
};

struct Workflow {
    std::string name;
    std::vector<Step> steps;
    bool synthesized = false;  // not written in packages.json
};

// A dependency as written in packages.json.
struct Dependency {
    enum Kind { Path, Git, Url, Registry };
    Kind kind = Path;
    std::string name;
    std::string path;     // Path: relative directory
    std::string git;      // Git: repository URL or path
    std::string rev;      // Git: branch, tag or commit to use
    std::string url;      // Url: archive URL
    std::string sha256;   // Url: expected hash of the archive
    std::string version;  // Registry: the version; others: optional exact version the package must declare
    int line = 0, col = 0;

    // Canonical one-line form, used to detect whether a lock file still matches the manifest.
    std::string canonical() const;
};

// The validated contents of packages.json (schema version 1).
struct Manifest {
    int schemaVersion = 0;
    std::string name;
    std::string version;
    std::string entry = "app.coy";
    std::vector<std::string> requires_;         // library projects only: other libraries to load together
    std::string type = "app";                   // "app" (runnable) or "library" (packed into a .lib)
    std::map<std::string, Workflow> workflows;  // declared plus synthesized defaults
    std::vector<Dependency> dependencies;       // sorted by name

    // Optional `output` and `run` sections (any project file; written by Coyote Studio's Project Manager).
    std::string description;
    bool hasOutput = false;                     // an "output" section is present: it drives the default build/start workflows
    std::string outDir = "bin";                 // output.directory: where Build writes its result
    std::string outName;                        // output.name: file name without extension (default: the project name)
    std::string outKind = "native";             // output.kind: "native" (executable) or "ml" (a .ml file for the PVM)
    std::string outMode = "auto";               // output.mode: "auto", "bundle" or "native-only" (coyote-llvm)
    bool keepUnused = false;                    // output.keepUnused: keep methods that are never called
    std::vector<std::string> runArgs;           // run.args: arguments the program gets when started
};

struct ManifestError {
    int line, col;  // 0 when the problem is not tied to a position
    std::string message;
};

// The project file of `dir`: packages.json, else the first (by name) *.cyproj. Empty when there is none.
std::string findManifestFile(const std::string& dir);

// Parses and validates packages.json text. Throws ManifestError on the first problem.
Manifest loadManifest(const std::string& text);

} // namespace coyote::pkg
