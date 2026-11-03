#include "manifest.hpp"

#include <algorithm>
#include <filesystem>
#include <set>

#include "json.hpp"

namespace coyote::pkg {

namespace {

[[noreturn]] void bad(const Json& at, const std::string& msg) { throw ManifestError{at.line, at.col, msg}; }

const char* kindName(Json::Kind k) {
    switch (k) {
        case Json::Null: return "null";
        case Json::Bool: return "a boolean";
        case Json::Number: return "a number";
        case Json::String: return "a string";
        case Json::Array: return "an array";
        case Json::Object: return "an object";
    }
    return "?";
}

void needString(const Json& v, const std::string& what) {
    if (v.kind != Json::String) bad(v, what + " must be a string, not " + kindName(v.kind));
}

bool nameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}

void checkName(const Json& v, const std::string& what) {
    needString(v, what);
    const std::string& s = v.string;
    if (s.empty() || s.size() > 64) bad(v, what + " must be 1 to 64 characters long");
    for (char c : s)
        if (!nameChar(c)) bad(v, what + " may only contain letters, digits, '_', '-' and '.'");
}

void checkVersion(const Json& v) {
    needString(v, "'version'");
    const std::string& s = v.string;
    // MAJOR.MINOR.PATCH, digits only, no leading zeros.
    size_t pos = 0;
    for (int part = 0; part < 3; ++part) {
        size_t start = pos;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
        size_t len = pos - start;
        if (len == 0 || (len > 1 && s[start] == '0') || len > 9) bad(v, "'version' must look like MAJOR.MINOR.PATCH (for example \"1.0.0\")");
        if (part < 2) {
            if (pos >= s.size() || s[pos] != '.') bad(v, "'version' must look like MAJOR.MINOR.PATCH (for example \"1.0.0\")");
            ++pos;
        }
    }
    if (pos != s.size()) bad(v, "'version' must look like MAJOR.MINOR.PATCH (for example \"1.0.0\")");
}

// A relative path with '/' separators that stays inside the project.
void checkEntry(const Json& v) {
    needString(v, "'entry'");
    const std::string& s = v.string;
    if (s.size() < 5 || s.compare(s.size() - 4, 4, ".coy") != 0) bad(v, "'entry' must be a path to a .coy file");
    if (s[0] == '/' || s.find('\\') != std::string::npos || (s.size() > 1 && s[1] == ':'))
        bad(v, "'entry' must be a relative path that uses '/' separators");
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t next = s.find('/', pos);
        std::string seg = s.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        if (seg.empty() || seg == "." || seg == "..") bad(v, "'entry' must not contain empty, '.' or '..' path segments");
        if (next == std::string::npos) break;
        pos = next + 1;
    }
}

