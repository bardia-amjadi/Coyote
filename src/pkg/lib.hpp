// The .lib container: one file holding a library's Coyote source files.
//
//   coyote-lib 1
//   name Standard
//   version 1.0.0
//   file shell.coy 123
//   <123 bytes of source>
//   file math.coy 45
//   <45 bytes of source>
//
// A .lib is a *source* bundle: `use "Standard"` compiles its files together with the program (docs/libraries.md).
#pragma once

#include <string>
#include <vector>

namespace coyote::pkg {

struct LibFile {
    std::string path;  // relative, forward slashes, e.g. "shell.coy"
    std::string text;
};

struct Lib {
    std::string name;
    std::string version;
    std::vector<LibFile> files;
};

struct LibError {
    std::string message;
};

constexpr size_t kMaxLibFiles = 1000;
constexpr size_t kMaxLibFileBytes = 4 * 1024 * 1024;

Lib parseLib(const std::string& text);        // throws LibError
std::string writeLib(const Lib& lib);         // throws LibError when the library is invalid
bool isValidLibName(const std::string& name);

} // namespace coyote::pkg
