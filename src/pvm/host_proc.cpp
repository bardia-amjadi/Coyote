// Host modules of 0.6.4: proc.* (run and start programs; permission "process"), rt.* (information about the runtime;
// "system") and coy.* (load and run other Coyote ML programs inside this VM; "interop").
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>

#include "heap.hpp"
#include "host.hpp"
#include "interpreter.hpp"
#include "loader.hpp"
#include "verifier.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#ifndef COYOTE_VERSION
#define COYOTE_VERSION "unknown"
#endif

namespace pvm {

namespace {

using Args = std::vector<Value>;

Sig sig(std::vector<Type> params, Type ret) {
    Sig s;
    s.params = std::move(params);
    s.ret = ret;
    return s;
}

void add(HostRegistry& r, const std::string& name, std::vector<Type> params, Type ret, const std::string& perm,
         std::function<Value(const Args&, HostContext&)> fn) {
    r.add(HostFunction{name, sig(std::move(params), ret), perm, std::move(fn)});
}

constexpr size_t kMaxCapture = 32u * 1024u * 1024u;

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    if (s.empty()) return out;
    size_t pos = 0;
    for (;;) {
        size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) { out.push_back(s.substr(pos)); break; }
        out.push_back(s.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return out;
}

// ------------------------------------------------------------------------------------------- proc

struct ProcResult {
    std::string out, err;
    bool timedOut = false;
} g_proc;

#ifdef _WIN32

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string quoteArg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos) return a;
    std::string r = "\"";
    size_t backslashes = 0;
    for (char c : a) {
        if (c == '\\') { ++backslashes; continue; }
        if (c == '"') { r.append(backslashes * 2 + 1, '\\'); r += '"'; }
        else { r.append(backslashes, '\\'); r += c; }
        backslashes = 0;
    }
    r.append(backslashes * 2, '\\');
    r += '"';
    return r;
}

std::wstring commandLine(const std::string& program, const std::vector<std::string>& args) {
    std::string c = quoteArg(program);
    for (const std::string& a : args) c += " " + quoteArg(a);
    return widen(c);
}

std::map<long long, HANDLE> g_children;

long long startProcess(const std::string& program, const std::vector<std::string>& args, const std::string& cwd,
                       HANDLE hIn, HANDLE hOut, HANDLE hErr, HANDLE* outHandle) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (hIn || hOut || hErr) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = hIn ? hIn : GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = hOut ? hOut : GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = hErr ? hErr : GetStdHandle(STD_ERROR_HANDLE);
    }
    PROCESS_INFORMATION pi{};
    std::wstring cl = commandLine(program, args);
    std::wstring wd = widen(cwd);
    if (!CreateProcessW(nullptr, &cl[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, wd.empty() ? nullptr : wd.c_str(), &si, &pi))
        throw HostError{"cannot start '" + program + "' (error " + std::to_string(GetLastError()) + ")"};
    CloseHandle(pi.hThread);
    *outHandle = pi.hProcess;
    return static_cast<long long>(pi.dwProcessId);
}

void readAvailable(HANDLE h, std::string& into, bool& open) {
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) { open = false; return; }
        if (avail == 0) return;
        char buf[4096];
        DWORD got = 0;
        if (!ReadFile(h, buf, sizeof(buf) < avail ? sizeof(buf) : avail, &got, nullptr) || got == 0) { open = false; return; }
        if (into.size() < kMaxCapture) into.append(buf, got);
    }
}

