// The `win` host module (permission "windows"): message boxes, file/folder dialogs, the clipboard, the registry,
// processes, windows and notifications. Real on Windows; on every other system the functions exist (so programs
// load and verify) but a call is the runtime error "the win.* functions work only on Windows".
#include <algorithm>
#include <string>
#include <vector>

#include "host.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
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

void add(HostRegistry& r, const std::string& name, std::vector<Type> params, Type ret, std::function<Value(const Args&, HostContext&)> fn) {
    r.add(HostFunction{"win." + name, sig(std::move(params), ret), "windows", std::move(fn)});
}

#ifndef _WIN32
Value unsupported(const Args&, HostContext&) { throw HostError{"the win.* functions work only on Windows"}; }
#define IMPL(n) unsupported
#else
#define IMPL(n) w_##n

std::wstring W(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) throw HostError{"the text is not valid UTF-8"};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string U(const wchar_t* w, int len = -1) {
    if (!w) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, &s[0], n, nullptr, nullptr);
    if (len == -1 && !s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

std::string lastError(DWORD code = GetLastError()) {
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                                   reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string msg = n ? U(buf, static_cast<int>(n)) : "error " + std::to_string(code);
    if (buf) LocalFree(buf);
    while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n' || msg.back() == ' ' || msg.back() == '.')) msg.pop_back();
    return msg;
}

void flushOut(HostContext& c) { c.out.flush(); }

// ---------- message box ----------

Value w_messageBox(const Args& a, HostContext& c) {
    const long long buttons = a[2].i, icon = a[3].i;
    if (buttons < 0 || buttons > 6) throw HostError{"MessageBox buttons must be 0..6 (OK, OKCancel, AbortRetryIgnore, YesNoCancel, YesNo, RetryCancel, CancelTryContinue)"};
    if (icon != 0 && icon != 16 && icon != 32 && icon != 48 && icon != 64) throw HostError{"MessageBox icon must be 0 (none), 16 (error), 32 (question), 48 (warning) or 64 (information)"};
    flushOut(c);
    const int r = MessageBoxW(nullptr, W(a[0].s).c_str(), W(a[1].s).c_str(), static_cast<UINT>(buttons | icon) | MB_SETFOREGROUND);
    if (r == 0) throw HostError{"MessageBox failed: " + lastError()};
    return Value::ofInt(r);
}

// ---------- dialogs ----------

// "Text files|*.txt|All files|*.*"  ->  "Text files\0*.txt\0All files\0*.*\0\0"
std::wstring dialogFilter(const std::string& filter) {
    std::string f = filter.empty() ? "All files|*.*" : filter;
    std::wstring w = W(f);
    for (wchar_t& ch : w) if (ch == L'|') ch = L'\0';
    w.push_back(L'\0');
    w.push_back(L'\0');
    return w;
}

Value fileDialog(const Args& a, HostContext& c, bool save) {
    flushOut(c);
    std::wstring title = W(a[0].s), filter = dialogFilter(a[1].s), dir = W(a[2].s);
    std::vector<wchar_t> file(32768, L'\0');
    if (save && a.size() > 3) {
        std::wstring def = W(a[3].s);
        if (def.size() < file.size() - 1) std::copy(def.begin(), def.end(), file.begin());
    }
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = static_cast<DWORD>(file.size());
    ofn.lpstrTitle = title.empty() ? nullptr : title.c_str();
    ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    ofn.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    const BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok) {
        const DWORD e = CommDlgExtendedError();
        if (e != 0) throw HostError{"the file dialog failed (error " + std::to_string(e) + ")"};
        return Value::ofString("");  // cancelled
    }
    return Value::ofString(U(file.data()));
}

Value w_openFileDialog(const Args& a, HostContext& c) { return fileDialog(a, c, false); }
Value w_saveFileDialog(const Args& a, HostContext& c) { return fileDialog(a, c, true); }

