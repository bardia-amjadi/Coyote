#include "process.hpp"

#include <cerrno>
#include <cstdio>
#include <iostream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char** environ;
#endif

namespace coyote::pkg {

namespace {

bool hasSeparator(const std::string& s) { return s.find('/') != std::string::npos || s.find('\\') != std::string::npos; }

#ifdef _WIN32

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

// Quotes one argument following the CommandLineToArgvW rules.
std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'"') { r.append(backslashes * 2 + 1, L'\\'); r += L'"'; }
        else { r.append(backslashes, L'\\'); r += c; }
        backslashes = 0;
    }
    r.append(backslashes * 2, L'\\');
    return r + L"\"";
}

bool fileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

#else

bool isExecutable(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(p.c_str(), X_OK) == 0;
}

#endif

} // namespace

std::string executableDir(const char* argv0) {
#ifdef _WIN32
    (void)argv0;
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "";
    std::wstring w(buf, n);
    size_t slash = w.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return "";
    std::wstring dir = w.substr(0, slash);
    int len = WideCharToMultiByte(CP_UTF8, 0, dir.data(), static_cast<int>(dir.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, dir.data(), static_cast<int>(dir.size()), &out[0], len, nullptr, nullptr);
    return out;
#else
    std::string path;
#ifdef __APPLE__
    char buf[PATH_MAX];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) {
        char real[PATH_MAX];
        path = ::realpath(buf, real) ? real : buf;
    }
#else
    char buf[PATH_MAX];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) path.assign(buf, static_cast<size_t>(n));
#endif
    if (path.empty() && argv0 && hasSeparator(argv0)) path = argv0;
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? "" : path.substr(0, slash);
#endif
}

namespace {

ProcessResult runImpl(const std::vector<std::string>& argv, const std::string& siblingDir, std::string* capture) {
    ProcessResult res;
    if (argv.empty()) { res.error = "no program to run"; return res; }
    std::cout.flush();
    std::cerr.flush();

#ifdef _WIN32
    std::wstring program = widen(argv[0]);
    if (!hasSeparator(argv[0]) && !siblingDir.empty()) {
        std::wstring cand = widen(siblingDir) + L"\\" + widen(argv[0]);
        if (cand.size() < 4 || cand.compare(cand.size() - 4, 4, L".exe") != 0) cand += L".exe";
        if (fileExists(cand)) program = cand;
    }
    std::wstring cmd = quoteArg(program);
    for (size_t i = 1; i < argv.size(); ++i) cmd += L" " + quoteArg(widen(argv[i]));
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (capture) {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) { res.error = "cannot create a pipe"; return res; }
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = writeEnd;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    // lpApplicationName is null so a bare name is searched on PATH (".exe" is appended by Windows).
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        res.error = "cannot run '" + argv[0] + "' (Windows error " + std::to_string(GetLastError()) + ")";
        if (capture) { CloseHandle(readEnd); CloseHandle(writeEnd); }
        return res;
    }
    res.started = true;
    if (capture) {
        CloseHandle(writeEnd);
        char tmp[4096];
        DWORD got;
        while (ReadFile(readEnd, tmp, sizeof tmp, &got, nullptr) && got > 0) capture->append(tmp, got);
        CloseHandle(readEnd);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    res.exitCode = static_cast<int>(code);
    return res;
#else
    std::string program = argv[0];
    bool resolved = false;
    if (!hasSeparator(program) && !siblingDir.empty()) {
        std::string cand = siblingDir + "/" + program;
        if (isExecutable(cand)) { program = cand; resolved = true; }
    }
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    int fds[2] = {-1, -1};
    if (capture) {
        if (::pipe(fds) != 0) { posix_spawn_file_actions_destroy(&actions); res.error = "cannot create a pipe"; return res; }
        posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
        posix_spawn_file_actions_addclose(&actions, fds[0]);
        posix_spawn_file_actions_addclose(&actions, fds[1]);
    }
    pid_t pid;
    int rc = resolved || hasSeparator(program) ? ::posix_spawn(&pid, program.c_str(), &actions, nullptr, args.data(), environ)
                                                : ::posix_spawnp(&pid, program.c_str(), &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (capture) ::close(fds[1]);
    if (rc != 0) {
        if (capture) ::close(fds[0]);
        res.error = rc == ENOENT ? "cannot run '" + argv[0] + "': program not found"
                                 : "cannot run '" + argv[0] + "' (error " + std::to_string(rc) + ")";
        return res;
    }
    res.started = true;
    if (capture) {
        char tmp[4096];
        for (;;) {
            ssize_t n = ::read(fds[0], tmp, sizeof tmp);
            if (n > 0) capture->append(tmp, static_cast<size_t>(n));
            else if (n < 0 && errno == EINTR) continue;
            else break;
        }
        ::close(fds[0]);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) { res.started = false; res.error = "waiting for '" + argv[0] + "' failed"; return res; }
    }
    if (WIFEXITED(status)) res.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) res.exitCode = 128 + WTERMSIG(status);
    return res;
#endif
}

} // namespace

ProcessResult runProcess(const std::vector<std::string>& argv, const std::string& siblingDir) {
    return runImpl(argv, siblingDir, nullptr);
}

ProcessResult runProcessCapture(const std::vector<std::string>& argv, const std::string& siblingDir, std::string& out) {
    return runImpl(argv, siblingDir, &out);
}

} // namespace coyote::pkg