std::vector<std::string> splitWords(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

Step parseStep(const Json& v, const std::string& wf) {
    Step st;
    st.line = v.line;
    st.col = v.col;
    if (v.kind == Json::String) {
        if (!v.string.empty() && v.string[0] == '@') {
            st.kind = Step::Ref;
            st.ref = v.string.substr(1);
            if (st.ref.empty()) bad(v, "'@' must be followed by a workflow name");
            for (char c : st.ref)
                if (!nameChar(c)) bad(v, "invalid workflow name in reference '" + v.string + "'");
            return st;
        }
        st.argv = splitWords(v.string);
        if (st.argv.empty()) bad(v, "a step in workflow '" + wf + "' is empty");
        return st;
    }
    if (v.kind == Json::Array) {
        if (v.items.empty()) bad(v, "a step in workflow '" + wf + "' is an empty array");
        for (const Json& a : v.items) {
            if (a.kind != Json::String) bad(a, "step arguments must be strings, not " + std::string(kindName(a.kind)));
            st.argv.push_back(a.string);
        }
        if (st.argv[0].empty()) bad(v.items[0], "the program name of a step must not be empty");
        return st;
    }
    bad(v, "a step must be a string or an array of strings, not " + std::string(kindName(v.kind)));
}

bool isHex(const std::string& s, size_t len) {
    if (s.size() != len) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

Dependency parseDependency(const std::string& name, const Json& v) {
    Dependency d;
    d.name = name;
    d.line = v.line;
    d.col = v.col;
    if (name.empty() || name.size() > 64) bad(v, "dependency names must be 1 to 64 characters long");
    for (char c : name)
        if (!nameChar(c) || c == '.') bad(v, "invalid dependency name '" + name + "' (letters, digits, '_' and '-' only)");
    if (v.kind == Json::String) {  // "name": "1.2.3" -- a registry version
        d.kind = Dependency::Registry;
        Json copy = v;
        checkVersion(copy);
        d.version = v.string;
        return d;
    }
    if (v.kind != Json::Object) bad(v, "dependency '" + name + "' must be a version string or an object, not " + kindName(v.kind));
    static const std::set<std::string> keys = {"path", "git", "rev", "url", "sha256", "version"};
    for (const auto& kv : v.members)
        if (!keys.count(kv.first)) bad(kv.second, "unknown key '" + kv.first + "' in dependency '" + name + "' (known: path, git, rev, url, sha256, version)");
    const Json* path = v.find("path");
    const Json* git = v.find("git");
    const Json* url = v.find("url");
    int sources = (path ? 1 : 0) + (git ? 1 : 0) + (url ? 1 : 0);
    if (sources != 1) bad(v, "dependency '" + name + "' needs exactly one of 'path', 'git' or 'url'");
    if (const Json* ver = v.find("version")) {
        checkVersion(*ver);
        d.version = ver->string;
    }
    if (path) {
        needString(*path, "'path'");
        const std::string& p = path->string;
        if (p.empty() || p[0] == '/' || p.find('\\') != std::string::npos || (p.size() > 1 && p[1] == ':'))
            bad(*path, "'path' must be a relative directory that uses '/' separators");
        if (v.find("rev") || v.find("sha256")) bad(v, "'rev' and 'sha256' do not apply to a path dependency");
        d.kind = Dependency::Path;
        d.path = p;
    } else if (git) {
        needString(*git, "'git'");
        if (git->string.empty()) bad(*git, "'git' must not be empty");
        const Json* rev = v.find("rev");
        if (!rev) bad(v, "git dependency '" + name + "' needs a 'rev' (branch, tag or commit)");
        needString(*rev, "'rev'");
        if (rev->string.empty() || rev->string[0] == '-') bad(*rev, "invalid 'rev'");
        if (git->string[0] == '-') bad(*git, "invalid 'git' value");
        if (v.find("sha256")) bad(v, "'sha256' does not apply to a git dependency");
        d.kind = Dependency::Git;
        d.git = git->string;
        d.rev = rev->string;
    } else {
        needString(*url, "'url'");
        const Json* sha = v.find("sha256");
        if (!sha) bad(v, "url dependency '" + name + "' needs a 'sha256'");
        needString(*sha, "'sha256'");
        if (!isHex(sha->string, 64)) bad(*sha, "'sha256' must be 64 lowercase hex digits");
        if (url->string.compare(0, 8, "https://") != 0) bad(*url, "'url' must start with https://");
        if (v.find("rev")) bad(v, "'rev' does not apply to a url dependency");
        d.kind = Dependency::Url;
        d.url = url->string;
        d.sha256 = sha->string;
    }
    return d;
}

void checkReferences(const Manifest& m) {
    // Every reference must name a workflow, and references must not form a cycle.
    for (const auto& w : m.workflows)
        for (const Step& st : w.second.steps)
            if (st.kind == Step::Ref && !m.workflows.count(st.ref))
                throw ManifestError{st.line, st.col, "workflow '" + w.first + "' refers to unknown workflow '" + st.ref + "'"};

    enum { White, Grey, Black };
    std::map<std::string, int> colour;
    std::vector<std::string> path;
    // Iterative DFS keeps this safe for any number of workflows.
    for (const auto& root : m.workflows) {
        if (colour[root.first] != White) continue;
        struct Frame { const Workflow* w; size_t next; };
        std::vector<Frame> stack{{&root.second, 0}};
        colour[root.first] = Grey;
        path.push_back(root.first);
        while (!stack.empty()) {
            Frame& f = stack.back();
            bool descended = false;
            while (f.next < f.w->steps.size()) {
                const Step& st = f.w->steps[f.next++];
                if (st.kind != Step::Ref) continue;
                int c = colour[st.ref];
                if (c == Grey) {
                    std::string cyc;
                    auto it = std::find(path.begin(), path.end(), st.ref);
                    for (; it != path.end(); ++it) cyc += *it + " -> ";
                    throw ManifestError{st.line, st.col, "workflows refer to each other in a cycle: " + cyc + st.ref};
                }
                if (c == White) {
                    colour[st.ref] = Grey;
                    path.push_back(st.ref);
                    stack.push_back({&m.workflows.at(st.ref), 0});
                    descended = true;
                    break;
                }
            }
            if (descended) continue;
            colour[path.back()] = Black;
            path.pop_back();
            stack.pop_back();
        }
    }
}

std::string stemOf(const std::string& entry) {
    size_t slash = entry.find_last_of('/');
    std::string base = slash == std::string::npos ? entry : entry.substr(slash + 1);
    return base.substr(0, base.size() - 4);
}

} // namespace

std::string findManifestFile(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const bool here = dir.empty() || dir == ".";
    fs::path d = here ? fs::path(".") : fs::path(dir);
    auto shown = [&](const fs::path& file) { return here ? file.filename().string() : file.string(); };
    if (fs::is_regular_file(d / "packages.json", ec)) return shown(d / "packages.json");
    std::vector<std::string> found;
    for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".cyproj") found.push_back(it->path().string());
    if (found.empty()) return "";
    std::sort(found.begin(), found.end());
    return shown(found.front());
}