Value w_folderDialog(const Args& a, HostContext& c) {
    flushOut(c);
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring title = W(a[0].s);
    wchar_t name[MAX_PATH] = {0};
    BROWSEINFOW bi{};
    bi.lpszTitle = title.c_str();
    bi.pszDisplayName = name;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    std::string result;
    if (pidl) {
        wchar_t path[MAX_PATH] = {0};
        if (SHGetPathFromIDListW(pidl, path)) result = U(path);
        CoTaskMemFree(pidl);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return Value::ofString(result);
}

// ---------- clipboard ----------

struct Clipboard {
    bool opened = false;
    Clipboard() {
        for (int i = 0; i < 20 && !opened; ++i) {  // another program may hold it for a moment
            opened = OpenClipboard(nullptr) != 0;
            if (!opened) Sleep(10);
        }
        if (!opened) throw HostError{"cannot open the clipboard: " + lastError()};
    }
    ~Clipboard() { if (opened) CloseClipboard(); }
};

Value w_clipboardGetText(const Args&, HostContext&) {
    Clipboard cb;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return Value::ofString("");
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (!h) return Value::ofString("");
    const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h));
    if (!p) return Value::ofString("");
    std::string s = U(p);
    GlobalUnlock(h);
    return Value::ofString(s);
}

Value w_clipboardSetText(const Args& a, HostContext&) {
    std::wstring w = W(a[0].s);
    Clipboard cb;
    if (!EmptyClipboard()) throw HostError{"cannot clear the clipboard: " + lastError()};
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
    if (!mem) throw HostError{"out of memory for the clipboard text"};
    wchar_t* p = static_cast<wchar_t*>(GlobalLock(mem));
    if (!p) { GlobalFree(mem); throw HostError{"cannot lock the clipboard memory"}; }
    std::copy(w.begin(), w.end(), p);
    p[w.size()] = L'\0';
    GlobalUnlock(mem);
    if (!SetClipboardData(CF_UNICODETEXT, mem)) {
        const std::string e = lastError();
        GlobalFree(mem);
        throw HostError{"cannot set the clipboard text: " + e};
    }
    return Value::none();  // the clipboard owns the memory now
}

Value w_clipboardClear(const Args&, HostContext&) {
    Clipboard cb;
    if (!EmptyClipboard()) throw HostError{"cannot clear the clipboard: " + lastError()};
    return Value::none();
}

Value w_clipboardHasText(const Args&, HostContext&) { return Value::ofBool(IsClipboardFormatAvailable(CF_UNICODETEXT) != 0); }

// ---------- registry ----------

struct RegPath {
    HKEY root = nullptr;
    std::wstring sub;
    std::string rootName;
};

RegPath parseKey(const std::string& key, bool needSub) {
    std::string k = key;
    for (char& ch : k) if (ch == '/') ch = '\\';
    const size_t slash = k.find('\\');
    std::string root = k.substr(0, slash);
    std::string rest = slash == std::string::npos ? "" : k.substr(slash + 1);
    for (char& ch : root) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 32);
    RegPath p;
    if (root == "HKCU" || root == "HKEY_CURRENT_USER") p.root = HKEY_CURRENT_USER;
    else if (root == "HKLM" || root == "HKEY_LOCAL_MACHINE") p.root = HKEY_LOCAL_MACHINE;
    else if (root == "HKCR" || root == "HKEY_CLASSES_ROOT") p.root = HKEY_CLASSES_ROOT;
    else if (root == "HKU" || root == "HKEY_USERS") p.root = HKEY_USERS;
    else if (root == "HKCC" || root == "HKEY_CURRENT_CONFIG") p.root = HKEY_CURRENT_CONFIG;
    else throw HostError{"unknown registry root '" + root + "' (use HKCU, HKLM, HKCR, HKU or HKCC, e.g. HKCU\\Software\\MyApp)"};
    p.rootName = root;
    while (!rest.empty() && rest.back() == '\\') rest.pop_back();
    if (needSub && rest.empty()) throw HostError{"the registry key needs a path below " + root};
    p.sub = W(rest);
    return p;
}

[[noreturn]] void regFail(const std::string& what, LONG code) { throw HostError{what + ": " + lastError(static_cast<DWORD>(code))}; }

struct Key {
    HKEY h = nullptr;
    ~Key() { if (h) RegCloseKey(h); }
};

Key openKey(const RegPath& p, REGSAM access, const std::string& what) {
    Key k;
    const LONG r = RegOpenKeyExW(p.root, p.sub.c_str(), 0, access, &k.h);
    if (r != ERROR_SUCCESS) regFail(what, r);
    return k;
}

