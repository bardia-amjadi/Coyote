#include "lib.hpp"

#include <set>

namespace coyote::pkg {

bool isValidLibName(const std::string& n) {
    if (n.empty() || n.size() > 64) return false;
    for (size_t i = 0; i < n.size(); ++i) {
        char c = n[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (i > 0 && ((c >= '0' && c <= '9') || c == '-' || c == '.'));
        if (!ok) return false;
    }
    return true;
}

namespace {

bool validPath(const std::string& p) {
    if (p.empty() || p.size() > 200 || p[0] == '/' || p.find('\\') != std::string::npos || p.find(':') != std::string::npos) return false;
    if (p.size() < 5 || p.compare(p.size() - 4, 4, ".coy") != 0) return false;
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find('/', start);
        if (end == std::string::npos) end = p.size();
        std::string part = p.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        for (char c : part) if (static_cast<unsigned char>(c) < 0x20) return false;
        start = end + 1;
    }
    return true;
}

bool readLine(const std::string& s, size_t& pos, std::string& line) {
    if (pos >= s.size()) return false;
    size_t nl = s.find('\n', pos);
    if (nl == std::string::npos) return false;
    line = s.substr(pos, nl - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    pos = nl + 1;
    return true;
}

} // namespace

Lib parseLib(const std::string& text) {
    Lib lib;
    size_t pos = 0;
    std::string line;
    if (!readLine(text, pos, line) || line != "coyote-lib 1") {
        if (line.rfind("coyote-lib ", 0) == 0) throw LibError{"unsupported library format '" + line + "' (this toolchain reads 'coyote-lib 1')"};
        throw LibError{"not a Coyote library (the first line must be 'coyote-lib 1')"};
    }
    bool haveName = false;
    std::set<std::string> seen;
    while (readLine(text, pos, line)) {
        if (line.rfind("name ", 0) == 0 && !haveName && lib.files.empty()) {
            lib.name = line.substr(5);
            if (!isValidLibName(lib.name)) throw LibError{"invalid library name '" + lib.name + "'"};
            haveName = true;
        } else if (line.rfind("version ", 0) == 0 && lib.files.empty()) {
            lib.version = line.substr(8);
            if (lib.version.size() > 64) throw LibError{"library version is too long"};
        } else if (line.rfind("requires ", 0) == 0 && lib.files.empty()) {
            std::string r = line.substr(9);
            if (!isValidLibName(r)) throw LibError{"invalid required library name '" + r + "'"};
            if (lib.requires_.size() >= 64) throw LibError{"too many 'requires' lines"};
            lib.requires_.push_back(r);
        } else if (line.rfind("file ", 0) == 0) {
            if (!haveName) throw LibError{"the library has no 'name' line before its first file"};
            size_t sp = line.find_last_of(' ');
            if (sp <= 5) throw LibError{"malformed file line '" + line + "'"};
            std::string path = line.substr(5, sp - 5);
            std::string num = line.substr(sp + 1);
            if (num.empty() || num.size() > 9 || num.find_first_not_of("0123456789") != std::string::npos)
                throw LibError{"malformed byte count in '" + line + "'"};
            size_t n = std::stoul(num);
            if (!validPath(path)) throw LibError{"invalid file path '" + path + "' in the library"};
            if (n > kMaxLibFileBytes) throw LibError{"file '" + path + "' in the library is larger than " + std::to_string(kMaxLibFileBytes / (1024 * 1024)) + " MiB"};
            if (lib.files.size() >= kMaxLibFiles) throw LibError{"the library has more than " + std::to_string(kMaxLibFiles) + " files"};
            if (!seen.insert(path).second) throw LibError{"duplicate file '" + path + "' in the library"};
            if (pos + n > text.size()) throw LibError{"the library is truncated (file '" + path + "' is cut off)"};
            lib.files.push_back(LibFile{path, text.substr(pos, n)});
            pos += n;
            if (pos < text.size() && text[pos] == '\n') ++pos;
        } else if (line.empty()) {
            continue;
        } else {
            throw LibError{"unexpected line '" + line.substr(0, 40) + "' in the library"};
        }
    }
    if (!haveName) throw LibError{"the library has no name"};
    if (lib.files.empty() && lib.requires_.empty()) throw LibError{"the library '" + lib.name + "' contains no .coy files"};
    return lib;
}

std::string writeLib(const Lib& lib) {
    if (!isValidLibName(lib.name)) throw LibError{"invalid library name '" + lib.name + "' (letters, digits, '_', '-', '.'; must start with a letter or '_')"};
    if (lib.files.empty() && lib.requires_.empty()) throw LibError{"the library has no .coy files"};
    std::string out = "coyote-lib 1\nname " + lib.name + "\n";
    if (!lib.version.empty()) out += "version " + lib.version + "\n";
    for (const std::string& r : lib.requires_) {
        if (!isValidLibName(r)) throw LibError{"invalid required library name '" + r + "'"};
        out += "requires " + r + "\n";
    }
    for (const LibFile& f : lib.files) {
        if (!validPath(f.path)) throw LibError{"invalid file path '" + f.path + "'"};
        if (f.text.size() > kMaxLibFileBytes) throw LibError{"file '" + f.path + "' is too large"};
        out += "file " + f.path + " " + std::to_string(f.text.size()) + "\n" + f.text + "\n";
    }
    return out;
}

} // namespace coyote::pkg