std::string Dependency::canonical() const {
    switch (kind) {
        case Path: return "path:" + path + "|" + version;
        case Git: return "git:" + git + "|" + rev + "|" + version;
        case Url: return "url:" + url + "|" + sha256 + "|" + version;
        case Registry: return "registry:" + version;
    }
    return "";
}

Manifest loadManifest(const std::string& text) {
    Json root;
    try {
        root = parseJson(text);
    } catch (const JsonError& e) {
        throw ManifestError{e.line, e.col, "invalid JSON: " + e.message};
    }
    if (root.kind != Json::Object) bad(root, "packages.json must contain a JSON object");

    static const std::set<std::string> known = {"schemaVersion", "name", "version", "entry", "type", "requires", "workflows", "dependencies",
                                                "description", "output", "run", "editor", "cyprojVersion"};
    for (const auto& m : root.members)
        if (!known.count(m.first)) {
            Json at = m.second;
            bad(at, "unknown key '" + m.first + "' (known keys: schemaVersion, name, version, entry, type, requires, workflows, dependencies, description, output, run)");
        }

    Manifest m;
    const Json* sv = root.find("schemaVersion");
    if (!sv) throw ManifestError{root.line, root.col, "missing required key 'schemaVersion'"};
    if (sv->kind != Json::Number || !sv->isInteger) bad(*sv, "'schemaVersion' must be an integer");
    if (sv->number != 1) bad(*sv, "unsupported schemaVersion " + std::to_string(static_cast<long long>(sv->number)) + " (this cpx supports 1)");
    m.schemaVersion = 1;

    const Json* name = root.find("name");
    if (!name) throw ManifestError{root.line, root.col, "missing required key 'name'"};
    checkName(*name, "'name'");
    m.name = name->string;

    if (const Json* v = root.find("version")) {
        checkVersion(*v);
        m.version = v->string;
    }
    if (const Json* t = root.find("type")) {
        if (t->kind != Json::String || (t->string != "app" && t->string != "library")) bad(*t, "'type' must be \"app\" or \"library\"");
        m.type = t->string;
    }
    if (const Json* rq = root.find("requires")) {
        if (rq->kind != Json::Array) bad(*rq, "'requires' must be an array of library names");
        for (const Json& item : rq->items) {
            if (item.kind != Json::String) bad(item, "'requires' entries must be strings");
            m.requires_.push_back(item.string);
        }
    }
    if (const Json* e = root.find("entry")) {
        checkEntry(*e);
        m.entry = e->string;
    }

    if (const Json* ds = root.find("description")) {
        if (ds->kind != Json::String) bad(*ds, "'description' must be a string");
        m.description = ds->string;
    }
    if (const Json* o = root.find("output")) {
        if (o->kind != Json::Object) bad(*o, "'output' must be an object");
        m.hasOutput = true;
        for (const auto& kv : o->members) {
            const Json& v = kv.second;
            if (kv.first == "directory") {
                if (v.kind != Json::String || v.string.empty() || v.string[0] == '/' || v.string.find("..") != std::string::npos || v.string.find(':') != std::string::npos)
                    bad(v, "'output.directory' must be a relative folder inside the project (no '..')");
                m.outDir = v.string;
            } else if (kv.first == "name") {
                if (v.kind != Json::String || v.string.empty() || v.string.find_first_of("/\\:") != std::string::npos) bad(v, "'output.name' must be a file name without folders");
                m.outName = v.string;
            } else if (kv.first == "kind") {
                if (v.kind != Json::String || (v.string != "native" && v.string != "ml")) bad(v, "'output.kind' must be \"native\" or \"ml\"");
                m.outKind = v.string;
            } else if (kv.first == "mode") {
                if (v.kind != Json::String || (v.string != "auto" && v.string != "bundle" && v.string != "native-only")) bad(v, "'output.mode' must be \"auto\", \"bundle\" or \"native-only\"");
                m.outMode = v.string;
            } else if (kv.first == "keepUnused") {
                if (v.kind != Json::Bool) bad(v, "'output.keepUnused' must be true or false");
                m.keepUnused = v.boolean;
            } else {
                Json at = v;
                bad(at, "unknown key 'output." + kv.first + "' (known: directory, name, kind, mode, keepUnused)");
            }
        }
    }
    if (const Json* r = root.find("run")) {
        if (r->kind != Json::Object) bad(*r, "'run' must be an object");
        for (const auto& kv : r->members) {
            const Json& v = kv.second;
            if (kv.first != "args") { Json at = v; bad(at, "unknown key 'run." + kv.first + "' (known: args)"); }
            if (v.kind != Json::Array) bad(v, "'run.args' must be an array of strings");
            for (const Json& item : v.items) {
                if (item.kind != Json::String) bad(item, "'run.args' entries must be strings");
                m.runArgs.push_back(item.string);
            }
        }
    }

    if (const Json* d = root.find("dependencies")) {
        if (d->kind != Json::Object) bad(*d, "'dependencies' must be an object");
        for (const auto& kv : d->members) m.dependencies.push_back(parseDependency(kv.first, kv.second));
        std::sort(m.dependencies.begin(), m.dependencies.end(),
                  [](const Dependency& a, const Dependency& b) { return a.name < b.name; });
    }

    if (const Json* w = root.find("workflows")) {
        if (w->kind != Json::Object) bad(*w, "'workflows' must be an object");
        for (const auto& kv : w->members) {
            const Json& list = kv.second;
            Json keyAt = list;
            for (char c : kv.first)
                if (!nameChar(c)) bad(keyAt, "invalid workflow name '" + kv.first + "'");
            if (kv.first.empty() || kv.first.size() > 64) bad(keyAt, "workflow names must be 1 to 64 characters long");
            static const std::set<std::string> reserved = {"list", "check", "run"};
            if (reserved.count(kv.first)) bad(keyAt, "'" + kv.first + "' is a cpx command and cannot be used as a workflow name");
            if (m.workflows.size() >= 256) bad(keyAt, "too many workflows (at most 256)");
            Workflow wf;
            wf.name = kv.first;
            if (list.kind == Json::Array) {
                if (list.items.empty()) bad(list, "workflow '" + kv.first + "' has no steps");
                for (const Json& s : list.items) wf.steps.push_back(parseStep(s, kv.first));
            } else {
                wf.steps.push_back(parseStep(list, kv.first));  // a single step may be written without an array
            }
            m.workflows[kv.first] = std::move(wf);
        }
    }

    // Defaults, so a vanilla project (app.coy + packages.json with no workflows) works. Build output
    // goes to a hidden directory; source files are never moved or written.
    if (!m.workflows.count("build")) {
        Workflow b;
        b.name = "build";
        b.synthesized = true;
        if (m.hasOutput) {
            // Built according to the project's output settings: <directory>/<name>[.exe] (native) or <name>.ml.
            const std::string stem = m.outName.empty() ? m.name : m.outName;
            Step mk;
            mk.kind = Step::Mkdir;
            mk.argv = {m.outDir};
            b.steps.push_back(mk);
            std::vector<std::string> cc = {"coyote", "build", m.entry};
            if (m.keepUnused) cc.push_back("--keep-unused");
            if (m.outKind == "ml") {
                cc.push_back("-o");
                cc.push_back(m.outDir + "/" + stem + ".ml");
                Step run;
                run.argv = cc;
                b.steps.push_back(run);
            } else {
                Step mk2;
                mk2.kind = Step::Mkdir;
                mk2.argv = {".coyote/out"};
                b.steps.push_back(mk2);
                cc.push_back("-o");
                cc.push_back(".coyote/out/" + stemOf(m.entry) + ".ml");
                Step run;
                run.argv = cc;
                b.steps.push_back(run);
                Step nat;
#ifdef _WIN32
                const std::string exe = m.outDir + "/" + stem + ".exe";
#else
                const std::string exe = m.outDir + "/" + stem;
#endif
                nat.argv = {"coyote", "llvm", ".coyote/out/" + stemOf(m.entry) + ".ml", "-o", exe};
                if (m.outMode == "bundle") nat.argv.push_back("--bundle");
                else if (m.outMode == "native-only") nat.argv.push_back("--native-only");
                b.steps.push_back(nat);
            }
        } else {
            Step mk;
            mk.kind = Step::Mkdir;
            mk.argv = {".coyote/out"};
            Step run;
            run.argv = {"coyote", "build", m.entry, "-o", ".coyote/out/" + stemOf(m.entry) + ".ml"};
            b.steps = {mk, run};
        }
        m.workflows["build"] = std::move(b);
    }
    if (!m.workflows.count("start")) {
        Workflow s;
        s.name = "start";
        s.synthesized = true;
        if (m.hasOutput) {  // compile and run on the PVM with every permission; run.args reach the program
            Step run;
            run.argv = {"coyote", "run", m.entry};
            if (!m.runArgs.empty()) {
                run.argv.push_back("--");
                run.argv.insert(run.argv.end(), m.runArgs.begin(), m.runArgs.end());
            }
            s.steps = {run};
        } else {
            Step ref;
            ref.kind = Step::Ref;
            ref.ref = "build";
            Step run;
            run.argv = {"pvm", ".coyote/out/" + stemOf(m.entry) + ".ml"};
            s.steps = {ref, run};
        }
        m.workflows["start"] = std::move(s);
    }

    checkReferences(m);
    return m;
}

} // namespace coyote::pkg