Value w_regRead(const Args& a, HostContext&) {
    RegPath p = parseKey(a[0].s, false);
    Key k = openKey(p, KEY_QUERY_VALUE, "cannot open '" + a[0].s + "'");
    const std::wstring name = W(a[1].s);
    DWORD type = 0, size = 0;
    LONG r = RegQueryValueExW(k.h, name.c_str(), nullptr, &type, nullptr, &size);
    if (r != ERROR_SUCCESS) regFail("cannot read value '" + a[1].s + "' of '" + a[0].s + "'", r);
    std::vector<unsigned char> buf(size + 4, 0);
    DWORD got = size;
    r = RegQueryValueExW(k.h, name.c_str(), nullptr, &type, buf.data(), &got);
    if (r != ERROR_SUCCESS) regFail("cannot read value '" + a[1].s + "' of '" + a[0].s + "'", r);
    if (type == REG_SZ || type == REG_EXPAND_SZ) return Value::ofString(U(reinterpret_cast<const wchar_t*>(buf.data())));
    if (type == REG_DWORD && got >= 4) return Value::ofString(std::to_string(*reinterpret_cast<const DWORD*>(buf.data())));
    if (type == REG_QWORD && got >= 8) return Value::ofString(std::to_string(*reinterpret_cast<const long long*>(buf.data())));
    throw HostError{"value '" + a[1].s + "' has a type other than text or number (REG_SZ, REG_EXPAND_SZ, REG_DWORD, REG_QWORD)"};
}

Value w_regExists(const Args& a, HostContext&) {
    RegPath p = parseKey(a[0].s, false);
    Key k;
    if (RegOpenKeyExW(p.root, p.sub.c_str(), 0, KEY_QUERY_VALUE, &k.h) != ERROR_SUCCESS) return Value::ofBool(false);
    if (a[1].s.empty()) return Value::ofBool(true);
    return Value::ofBool(RegQueryValueExW(k.h, W(a[1].s).c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS);
}

Value regWriteRaw(const Args& a, DWORD type, const void* data, DWORD size) {
    RegPath p = parseKey(a[0].s, true);
    Key k;
    const LONG r = RegCreateKeyExW(p.root, p.sub.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k.h, nullptr);
    if (r != ERROR_SUCCESS) regFail("cannot open '" + a[0].s + "' for writing", r);
    const LONG w = RegSetValueExW(k.h, W(a[1].s).c_str(), 0, type, static_cast<const BYTE*>(data), size);
    if (w != ERROR_SUCCESS) regFail("cannot write value '" + a[1].s + "'", w);
    return Value::none();
}

Value w_regWrite(const Args& a, HostContext&) {
    std::wstring v = W(a[2].s);
    return regWriteRaw(a, REG_SZ, v.c_str(), static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
}

Value w_regWriteInt(const Args& a, HostContext&) {
    if (a[2].i >= 0 && a[2].i <= 0xFFFFFFFFLL) {
        const DWORD d = static_cast<DWORD>(a[2].i);
        return regWriteRaw(a, REG_DWORD, &d, sizeof d);
    }
    const long long q = a[2].i;
    return regWriteRaw(a, REG_QWORD, &q, sizeof q);
}

Value w_regDeleteValue(const Args& a, HostContext&) {
    RegPath p = parseKey(a[0].s, false);
    Key k = openKey(p, KEY_SET_VALUE, "cannot open '" + a[0].s + "'");
    const LONG r = RegDeleteValueW(k.h, W(a[1].s).c_str());
    if (r != ERROR_SUCCESS) regFail("cannot delete value '" + a[1].s + "'", r);
    return Value::none();
}

Value w_regDeleteKey(const Args& a, HostContext&) {
    RegPath p = parseKey(a[0].s, true);
    if (std::count(p.sub.begin(), p.sub.end(), L'\\') < 1)  // HKCU\Software itself is far too much to delete
        throw HostError{"refusing to delete '" + a[0].s + "': a registry key to delete needs at least two path parts (e.g. HKCU\\Software\\MyApp)"};
    const LONG r = RegDeleteTreeW(p.root, p.sub.c_str());
    if (r != ERROR_SUCCESS) regFail("cannot delete '" + a[0].s + "'", r);
    return Value::none();
}

Value w_regSubKeys(const Args& a, HostContext&) {
    RegPath p = parseKey(a[0].s, false);
    Key k = openKey(p, KEY_ENUMERATE_SUB_KEYS, "cannot open '" + a[0].s + "'");
    std::string out;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD n = 256;
        const LONG r = RegEnumKeyExW(k.h, i, name, &n, nullptr, nullptr, nullptr, nullptr);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) regFail("cannot list '" + a[0].s + "'", r);
        out += (i ? "\n" : "") + U(name, static_cast<int>(n));
    }
    return Value::ofString(out);
}