int runCapture(const std::string& program, const std::vector<std::string>& args, const std::string& cwd,
               const std::string& input, long long timeoutMs) {
    g_proc = ProcResult();
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
    if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 0) || !CreatePipe(&errR, &errW, &sa, 0))
        throw HostError{"cannot create pipes"};
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
    HANDLE hp = nullptr;
    try {
        startProcess(program, args, cwd, inR, outW, errW, &hp);
    } catch (...) {
        CloseHandle(inR); CloseHandle(inW); CloseHandle(outR); CloseHandle(outW); CloseHandle(errR); CloseHandle(errW);
        throw;
    }
    CloseHandle(inR); CloseHandle(outW); CloseHandle(errW);
    if (!input.empty()) { DWORD w = 0; WriteFile(inW, input.data(), static_cast<DWORD>(input.size()), &w, nullptr); }
    CloseHandle(inW);
    bool oo = true, eo = true;
    auto t0 = std::chrono::steady_clock::now();
    int code = 0;
    for (;;) {
        readAvailable(outR, g_proc.out, oo);
        readAvailable(errR, g_proc.err, eo);
        DWORD w = WaitForSingleObject(hp, 5);
        if (w == WAIT_OBJECT_0) {
            readAvailable(outR, g_proc.out, oo);
            readAvailable(errR, g_proc.err, eo);
            DWORD ec = 0;
            GetExitCodeProcess(hp, &ec);
            code = static_cast<int>(ec);
            break;
        }
        if (timeoutMs > 0 && std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() >= timeoutMs) {
            TerminateProcess(hp, 1);
            WaitForSingleObject(hp, 1000);
            g_proc.timedOut = true;
            code = -1;
            break;
        }
    }
    CloseHandle(outR); CloseHandle(errR); CloseHandle(hp);
    return code;
}

long long startDetached(const std::string& program, const std::vector<std::string>& args, const std::string& cwd) {
    HANDLE hp = nullptr;
    long long pid = startProcess(program, args, cwd, nullptr, nullptr, nullptr, &hp);
    g_children[pid] = hp;
    return pid;
}

// exit code, or -1 while running (timeout), or -2 for an unknown pid
long long waitProcess(long long pid, long long timeoutMs) {
    auto it = g_children.find(pid);
    if (it == g_children.end()) return -2;
    DWORD w = WaitForSingleObject(it->second, timeoutMs < 0 ? INFINITE : static_cast<DWORD>(timeoutMs));
    if (w != WAIT_OBJECT_0) return -1;
    DWORD ec = 0;
    GetExitCodeProcess(it->second, &ec);
    CloseHandle(it->second);
    g_children.erase(it);
    return static_cast<long long>(ec);
}

bool killProcess(long long pid) {
    auto it = g_children.find(pid);
    HANDLE h = it != g_children.end() ? it->second : OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    bool ok = TerminateProcess(h, 1) != 0;
    if (it == g_children.end()) CloseHandle(h);
    return ok;
}

bool isRunning(long long pid) {
    auto it = g_children.find(pid);
    HANDLE h = it != g_children.end() ? it->second : OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    bool running = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    if (it == g_children.end()) CloseHandle(h);
    return running;
}

#else  // POSIX

std::vector<char*> argvOf(const std::string& program, const std::vector<std::string>& args, std::vector<std::string>& hold) {
    hold.push_back(program);
    for (const std::string& a : args) hold.push_back(a);
    std::vector<char*> v;
    for (std::string& s : hold) v.push_back(&s[0]);
    v.push_back(nullptr);
    return v;
}

pid_t spawn(const std::string& program, const std::vector<std::string>& args, const std::string& cwd, int in, int out, int err) {
    std::vector<std::string> hold;
    std::vector<char*> argv = argvOf(program, args, hold);
    int status[2];
    if (pipe(status) != 0) throw HostError{"cannot create a pipe"};
    fcntl(status[1], F_SETFD, FD_CLOEXEC);
    pid_t pid = fork();
    if (pid < 0) { close(status[0]); close(status[1]); throw HostError{"cannot fork"}; }
    if (pid == 0) {
        close(status[0]);
        if (in >= 0) dup2(in, 0);
        if (out >= 0) dup2(out, 1);
        if (err >= 0) dup2(err, 2);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) { int e = errno; ssize_t w = write(status[1], &e, sizeof e); (void)w; _exit(127); }
        execvp(program.c_str(), argv.data());
        int e = errno;
        ssize_t w = write(status[1], &e, sizeof e);
        (void)w;
        _exit(127);
    }
    close(status[1]);
    int e = 0;
    ssize_t n = read(status[0], &e, sizeof e);
    close(status[0]);
    if (n == static_cast<ssize_t>(sizeof e)) {
        int st = 0;
        waitpid(pid, &st, 0);
        throw HostError{"cannot start '" + program + "': " + std::strerror(e)};
    }
    return pid;
}

int exitCodeOf(int st) { return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0); }

