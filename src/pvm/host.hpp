// Host intrinsic registry. Source code can *declare* a host function with `hand:`, but the PVM
// only runs functions that are explicitly registered here, with a matching signature, and whose
// permission has been granted to the VM instance.
#pragma once

#include <functional>
#include <map>
#include <ostream>
#include <set>
#include <string>
#include <vector>

#include "ml.hpp"
#include "value.hpp"

namespace pvm {

class Heap;
class HostRegistry;

struct HostError {
    std::string message;
};

// Thrown by sys.exit: ends the program with this exit code (not an error).
struct HostExit {
    int code;
};

struct HostContext {
    HostContext(std::ostream& o, std::istream* i = nullptr, bool unbuf = false, const std::vector<std::string>* a = nullptr, const HostRegistry* r = nullptr, Heap* h = nullptr)
        : out(o), in(i), unbuffered(unbuf), args(a), registry(r), heap(h) {}
    std::ostream& out;
    std::istream* in = nullptr;  // null: no input available (e.g. while the debugger owns stdin)
    bool unbuffered = false;     // flush after every write (interactive front ends)
    const std::vector<std::string>* args = nullptr;  // program arguments (sys.arg)
    const HostRegistry* registry = nullptr;          // the registry that runs this program (nested programs reuse it)
    Heap* heap = nullptr;                            // the running program's heap (statistics for rt.*)
    const std::function<void(const std::function<void(const Value&)>&)>* roots = nullptr;  // GC roots while a host function runs (set by the interpreter)
    std::string lastError;                           // message of the last exception a `try` caught (mouth sys.exception)
    bool hasLastError = false;
    std::string lastTrace;                           // stack trace of that exception (mouth sys.stackTrace)
    Value lastPayload = Value::nullObj();            // the exception object it carried, or null for a runtime error (mouth sys.exceptionObject); a GC root
    long long exitCode = 0;                          // mouth sys.exitCode: the exit code when the program ends normally
    bool exitCodeSet = false;
};

using HostFn = std::function<Value(const std::vector<Value>& args, HostContext& ctx)>;

struct HostFunction {
    std::string name;        // "<module>.<function>", e.g. "con.println"
    Sig sig;
    std::string permission;  // capability required to call it, e.g. "console"
    HostFn fn;
};

// A host variable (`mouth:module name {get;}` in Coyote source). Reading gives an obj: whatever value the host returns is boxed.
using MouthGetFn = std::function<Value(HostContext& ctx)>;
using MouthSetFn = std::function<void(const Value& value, HostContext& ctx)>;  // value is an obj; empty: read-only

struct HostMouth {
    std::string name;        // "<module>.<name>", e.g. "sys.exception"
    std::string permission;  // capability required to read or write it ("" = always allowed)
    MouthGetFn get;
    MouthSetFn set;
};

class HostRegistry {
public:
    void addMouth(HostMouth m) {
        std::string key = m.name;
        mouths_.emplace(std::move(key), std::move(m));
    }
    const HostMouth* findMouth(const std::string& name) const {
        auto it = mouths_.find(name);
        return it == mouths_.end() ? nullptr : &it->second;
    }

    void add(HostFunction f) {
        std::string key = f.name;
        funcs_.emplace(std::move(key), std::move(f));
    }
    const HostFunction* find(const std::string& name) const {
        auto it = funcs_.find(name);
        return it == funcs_.end() ? nullptr : &it->second;
    }

    void grant(const std::string& permission) { granted_.insert(permission); }
    // An empty permission means "pure function, always allowed" (math, text).
    bool isGranted(const std::string& permission) const { return permission.empty() || granted_.count(permission) != 0; }
    void setInput(std::istream* in) { in_ = in; }
    std::istream* input() const { return in_; }
    void setUnbuffered(bool v) { unbuffered_ = v; }
    void setArgs(std::vector<std::string> a) { args_ = std::move(a); }
    HostContext context(std::ostream& out) const { return HostContext{out, in_, unbuffered_, &args_, this, nullptr}; }

private:
    std::map<std::string, HostFunction> funcs_;
    std::map<std::string, HostMouth> mouths_;
    std::set<std::string> granted_;
    std::istream* in_ = nullptr;
    bool unbuffered_ = false;
    std::vector<std::string> args_;
};

// Registers the built-in console host functions (permission "console").
void registerConsoleHosts(HostRegistry& registry);
// Registers everything: console (con.*), math.*, text.* (pure), time.* ("time") and random.* ("random").
void registerAllHosts(HostRegistry& registry);
// The permissions a program gets by default (console, time, random).
void grantDefaultPermissions(HostRegistry& registry);
// Every permission: console, time, random, file, system, net (`coyote run`, Coyote Studio).
void grantAllPermissions(HostRegistry& registry);
// Registers the file/dir/env/sys/http modules (host_sys.cpp).
void registerSystemHosts(HostRegistry& registry);
// Registers proc.* (permission "process"), rt.* ("system") and coy.* ("interop"): host_proc.cpp.
void registerProcessHosts(HostRegistry& registry);
// Registers the win.* module (host_win.cpp): permission "windows"; real on Windows, a runtime error elsewhere.
void registerWindowsHosts(HostRegistry& registry);
// Registers the ui.* module (host_ui.cpp): permission "ui"; Win32 controls on Windows, a headless backend elsewhere (Vision UI).
void registerUiHosts(HostRegistry& registry);

} // namespace pvm
