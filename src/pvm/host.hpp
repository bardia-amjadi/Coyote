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

struct HostError {
    std::string message;
};

// Thrown by sys.exit: ends the program with this exit code (not an error).
struct HostExit {
    int code;
};

struct HostContext {
    std::ostream& out;
    std::istream* in = nullptr;  // null: no input available (e.g. while the debugger owns stdin)
    bool unbuffered = false;     // flush after every write (interactive front ends)
    const std::vector<std::string>* args = nullptr;  // program arguments (sys.arg)
};

using HostFn = std::function<Value(const std::vector<Value>& args, HostContext& ctx)>;

struct HostFunction {
    std::string name;        // "<module>.<function>", e.g. "con.println"
    Sig sig;
    std::string permission;  // capability required to call it, e.g. "console"
    HostFn fn;
};

class HostRegistry {
public:
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
    HostContext context(std::ostream& out) const { return HostContext{out, in_, unbuffered_, &args_}; }

private:
    std::map<std::string, HostFunction> funcs_;
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

} // namespace pvm
