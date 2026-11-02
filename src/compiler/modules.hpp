#pragma once

#include <string>
#include <vector>

#include "ast.hpp"

namespace coyote {

// Reads `entryPath` and every file it imports (transitively) and merges them into one Program.
// Imports are `import "relative/file.coy"` (resolved against the importing file's directory) or
// `import "package"` (resolved to <ancestor>/.coyote/packages/<package>, using that package's
// `entry` from packages.json). Imported files may contain only imports, `hand` declarations and
// methods. Returns false and appends diagnostics when a file cannot be read, parsed or resolved.
// `readError` is set (and false returned) only when the entry file itself cannot be read.
// Additional folders `use "Name"` searches first (used by `coyote lib pack` so a library can require its siblings).
void setExtraLibDirs(std::vector<std::string> dirs);

bool loadProgram(const std::string& entryPath, Program& out, std::vector<Diagnostic>& diagnostics,
                 std::string& readError);

} // namespace coyote