Value w_regValues(const Args& a, HostContext&) {
    RegPath p = parseKey(a[0].s, false);
    Key k = openKey(p, KEY_QUERY_VALUE, "cannot open '" + a[0].s + "'");
    std::string out;
    for (DWORD i = 0;; ++i) {
        wchar_t name[16384];
        DWORD n = 16384;
        const LONG r = RegEnumValueW(k.h, i, name, &n, nullptr, nullptr, nullptr, nullptr);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) regFail("cannot list '" + a[0].s + "'", r);
        out += (i ? "\n" : "") + U(name, static_cast<int>(n));
    }
    return Value::ofString(out);
}

// ---------- processes ----------

Value w_processList(const Args&, HostContext&) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) throw HostError{"cannot list the processes: " + lastError()};
    std::string out;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof e;
    bool first = true;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        out += (first ? "" : "\n") + std::to_string(e.th32ProcessID) + "\t" + U(e.szExeFile);
        first = false;
    }
    CloseHandle(snap);
    return Value::ofString(out);
}

std::wstring quoteArg(const std::wstring& s) {
    if (!s.empty() && s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    return L"\"" + s + L"\"";
}

bool launch(const std::string& command, const std::string& args, PROCESS_INFORMATION& pi) {
    if (command.empty()) throw HostError{"the program to start is empty"};
    std::wstring line = quoteArg(W(command));
    if (!args.empty()) line += L" " + W(args);
    std::vector<wchar_t> buf(line.begin(), line.end());
    buf.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof si;
    pi = PROCESS_INFORMATION{};
    return CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi) != 0;
}

Value w_processStart(const Args& a, HostContext& c) {
    flushOut(c);
    PROCESS_INFORMATION pi;
    if (!launch(a[0].s, a[1].s, pi)) throw HostError{"cannot start '" + a[0].s + "': " + lastError()};
    const DWORD pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return Value::ofInt(pid);
}

Value w_processRun(const Args& a, HostContext& c) {
    flushOut(c);
    if (a[2].i < 0 || a[2].i > 86400000LL) throw HostError{"the timeout must be 0..86400000 milliseconds (0 waits for ever)"};
    PROCESS_INFORMATION pi;
    if (!launch(a[0].s, a[1].s, pi)) throw HostError{"cannot start '" + a[0].s + "': " + lastError()};
    CloseHandle(pi.hThread);
    const DWORD w = WaitForSingleObject(pi.hProcess, a[2].i == 0 ? INFINITE : static_cast<DWORD>(a[2].i));
    if (w == WAIT_TIMEOUT) {
        CloseHandle(pi.hProcess);
        throw HostError{"'" + a[0].s + "' did not finish within " + std::to_string(a[2].i) + " ms (it is still running)"};
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return Value::ofInt(static_cast<long long>(static_cast<int>(code)));
}

Value w_processIsRunning(const Args& a, HostContext&) {
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(a[0].i));
    if (!h) return Value::ofBool(false);
    const bool running = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return Value::ofBool(running);
}

Value w_processKill(const Args& a, HostContext&) {
    if (a[0].i <= 0 || a[0].i > 0xFFFFFFFFLL) throw HostError{"invalid process id " + std::to_string(a[0].i)};
    if (static_cast<DWORD>(a[0].i) == GetCurrentProcessId()) throw HostError{"a program cannot kill itself with win.processKill (use Environment.Exit)"};
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(a[0].i));
    if (!h) throw HostError{"cannot open process " + std::to_string(a[0].i) + ": " + lastError()};
    const BOOL ok = TerminateProcess(h, 1);
    const std::string e = ok ? "" : lastError();
    CloseHandle(h);
    if (!ok) throw HostError{"cannot end process " + std::to_string(a[0].i) + ": " + e};
    return Value::ofBool(true);
}

Value w_processId(const Args&, HostContext&) { return Value::ofInt(GetCurrentProcessId()); }

Value w_shellOpen(const Args& a, HostContext&) {
    if (a[0].s.empty()) throw HostError{"nothing to open"};
    const INT_PTR r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", W(a[0].s).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return Value::ofBool(r > 32);
}