int runCapture(const std::string& program, const std::vector<std::string>& args, const std::string& cwd,
               const std::string& input, long long timeoutMs) {
    g_proc = ProcResult();
    int pin[2], pout[2], perr[2];
    if (pipe(pin) != 0 || pipe(pout) != 0 || pipe(perr) != 0) throw HostError{"cannot create pipes"};
    // The child must only keep its own ends (dup2 to 0/1/2 clears the flag on those copies). Without this the child inherits the
    // write end of its own stdin and never sees end-of-file, so `sort` or `cat` would wait until the time limit.
    for (int fd : {pin[0], pin[1], pout[0], pout[1], perr[0], perr[1]}) fcntl(fd, F_SETFD, FD_CLOEXEC);
    pid_t pid;
    try {
        pid = spawn(program, args, cwd, pin[0], pout[1], perr[1]);
    } catch (...) {
        for (int fd : {pin[0], pin[1], pout[0], pout[1], perr[0], perr[1]}) close(fd);
        throw;
    }
    close(pin[0]); close(pout[1]); close(perr[1]);
    signal(SIGPIPE, SIG_IGN);
    fcntl(pin[1], F_SETFL, O_NONBLOCK);
    size_t written = 0;
    if (input.empty()) { close(pin[1]); pin[1] = -1; }
    int fo = pout[0], fe = perr[0];
    auto t0 = std::chrono::steady_clock::now();
    int status = 0;
    bool exited = false;
    while (fo >= 0 || fe >= 0) {
        struct pollfd p[3];
        int n = 0;
        if (fo >= 0) p[n++] = {fo, POLLIN, 0};
        if (fe >= 0) p[n++] = {fe, POLLIN, 0};
        if (pin[1] >= 0) p[n++] = {pin[1], POLLOUT, 0};
        int pr = poll(p, static_cast<nfds_t>(n), 20);
        if (pr > 0) {
            for (int i = 0; i < n; ++i) {
                if (p[i].fd == pin[1]) {
                    if (p[i].revents & (POLLOUT | POLLERR | POLLHUP)) {
                        ssize_t w = write(pin[1], input.data() + written, input.size() - written);
                        if (w > 0) written += static_cast<size_t>(w);
                        if (w < 0 && errno != EAGAIN) written = input.size();
                        if (written >= input.size()) { close(pin[1]); pin[1] = -1; }
                    }
                    continue;
                }
                if (p[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                    char buf[4096];
                    ssize_t r = read(p[i].fd, buf, sizeof buf);
                    std::string& dst = p[i].fd == fo ? g_proc.out : g_proc.err;
                    if (r > 0) { if (dst.size() < kMaxCapture) dst.append(buf, static_cast<size_t>(r)); }
                    else if (p[i].fd == fo) { close(fo); fo = -1; }
                    else { close(fe); fe = -1; }
                }
            }
        }
        if (timeoutMs > 0 && std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() >= timeoutMs) {
            kill(pid, SIGKILL);
            g_proc.timedOut = true;
            break;
        }
    }
    if (pin[1] >= 0) close(pin[1]);
    if (fo >= 0) close(fo);
    if (fe >= 0) close(fe);
    // the pipes are closed; wait for the exit (bounded when a timeout was requested)
    for (;;) {
        pid_t w = waitpid(pid, &status, timeoutMs > 0 ? WNOHANG : 0);
        if (w == pid) { exited = true; break; }
        if (w < 0) break;
        if (timeoutMs > 0) {
            if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() >= timeoutMs + 2000) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                g_proc.timedOut = true;
                exited = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    if (g_proc.timedOut) return -1;
    return exited ? exitCodeOf(status) : -1;
}

std::map<long long, int> g_exited;  // pid -> exit code of children already reaped by isRunning

long long startDetached(const std::string& program, const std::vector<std::string>& args, const std::string& cwd) {
    int devnull = open("/dev/null", O_RDWR);
    pid_t pid = spawn(program, args, cwd, devnull, -1, -1);
    if (devnull >= 0) close(devnull);
    return static_cast<long long>(pid);
}

long long waitProcess(long long pid, long long timeoutMs) {
    auto done = g_exited.find(pid);
    if (done != g_exited.end()) { int c = done->second; g_exited.erase(done); return c; }
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        int st = 0;
        pid_t w = waitpid(static_cast<pid_t>(pid), &st, WNOHANG);
        if (w == static_cast<pid_t>(pid)) return exitCodeOf(st);
        if (w < 0) return -2;
        if (timeoutMs >= 0 && std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() >= timeoutMs) return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

bool killProcess(long long pid) { return kill(static_cast<pid_t>(pid), SIGKILL) == 0; }

bool isRunning(long long pid) {
    if (g_exited.count(pid)) return false;
    int st = 0;
    pid_t w = waitpid(static_cast<pid_t>(pid), &st, WNOHANG);
    if (w == static_cast<pid_t>(pid)) { g_exited[pid] = exitCodeOf(st); return false; }
    if (w < 0) return kill(static_cast<pid_t>(pid), 0) == 0;  // not our child: just ask
    return true;
}

#endif

// -------------------------------------------------------------------------------------------- rt

long long processMemory() {
#ifdef _WIN32
    return -1;
#else
    std::ifstream f("/proc/self/statm");
    long long pages = 0, resident = 0;
    if (!(f >> pages >> resident)) return -1;
    return resident * static_cast<long long>(sysconf(_SC_PAGESIZE));
#endif
}

// ------------------------------------------------------------------------------------------- coy

struct Loaded {
    Module module;
    std::unique_ptr<Heap> heap;
    std::string path;
};
std::vector<std::unique_ptr<Loaded>> g_loaded;
std::string g_coyOut, g_coyErr;

Loaded& loadedAt(long long h) {
    if (h < 0 || static_cast<size_t>(h) >= g_loaded.size() || !g_loaded[static_cast<size_t>(h)])
        throw HostError{"invalid Coyote module handle " + std::to_string(h)};
    return *g_loaded[static_cast<size_t>(h)];
}

std::string typeText(const Type& t) { return typeName(t); }

// Decodes "<len>:<text>" items written by Standard.Coyote.InteropServices.
// Number of bytes that hold the next `count` UTF-8 characters starting at pos (item lengths count characters, like Standard.Text).
size_t utf8Span(const std::string& s, size_t pos, size_t count) {
    size_t i = pos;
    while (count > 0 && i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        i += n;
        --count;
    }
    if (count > 0 || i > s.size()) throw HostError{"malformed argument list"};
    return i - pos;
}

std::vector<std::string> decodeItems(const std::string& s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t colon = s.find(':', pos);
        if (colon == std::string::npos) throw HostError{"malformed argument list"};
        size_t len = static_cast<size_t>(std::strtoull(s.substr(pos, colon - pos).c_str(), nullptr, 10));
        const size_t bytes = utf8Span(s, colon + 1, len);
        out.push_back(s.substr(colon + 1, bytes));
        pos = colon + 1 + bytes;
    }
    return out;
}

Value convertArg(const Type& t, const std::string& text) {
    switch (t.kind) {
        case Type::Int: {
            char* end = nullptr;
            long long v = std::strtoll(text.c_str(), &end, 10);
            if (text.empty() || *end != '\0') throw HostError{"'" + text + "' is not an int"};
            return Value::ofInt(v);
        }
        case Type::Bool:
            if (text == "true") return Value::ofBool(true);
            if (text == "false") return Value::ofBool(false);
            throw HostError{"'" + text + "' is not a boolean"};
        case Type::Double: {
            char* end = nullptr;
            double v = std::strtod(text.c_str(), &end);
            if (text.empty() || *end != '\0') throw HostError{"'" + text + "' is not a double"};
            return Value::ofDouble(v);
        }
        case Type::String: return Value::ofString(text);
        default: throw HostError{"parameter type " + typeText(t) + " cannot be passed from outside the module"};
    }
}

}  // namespace

void registerProcessHosts(HostRegistry& r) {
    // ---- proc.*
    add(r, "proc.run", {Type::String, Type::String, Type::String, Type::String, Type::Int}, Type::Int, "process",
        [](const Args& a, HostContext&) {
            if (a[0].s.empty()) throw HostError{"the program name is empty"};
            return Value::ofInt(runCapture(a[0].s, splitLines(a[1].s), a[2].s, a[3].s, a[4].i));
        });
    add(r, "proc.stdout", {}, Type::String, "process", [](const Args&, HostContext&) { return Value::ofString(g_proc.out); });
    add(r, "proc.stderr", {}, Type::String, "process", [](const Args&, HostContext&) { return Value::ofString(g_proc.err); });
    add(r, "proc.timedOut", {}, Type::Bool, "process", [](const Args&, HostContext&) { return Value::ofBool(g_proc.timedOut); });
    add(r, "proc.start", {Type::String, Type::String, Type::String}, Type::Int, "process", [](const Args& a, HostContext&) {
        if (a[0].s.empty()) throw HostError{"the program name is empty"};
        return Value::ofInt(startDetached(a[0].s, splitLines(a[1].s), a[2].s));
    });
    add(r, "proc.wait", {Type::Int, Type::Int}, Type::Int, "process", [](const Args& a, HostContext&) { return Value::ofInt(waitProcess(a[0].i, a[1].i)); });
    add(r, "proc.isRunning", {Type::Int}, Type::Bool, "process", [](const Args& a, HostContext&) { return Value::ofBool(isRunning(a[0].i)); });
    add(r, "proc.kill", {Type::Int}, Type::Bool, "process", [](const Args& a, HostContext&) {
        long long self;
#ifdef _WIN32
        self = static_cast<long long>(GetCurrentProcessId());
#else
        self = static_cast<long long>(getpid());
#endif
        if (a[0].i == self) throw HostError{"a program cannot kill itself with proc.kill"};
        return Value::ofBool(killProcess(a[0].i));
    });
    add(r, "proc.pid", {}, Type::Int, "process", [](const Args&, HostContext&) {
#ifdef _WIN32
        return Value::ofInt(static_cast<long long>(GetCurrentProcessId()));
#else
        return Value::ofInt(static_cast<long long>(getpid()));
#endif
    });
    add(r, "proc.which", {Type::String}, Type::String, "process", [](const Args& a, HostContext&) {
        const std::string& name = a[0].s;
        if (name.empty()) return Value::ofString("");
        const char* path = std::getenv("PATH");
        std::string p = path ? path : "";
#ifdef _WIN32
        const char sep = ';';
        std::vector<std::string> exts = {"", ".exe", ".cmd", ".bat", ".com"};
#else
        const char sep = ':';
        std::vector<std::string> exts = {""};
#endif
        if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
            for (const std::string& e : exts) { std::ifstream f(name + e); if (f) return Value::ofString(name + e); }
            return Value::ofString("");
        }
        size_t pos = 0;
        while (pos <= p.size()) {
            size_t e = p.find(sep, pos);
            std::string dir = p.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
            if (!dir.empty()) {
                for (const std::string& x : exts) {
                    std::string full = dir + "/" + name + x;
#ifdef _WIN32
                    std::ifstream f(full, std::ios::binary);
                    if (f) return Value::ofString(full);
#else
                    if (access(full.c_str(), X_OK) == 0) {
                        struct stat st;
                        if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return Value::ofString(full);
                    }
#endif
                }
            }
            if (e == std::string::npos) break;
            pos = e + 1;
        }
        return Value::ofString("");
    });

    // ---- rt.*
    add(r, "rt.version", {}, Type::String, "system", [](const Args&, HostContext&) { return Value::ofString(COYOTE_VERSION); });
    add(r, "rt.arch", {}, Type::String, "system", [](const Args&, HostContext&) {
#if defined(__x86_64__) || defined(_M_X64)
        return Value::ofString("x64");
#elif defined(__aarch64__) || defined(_M_ARM64)
        return Value::ofString("arm64");
#elif defined(__i386__) || defined(_M_IX86)
        return Value::ofString("x86");
#else
        return Value::ofString("unknown");
#endif
    });
    add(r, "rt.cpuCount", {}, Type::Int, "system", [](const Args&, HostContext&) {
        return Value::ofInt(static_cast<long long>(std::max(1u, std::thread::hardware_concurrency())));
    });
    add(r, "rt.memoryBytes", {}, Type::Int, "system", [](const Args&, HostContext&) { return Value::ofInt(processMemory()); });
    add(r, "rt.machine", {}, Type::String, "system", [](const Args&, HostContext&) {
#ifdef _WIN32
        const char* n = std::getenv("COMPUTERNAME");
        return Value::ofString(n ? n : "");
#else
        char buf[256] = {0};
        if (gethostname(buf, sizeof buf - 1) != 0) buf[0] = '\0';
        return Value::ofString(buf);
#endif
    });
    add(r, "rt.user", {}, Type::String, "system", [](const Args&, HostContext&) {
        const char* n = std::getenv("USERNAME");
        if (!n) n = std::getenv("USER");
        return Value::ofString(n ? n : "");
    });
    add(r, "rt.gcCollections", {}, Type::Int, "system", [](const Args&, HostContext& c) { return Value::ofInt(c.heap ? static_cast<long long>(c.heap->collections()) : 0); });
    add(r, "rt.gcFreed", {}, Type::Int, "system", [](const Args&, HostContext& c) { return Value::ofInt(c.heap ? static_cast<long long>(c.heap->freedObjects()) : 0); });
    add(r, "rt.heapObjects", {}, Type::Int, "system", [](const Args&, HostContext& c) { return Value::ofInt(c.heap ? static_cast<long long>(c.heap->liveObjects()) : 0); });
    add(r, "rt.heapElements", {}, Type::Int, "system", [](const Args&, HostContext& c) { return Value::ofInt(c.heap ? static_cast<long long>(c.heap->liveElements()) : 0); });

    // ---- coy.*
    add(r, "coy.load", {Type::String}, Type::Int, "interop", [](const Args& a, HostContext& c) {
        std::ifstream in(a[0].s, std::ios::binary);
        if (!in) throw HostError{"cannot read '" + a[0].s + "'"};
        std::ostringstream ss;
        ss << in.rdbuf();
        if (ss.str().size() > 32u * 1024u * 1024u) throw HostError{"'" + a[0].s + "' is too large"};
        auto l = std::make_unique<Loaded>();
        l->path = a[0].s;
        try {
            l->module = loadMl(ss.str(), a[0].s);
            verify(l->module, *c.registry, a[0].s);
        } catch (const MlError& e) {
            throw HostError{formatMlError(e)};
        }
        l->heap = std::make_unique<Heap>();
        g_loaded.push_back(std::move(l));
        return Value::ofInt(static_cast<long long>(g_loaded.size() - 1));
    });
    add(r, "coy.unload", {Type::Int}, Type::Void, "interop", [](const Args& a, HostContext&) {
        loadedAt(a[0].i);
        g_loaded[static_cast<size_t>(a[0].i)].reset();
        return Value::none();
    });
    add(r, "coy.run", {Type::Int}, Type::Int, "interop", [](const Args& a, HostContext& c) {
        Loaded& l = loadedAt(a[0].i);
        std::ostringstream out;
        g_coyErr.clear();
        int code = 0;
        try {
            run(l.module, *c.registry, out, *l.heap);
        } catch (const RuntimeError& e) {
            g_coyErr = formatRuntimeError(e);
            code = 1;
        } catch (const HostExit& e) {
            code = e.code;
        }
        g_coyOut = out.str();
        return Value::ofInt(code);
    });
    add(r, "coy.output", {}, Type::String, "interop", [](const Args&, HostContext&) { return Value::ofString(g_coyOut); });
    add(r, "coy.error", {}, Type::String, "interop", [](const Args&, HostContext&) { return Value::ofString(g_coyErr); });
    add(r, "coy.funcCount", {Type::Int}, Type::Int, "interop", [](const Args& a, HostContext&) {
        return Value::ofInt(static_cast<long long>(loadedAt(a[0].i).module.funcs.size()));
    });
    add(r, "coy.funcName", {Type::Int, Type::Int}, Type::String, "interop", [](const Args& a, HostContext&) {
        Loaded& l = loadedAt(a[0].i);
        if (a[1].i < 0 || static_cast<size_t>(a[1].i) >= l.module.funcs.size()) throw HostError{"function index out of range"};
        return Value::ofString(l.module.funcs[static_cast<size_t>(a[1].i)].name);
    });
    add(r, "coy.funcSig", {Type::Int, Type::Int}, Type::String, "interop", [](const Args& a, HostContext&) {
        Loaded& l = loadedAt(a[0].i);
        if (a[1].i < 0 || static_cast<size_t>(a[1].i) >= l.module.funcs.size()) throw HostError{"function index out of range"};
        const Sig& s = l.module.funcs[static_cast<size_t>(a[1].i)].sig;
        std::string t;
        for (size_t i = 0; i < s.params.size(); ++i) t += (i ? "," : "") + typeText(s.params[i]);
        return Value::ofString(t + ":" + typeText(s.ret));
    });
    add(r, "coy.call", {Type::Int, Type::String, Type::String}, Type::String, "interop", [](const Args& a, HostContext& c) {
        Loaded& l = loadedAt(a[0].i);
        int idx = -1;
        for (size_t i = 0; i < l.module.funcs.size(); ++i)
            if (l.module.funcs[i].name == a[1].s) { idx = static_cast<int>(i); break; }
        if (idx < 0) throw HostError{"the module has no function '" + a[1].s + "'"};
        const Function& fn = l.module.funcs[static_cast<size_t>(idx)];
        std::vector<std::string> items = decodeItems(a[2].s);
        if (items.size() != fn.sig.params.size())
            throw HostError{"'" + a[1].s + "' takes " + std::to_string(fn.sig.params.size()) + " argument(s), got " + std::to_string(items.size())};
        CallRequest req;
        req.func = idx;
        for (size_t i = 0; i < items.size(); ++i) req.args.push_back(convertArg(fn.sig.params[i], items[i]));
        std::ostringstream out;
        g_coyErr.clear();
        try {
            run(l.module, *c.registry, out, *l.heap, nullptr, &req);
        } catch (const RuntimeError& e) {
            g_coyOut = out.str();
            throw HostError{formatRuntimeError(e)};
        }
        g_coyOut = out.str();
        if (!req.hasResult) return Value::ofString("");
        const Value& v = req.result;
        switch (v.kind) {
            case Type::Int: return Value::ofString(std::to_string(v.i));
            case Type::Bool: return Value::ofString(v.i ? "true" : "false");
            case Type::String: return Value::ofString(v.s);
            case Type::Double: { std::ostringstream d; d.precision(17); d << v.d; return Value::ofString(d.str()); }
            default: throw HostError{"'" + a[1].s + "' returns " + typeText(fn.sig.ret) + ", which cannot be handed to the caller"};
        }
    });

    // ---- mouths (host variables, `mouth:sys name {get;}`), values are boxed to obj by the interpreter
    r.addMouth({"sys.exceptionObject", "", [](HostContext& c) { return c.lastPayload; }, nullptr});
    r.addMouth({"sys.stackTrace", "", [](HostContext& c) { return c.hasLastError ? Value::ofString(c.lastTrace) : Value::nullObj(); }, nullptr});
    r.addMouth({"sys.exception", "", [](HostContext& c) { return c.hasLastError ? Value::ofString(c.lastError) : Value::nullObj(); }, nullptr});
    r.addMouth({"sys.exitCode", "",
                [](HostContext& c) { return Value::ofInt(c.exitCode); },
                [](const Value& v, HostContext& c) {
                    if (!v.boxed || v.boxed->kind != Type::Int) throw HostError{"sys.exitCode expects an int"};
                    c.exitCode = v.boxed->i;
                    c.exitCodeSet = true;
                }});
    r.addMouth({"sys.os", "", [](HostContext&) {
#ifdef _WIN32
        return Value::ofString("windows");
#elif defined(__APPLE__)
        return Value::ofString("macos");
#else
        return Value::ofString("linux");
#endif
    }, nullptr});
    r.addMouth({"sys.version", "", [](HostContext&) { return Value::ofString(COYOTE_VERSION); }, nullptr});
    r.addMouth({"sys.pid", "system", [](HostContext&) {
#ifdef _WIN32
        return Value::ofInt(static_cast<long long>(GetCurrentProcessId()));
#else
        return Value::ofInt(static_cast<long long>(getpid()));
#endif
    }, nullptr});
}

}  // namespace pvm