// ---------- windows ----------

HWND windowOf(long long v) {
    HWND h = reinterpret_cast<HWND>(static_cast<intptr_t>(v));
    if (!IsWindow(h)) throw HostError{"there is no window with the handle " + std::to_string(v)};
    return h;
}

std::string titleOf(HWND h) {
    const int n = GetWindowTextLengthW(h);
    if (n <= 0) return std::string();
    std::vector<wchar_t> buf(static_cast<size_t>(n) + 1, L'\0');
    const int got = GetWindowTextW(h, buf.data(), n + 1);
    return U(buf.data(), got);
}

BOOL CALLBACK collectWindow(HWND h, LPARAM lp) {
    if (!IsWindowVisible(h)) return TRUE;
    const std::string t = titleOf(h);
    if (t.empty()) return TRUE;
    std::string* out = reinterpret_cast<std::string*>(lp);
    if (!out->empty()) *out += "\n";
    *out += std::to_string(static_cast<long long>(reinterpret_cast<intptr_t>(h))) + "\t" + t;
    return TRUE;
}

Value w_windowList(const Args&, HostContext&) {
    std::string out;
    EnumWindows(collectWindow, reinterpret_cast<LPARAM>(&out));
    return Value::ofString(out);
}

Value w_windowFind(const Args& a, HostContext&) {
    HWND h = FindWindowW(nullptr, W(a[0].s).c_str());
    return Value::ofInt(static_cast<long long>(reinterpret_cast<intptr_t>(h)));
}

Value w_windowTitle(const Args& a, HostContext&) { return Value::ofString(titleOf(windowOf(a[0].i))); }

Value w_windowSetTitle(const Args& a, HostContext&) {
    if (!SetWindowTextW(windowOf(a[0].i), W(a[1].s).c_str())) throw HostError{"cannot change the title: " + lastError()};
    return Value::none();
}

Value w_windowShow(const Args& a, HostContext&) {
    const long long cmd = a[1].i;
    if (cmd != 0 && cmd != 1 && cmd != 2 && cmd != 3 && cmd != 5 && cmd != 6 && cmd != 9)
        throw HostError{"window command must be 0 (hide), 1 (show), 2 (minimize), 3 (maximize), 5 (show), 6 (minimize) or 9 (restore)"};
    ShowWindow(windowOf(a[0].i), static_cast<int>(cmd));
    return Value::none();
}

Value w_windowClose(const Args& a, HostContext&) {
    if (!PostMessageW(windowOf(a[0].i), WM_CLOSE, 0, 0)) throw HostError{"cannot close the window: " + lastError()};
    return Value::none();
}

Value w_windowFocus(const Args& a, HostContext&) {
    HWND h = windowOf(a[0].i);
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    return Value::ofBool(SetForegroundWindow(h) != 0);
}

Value w_windowRect(const Args& a, HostContext&) {
    RECT r{};
    if (!GetWindowRect(windowOf(a[0].i), &r)) throw HostError{"cannot read the window position: " + lastError()};
    return Value::ofString(std::to_string(r.left) + " " + std::to_string(r.top) + " " + std::to_string(r.right - r.left) + " " + std::to_string(r.bottom - r.top));
}

Value w_windowMove(const Args& a, HostContext&) {
    for (int i = 1; i <= 4; ++i)
        if (a[static_cast<size_t>(i)].i < -32000 || a[static_cast<size_t>(i)].i > 32000) throw HostError{"window position and size must be between -32000 and 32000"};
    if (a[3].i < 1 || a[4].i < 1) throw HostError{"the window width and height must be at least 1"};
    if (!SetWindowPos(windowOf(a[0].i), nullptr, static_cast<int>(a[1].i), static_cast<int>(a[2].i), static_cast<int>(a[3].i), static_cast<int>(a[4].i), SWP_NOZORDER | SWP_NOACTIVATE))
        throw HostError{"cannot move the window: " + lastError()};
    return Value::none();
}

// ---------- notifications ----------

LRESULT CALLBACK notifyProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

template <size_t N>
void copyText(wchar_t (&dst)[N], const std::wstring& src) {
    const size_t n = std::min(src.size(), N - 1);
    std::copy(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(n), dst);
    dst[n] = L'\0';
}

// Shows a balloon/toast notification for `millis` milliseconds (the notification area icon is removed afterwards).
Value w_notify(const Args& a, HostContext& c) {
    flushOut(c);
    if (a[2].i < 500 || a[2].i > 30000) throw HostError{"the notification time must be 500..30000 milliseconds"};
    static const wchar_t* kClass = L"CoyoteNotifyWindow";
    WNDCLASSW wc{};
    wc.lpfnWndProc = notifyProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassW(&wc);  // fails harmlessly when it is already registered
    HWND hwnd = CreateWindowExW(0, kClass, L"Coyote", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!hwnd) throw HostError{"cannot create the notification window: " + lastError()};
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_INFO;
    nid.hIcon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_INFORMATION));
    copyText(nid.szTip, L"Coyote");
    copyText(nid.szInfoTitle, W(a[0].s));
    copyText(nid.szInfo, W(a[1].s));
    nid.dwInfoFlags = NIIF_INFO;
    const BOOL added = Shell_NotifyIconW(NIM_ADD, &nid);
    if (!added) {
        const std::string e = lastError();
        DestroyWindow(hwnd);
        throw HostError{"cannot show the notification: " + e};
    }
    const DWORD end = GetTickCount() + static_cast<DWORD>(a[2].i);
    MSG msg;
    while (static_cast<long long>(end - GetTickCount()) > 0) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(20);
    }
    Shell_NotifyIconW(NIM_DELETE, &nid);
    DestroyWindow(hwnd);
    return Value::none();
}
#endif  // _WIN32

}  // namespace

void registerWindowsHosts(HostRegistry& r) {
    const Type S = Type::String, I = Type::Int, B = Type::Bool, V = Type::Void;
    // dialogs
    add(r, "messageBox", {S, S, I, I}, I, IMPL(messageBox));
    add(r, "openFileDialog", {S, S, S}, S, IMPL(openFileDialog));
    add(r, "saveFileDialog", {S, S, S, S}, S, IMPL(saveFileDialog));
    add(r, "folderDialog", {S}, S, IMPL(folderDialog));
    // clipboard
    add(r, "clipboardGetText", {}, S, IMPL(clipboardGetText));
    add(r, "clipboardSetText", {S}, V, IMPL(clipboardSetText));
    add(r, "clipboardClear", {}, V, IMPL(clipboardClear));
    add(r, "clipboardHasText", {}, B, IMPL(clipboardHasText));
    // registry
    add(r, "regRead", {S, S}, S, IMPL(regRead));
    add(r, "regExists", {S, S}, B, IMPL(regExists));
    add(r, "regWrite", {S, S, S}, V, IMPL(regWrite));
    add(r, "regWriteInt", {S, S, I}, V, IMPL(regWriteInt));
    add(r, "regDeleteValue", {S, S}, V, IMPL(regDeleteValue));
    add(r, "regDeleteKey", {S}, V, IMPL(regDeleteKey));
    add(r, "regSubKeys", {S}, S, IMPL(regSubKeys));
    add(r, "regValues", {S}, S, IMPL(regValues));
    // processes
    add(r, "processList", {}, S, IMPL(processList));
    add(r, "processStart", {S, S}, I, IMPL(processStart));
    add(r, "processRun", {S, S, I}, I, IMPL(processRun));
    add(r, "processIsRunning", {I}, B, IMPL(processIsRunning));
    add(r, "processKill", {I}, B, IMPL(processKill));
    add(r, "processId", {}, I, IMPL(processId));
    add(r, "shellOpen", {S}, B, IMPL(shellOpen));
    // windows
    add(r, "windowList", {}, S, IMPL(windowList));
    add(r, "windowFind", {S}, I, IMPL(windowFind));
    add(r, "windowTitle", {I}, S, IMPL(windowTitle));
    add(r, "windowSetTitle", {I, S}, V, IMPL(windowSetTitle));
    add(r, "windowShow", {I, I}, V, IMPL(windowShow));
    add(r, "windowClose", {I}, V, IMPL(windowClose));
    add(r, "windowFocus", {I}, B, IMPL(windowFocus));
    add(r, "windowRect", {I}, S, IMPL(windowRect));
    add(r, "windowMove", {I, I, I, I, I}, V, IMPL(windowMove));
    // notifications
    add(r, "notify", {S, S, I}, V, IMPL(notify));
}

}  // namespace pvm
