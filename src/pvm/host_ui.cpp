// The `ui` host module (permission "ui"): windows and controls for the Vision UI framework (library VisionUI).
//
// Two backends share one model (windows, controls, an event queue):
//   * Win32     - real top-level windows with native controls (STATIC, BUTTON, EDIT). Used on Windows unless the
//                 environment variable COYOTE_UI=headless is set.
//   * headless  - the same model without any screen. Used on every other system (and on Windows with COYOTE_UI=headless).
//                 Nothing is drawn; programs and tests drive it with ui.simClick / ui.simSetText / ui.simClose, and
//                 ui.nextEvent returns "quit" as soon as no event is pending, so a program ends by itself.
//
// Events are plain strings, fields separated by TAB:  click<TAB>control   change<TAB>control   close<TAB>window
//                                                    resize<TAB>window<TAB>width<TAB>height    quit
// All sizes in this API are device-independent pixels (96 dpi); the Win32 backend scales them to the real DPI.
#include <algorithm>
#include <cstdlib>
#include <deque>
#include <map>
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
    r.add(HostFunction{"ui." + name, sig(std::move(params), ret), "ui", std::move(fn)});
}

struct Ctl {
    int id = 0;
    int win = 0;
    std::string kind;  // label button textbox checkbox
    std::string text;
    bool visible = true, enabled = true, checked = false;
    int x = 0, y = 0, w = 0, h = 0;
#ifdef _WIN32
    HWND hwnd = nullptr;
#endif
};

struct Win {
    int id = 0;
    std::string title;
    int w = 0, h = 0;      // client size in device-independent pixels
    bool shown = false;
    int bg = -1, fg = -1, accent = -1;  // 0xRRGGBB or -1 = system default
    std::string font;
    int fontSize = 0;
#ifdef _WIN32
    HWND hwnd = nullptr;
    HFONT hfont = nullptr;
    HBRUSH bgBrush = nullptr;
    bool ownsFont = false;
#endif
};

std::map<int, Win> g_wins;
std::map<int, Ctl> g_ctls;
std::deque<std::string> g_events;
int g_nextWin = 1;
int g_nextCtl = 1000;  // control ids start high so they never clash with IDOK, IDCANCEL ...
[[maybe_unused]] bool g_suppress = false;  // true while the host itself changes a control (no "change" event)

bool wantWin32() {
#ifdef _WIN32
    const char* e = std::getenv("COYOTE_UI");
    return !(e && std::string(e) == "headless");
#else
    return false;
#endif
}

Win& win(int id) {
    auto it = g_wins.find(id);
    if (it == g_wins.end()) throw HostError{"ui: there is no window with the handle " + std::to_string(id)};
    return it->second;
}

Ctl& ctl(int id) {
    auto it = g_ctls.find(id);
    if (it == g_ctls.end()) throw HostError{"ui: there is no control with the handle " + std::to_string(id)};
    return it->second;
}

void pushEvent(const std::string& e) { g_events.push_back(e); }

#ifdef _WIN32

std::wstring W(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string U8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

COLORREF rgb(int c) { return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF); }

const wchar_t* kClass = L"CoyoteVisionWindow";
bool g_classReady = false;
double g_scale = 1.0;

int px(int v) { return static_cast<int>(v * g_scale + (v >= 0 ? 0.5 : -0.5)); }
int dp(int v) { return static_cast<int>(v / g_scale + (v >= 0 ? 0.5 : -0.5)); }

int winIdOf(HWND h) { return static_cast<int>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

std::wstring windowText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring w(static_cast<size_t>(n) + 1, L'\0');
    int got = GetWindowTextW(h, &w[0], n + 1);
    w.resize(static_cast<size_t>(got < 0 ? 0 : got));
    return w;
}

HFONT makeFont(const Win& w) {
    if (!w.font.empty() && w.fontSize > 0) {
        HDC dc = GetDC(nullptr);
        int dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(nullptr, dc);
        return CreateFontW(-MulDiv(w.fontSize, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, W(w.font).c_str());
    }
    NONCLIENTMETRICSW ncm;
    ZeroMemory(&ncm, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) return CreateFontIndirectW(&ncm.lfMessageFont);
    return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

void applyFont(Win& w) {
    if (w.ownsFont && w.hfont) DeleteObject(w.hfont);
    w.hfont = makeFont(w);
    w.ownsFont = true;
    for (auto& kv : g_ctls)
        if (kv.second.win == w.id && kv.second.hwnd) SendMessageW(kv.second.hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(w.hfont), TRUE);
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    int wid = winIdOf(h);
    auto wit = g_wins.find(wid);
    Win* w = wit == g_wins.end() ? nullptr : &wit->second;
    switch (msg) {
        case WM_COMMAND: {
            int cid = LOWORD(wp);
            int code = HIWORD(wp);
            auto cit = g_ctls.find(cid);
            if (!lp || cit == g_ctls.end()) break;
            Ctl& c = cit->second;
            if (c.kind == "button" && code == BN_CLICKED) pushEvent("click\t" + std::to_string(cid));
            else if (c.kind == "checkbox" && code == BN_CLICKED) {
                c.checked = SendMessageW(c.hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
                pushEvent("change\t" + std::to_string(cid));
                pushEvent("click\t" + std::to_string(cid));
            } else if (c.kind == "textbox" && code == EN_CHANGE && !g_suppress) pushEvent("change\t" + std::to_string(cid));
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            if (w && w->bgBrush) {
                HDC dc = reinterpret_cast<HDC>(wp);
                if (w->fg >= 0) SetTextColor(dc, rgb(w->fg));
                SetBkColor(dc, rgb(w->bg));
                return reinterpret_cast<LRESULT>(w->bgBrush);
            }
            break;
        }
        case WM_CTLCOLOREDIT: {
            if (w && w->fg >= 0) {
                HDC dc = reinterpret_cast<HDC>(wp);
                SetTextColor(dc, rgb(w->fg));
                SetBkColor(dc, RGB(255, 255, 255));
                return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
            }
            break;
        }
        case WM_ERASEBKGND: {
            if (w && w->bgBrush) {
                RECT rc;
                GetClientRect(h, &rc);
                FillRect(reinterpret_cast<HDC>(wp), &rc, w->bgBrush);
                return 1;
            }
            break;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* d = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (d && d->CtlType == ODT_BUTTON && w && w->accent >= 0) {
                COLORREF base = rgb(w->accent);
                COLORREF fill = base;
                if (d->itemState & ODS_DISABLED) fill = RGB(190, 190, 190);
                else if (d->itemState & ODS_SELECTED) fill = RGB(GetRValue(base) * 3 / 4, GetGValue(base) * 3 / 4, GetBValue(base) * 3 / 4);
                HBRUSH br = CreateSolidBrush(fill);
                FillRect(d->hDC, &d->rcItem, br);
                DeleteObject(br);
                SetBkMode(d->hDC, TRANSPARENT);
                SetTextColor(d->hDC, RGB(255, 255, 255));
                HGDIOBJ old = w->hfont ? SelectObject(d->hDC, w->hfont) : nullptr;
                std::wstring t = windowText(d->hwndItem);
                RECT rc = d->rcItem;
                DrawTextW(d->hDC, t.c_str(), static_cast<int>(t.size()), &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                if (old) SelectObject(d->hDC, old);
                if (d->itemState & ODS_FOCUS) {
                    RECT f = d->rcItem;
                    InflateRect(&f, -3, -3);
                    DrawFocusRect(d->hDC, &f);
                }
                return TRUE;
            }
            break;
        }
        case WM_SIZE: {
            if (w && wp != SIZE_MINIMIZED) {
                w->w = dp(LOWORD(lp));
                w->h = dp(HIWORD(lp));
                pushEvent("resize\t" + std::to_string(wid) + "\t" + std::to_string(w->w) + "\t" + std::to_string(w->h));
            }
            return 0;
        }
        case WM_CLOSE:
            pushEvent("close\t" + std::to_string(wid));
            return 0;  // the program decides (ui.windowClose)
        case WM_DESTROY: {
            if (w) {
                for (auto it = g_ctls.begin(); it != g_ctls.end();) it = it->second.win == wid ? g_ctls.erase(it) : std::next(it);
                if (w->bgBrush) DeleteObject(w->bgBrush);
                if (w->ownsFont && w->hfont) DeleteObject(w->hfont);
                g_wins.erase(wid);
            }
            return 0;
        }
        default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void ensureClass() {
    if (g_classReady) return;
    SetProcessDPIAware();
    HDC dc = GetDC(nullptr);
    g_scale = GetDeviceCaps(dc, LOGPIXELSX) / 96.0;
    ReleaseDC(nullptr, dc);
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc)) throw HostError{"ui: RegisterClassEx failed (error " + std::to_string(GetLastError()) + ")"};
    g_classReady = true;
}

void pumpMessages() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        HWND root = m.hwnd ? GetAncestor(m.hwnd, GA_ROOT) : nullptr;
        if (root && IsDialogMessageW(root, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

#endif  // _WIN32

// ---- host functions -------------------------------------------------------------------------------------------------

Value u_backend(const Args&, HostContext&) { return Value::ofString(wantWin32() ? "win32" : "headless"); }

Value u_windowCreate(const Args& a, HostContext&) {
    Win w;
    w.id = g_nextWin++;
    w.title = a[0].s;
    w.w = static_cast<int>(a[1].i);
    w.h = static_cast<int>(a[2].i);
    if (w.w < 1 || w.h < 1) throw HostError{"ui.windowCreate: the size must be positive"};
#ifdef _WIN32
    if (wantWin32()) {
        ensureClass();
        RECT rc = {0, 0, px(w.w), px(w.h)};
        AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_CONTROLPARENT);
        HWND h = CreateWindowExW(WS_EX_CONTROLPARENT, kClass, W(w.title).c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                 rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!h) throw HostError{"ui.windowCreate: CreateWindowEx failed (error " + std::to_string(GetLastError()) + ")"};
        SetWindowLongPtrW(h, GWLP_USERDATA, static_cast<LONG_PTR>(w.id));
        w.hwnd = h;
        g_wins[w.id] = w;
        applyFont(g_wins[w.id]);
        return Value::ofInt(w.id);
    }
#endif
    int id = w.id;
    g_wins[id] = w;
    return Value::ofInt(id);
}

Value u_windowShow(const Args& a, HostContext&) {
    Win& w = win(static_cast<int>(a[0].i));
    w.shown = true;
#ifdef _WIN32
    if (w.hwnd) {
        ShowWindow(w.hwnd, SW_SHOW);
        UpdateWindow(w.hwnd);
    }
#endif
    return Value::none();
}

Value u_windowClose(const Args& a, HostContext&) {
    int id = static_cast<int>(a[0].i);
    Win& w = win(id);
#ifdef _WIN32
    if (w.hwnd) {
        DestroyWindow(w.hwnd);  // WM_DESTROY removes the window and its controls from the model
        return Value::none();
    }
#endif
    (void)w;
    for (auto it = g_ctls.begin(); it != g_ctls.end();) it = it->second.win == id ? g_ctls.erase(it) : std::next(it);
    g_wins.erase(id);
    return Value::none();
}

Value u_windowSetTitle(const Args& a, HostContext&) {
    Win& w = win(static_cast<int>(a[0].i));
    w.title = a[1].s;
#ifdef _WIN32
    if (w.hwnd) SetWindowTextW(w.hwnd, W(w.title).c_str());
#endif
    return Value::none();
}

// windowSetTheme(win, background, foreground, accent, fontName, fontSize): colors are 0xRRGGBB or -1 (system default).
// Call it before creating the controls: buttons become owner-drawn (flat, accent colored) only when an accent is set.
Value u_windowSetTheme(const Args& a, HostContext&) {
    Win& w = win(static_cast<int>(a[0].i));
    w.bg = static_cast<int>(a[1].i);
    w.fg = static_cast<int>(a[2].i);
    w.accent = static_cast<int>(a[3].i);
    w.font = a[4].s;
    w.fontSize = static_cast<int>(a[5].i);
#ifdef _WIN32
    if (w.hwnd) {
        if (w.bgBrush) { DeleteObject(w.bgBrush); w.bgBrush = nullptr; }
        if (w.bg >= 0) w.bgBrush = CreateSolidBrush(rgb(w.bg));
        applyFont(w);
        InvalidateRect(w.hwnd, nullptr, TRUE);
    }
#endif
    return Value::none();
}

Value u_windowClientSize(const Args& a, HostContext&) {
    Win& w = win(static_cast<int>(a[0].i));
    return Value::ofString(std::to_string(w.w) + " " + std::to_string(w.h));
}

Value u_controlCreate(const Args& a, HostContext&) {
    Win& w = win(static_cast<int>(a[0].i));
    const std::string kind = a[1].s;
    if (kind != "label" && kind != "button" && kind != "textbox" && kind != "checkbox")
        throw HostError{"ui.controlCreate: unknown control kind '" + kind + "' (label, button, textbox, checkbox)"};
    Ctl c;
    c.id = g_nextCtl++;
    c.win = w.id;
    c.kind = kind;
    c.text = a[2].s;
#ifdef _WIN32
    if (w.hwnd) {
        const wchar_t* cls = L"STATIC";
        DWORD style = WS_CHILD | WS_VISIBLE;
        if (kind == "label") style |= SS_LEFT | SS_NOPREFIX;
        else if (kind == "button") { cls = L"BUTTON"; style |= WS_TABSTOP | (w.accent >= 0 ? BS_OWNERDRAW : BS_PUSHBUTTON); }
        else if (kind == "checkbox") { cls = L"BUTTON"; style |= WS_TABSTOP | BS_AUTOCHECKBOX; }
        else { cls = L"EDIT"; style |= WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL; }
        HWND h = CreateWindowExW(0, cls, W(c.text).c_str(), style, 0, 0, 10, 10, w.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(c.id)),
                                 GetModuleHandleW(nullptr), nullptr);
        if (!h) throw HostError{"ui.controlCreate: CreateWindowEx failed (error " + std::to_string(GetLastError()) + ")"};
        if (w.hfont) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(w.hfont), TRUE);
        c.hwnd = h;
    }
#endif
    g_ctls[c.id] = c;
    return Value::ofInt(c.id);
}

Value u_controlSetText(const Args& a, HostContext&) {
    Ctl& c = ctl(static_cast<int>(a[0].i));
    c.text = a[1].s;
#ifdef _WIN32
    if (c.hwnd) {
        g_suppress = true;
        SetWindowTextW(c.hwnd, W(c.text).c_str());
        g_suppress = false;
    }
#endif
    return Value::none();
}

Value u_controlGetText(const Args& a, HostContext&) {
    Ctl& c = ctl(static_cast<int>(a[0].i));
#ifdef _WIN32
    if (c.hwnd) c.text = U8(windowText(c.hwnd));
#endif
    return Value::ofString(c.text);
}

Value u_controlSetVisible(const Args& a, HostContext&) {
    Ctl& c = ctl(static_cast<int>(a[0].i));
    c.visible = a[1].i != 0;
#ifdef _WIN32
    if (c.hwnd) ShowWindow(c.hwnd, c.visible ? SW_SHOW : SW_HIDE);
#endif
    return Value::none();
}

Value u_controlGetVisible(const Args& a, HostContext&) { return Value::ofBool(ctl(static_cast<int>(a[0].i)).visible); }

Value u_controlSetEnabled(const Args& a, HostContext&) {
    Ctl& c = ctl(static_cast<int>(a[0].i));
    c.enabled = a[1].i != 0;
#ifdef _WIN32
    if (c.hwnd) EnableWindow(c.hwnd, c.enabled ? TRUE : FALSE);
#endif
    return Value::none();
}

Value u_controlGetEnabled(const Args& a, HostContext&) { return Value::ofBool(ctl(static_cast<int>(a[0].i)).enabled); }

Value u_controlSetChecked(const Args& a, HostContext&) {
    Ctl& c = ctl(static_cast<int>(a[0].i));
    c.checked = a[1].i != 0;
#ifdef _WIN32
    if (c.hwnd && c.kind == "checkbox") SendMessageW(c.hwnd, BM_SETCHECK, c.checked ? BST_CHECKED : BST_UNCHECKED, 0);
#endif
    return Value::none();
}

Value u_controlGetChecked(const Args& a, HostContext&) {
    Ctl& c = ctl(static_cast<int>(a[0].i));
#ifdef _WIN32
    if (c.hwnd && c.kind == "checkbox") c.checked = SendMessageW(c.hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
#endif
    return Value::ofBool(c.checked);
}

Value u_controlSetBounds(const Args& a, HostContext&) {
    Ctl& c = ctl(static_cast<int>(a[0].i));
    c.x = static_cast<int>(a[1].i);
    c.y = static_cast<int>(a[2].i);
    c.w = static_cast<int>(a[3].i);
    c.h = static_cast<int>(a[4].i);
#ifdef _WIN32
    if (c.hwnd) MoveWindow(c.hwnd, px(c.x), px(c.y), px(c.w), px(c.h), TRUE);
#endif
    return Value::none();
}

// The bounds last given to a control: "x y width height" (for tests and layout checks).
Value u_controlBounds(const Args& a, HostContext&) {
    Ctl& c = ctl(static_cast<int>(a[0].i));
    return Value::ofString(std::to_string(c.x) + " " + std::to_string(c.y) + " " + std::to_string(c.w) + " " + std::to_string(c.h));
}

// nextEvent(waitMs): the next event, or "" when none arrived within waitMs (-1 = wait forever); "quit" when no window is open
// (headless: when no event is pending).
Value u_nextEvent(const Args& a, HostContext&) {
    long long waitMs = a[0].i;
#ifdef _WIN32
    if (wantWin32()) {
        ULONGLONG deadline = waitMs > 0 ? GetTickCount64() + static_cast<ULONGLONG>(waitMs) : 0;
        for (;;) {
            pumpMessages();
            if (!g_events.empty()) {
                std::string e = g_events.front();
                g_events.pop_front();
                return Value::ofString(e);
            }
            if (g_wins.empty()) return Value::ofString("quit");
            if (waitMs == 0) return Value::ofString("");
            DWORD t = INFINITE;
            if (waitMs > 0) {
                ULONGLONG now = GetTickCount64();
                if (now >= deadline) return Value::ofString("");
                t = static_cast<DWORD>(deadline - now);
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, t, QS_ALLINPUT);
        }
    }
#endif
    (void)waitMs;
    if (g_events.empty()) return Value::ofString("quit");
    std::string e = g_events.front();
    g_events.pop_front();
    return Value::ofString(e);
}

// Simulation (both backends): act as if the user had clicked / typed / closed.
Value u_simClick(const Args& a, HostContext&) {
    Ctl& c = ctl(static_cast<int>(a[0].i));
    if (!c.enabled || !c.visible) return Value::none();
    if (c.kind == "checkbox") {
        c.checked = !c.checked;
#ifdef _WIN32
        if (c.hwnd) SendMessageW(c.hwnd, BM_SETCHECK, c.checked ? BST_CHECKED : BST_UNCHECKED, 0);
#endif
        pushEvent("change\t" + std::to_string(c.id));
    }
    pushEvent("click\t" + std::to_string(c.id));
    return Value::none();
}

Value u_simSetText(const Args& a, HostContext& ctx) {
    u_controlSetText(a, ctx);
    pushEvent("change\t" + std::to_string(a[0].i));
    return Value::none();
}

Value u_simClose(const Args& a, HostContext&) {
    win(static_cast<int>(a[0].i));
    pushEvent("close\t" + std::to_string(a[0].i));
    return Value::none();
}

}  // namespace

void registerUiHosts(HostRegistry& r) {
    const Type S = Type::String, I = Type::Int, B = Type::Bool, V = Type::Void;
    add(r, "backend", {}, S, u_backend);
    add(r, "windowCreate", {S, I, I}, I, u_windowCreate);
    add(r, "windowShow", {I}, V, u_windowShow);
    add(r, "windowClose", {I}, V, u_windowClose);
    add(r, "windowSetTitle", {I, S}, V, u_windowSetTitle);
    add(r, "windowSetTheme", {I, I, I, I, S, I}, V, u_windowSetTheme);
    add(r, "windowClientSize", {I}, S, u_windowClientSize);
    add(r, "controlCreate", {I, S, S}, I, u_controlCreate);
    add(r, "controlSetText", {I, S}, V, u_controlSetText);
    add(r, "controlGetText", {I}, S, u_controlGetText);
    add(r, "controlSetVisible", {I, B}, V, u_controlSetVisible);
    add(r, "controlGetVisible", {I}, B, u_controlGetVisible);
    add(r, "controlSetEnabled", {I, B}, V, u_controlSetEnabled);
    add(r, "controlGetEnabled", {I}, B, u_controlGetEnabled);
    add(r, "controlSetChecked", {I, B}, V, u_controlSetChecked);
    add(r, "controlGetChecked", {I}, B, u_controlGetChecked);
    add(r, "controlSetBounds", {I, I, I, I, I}, V, u_controlSetBounds);
    add(r, "controlBounds", {I}, S, u_controlBounds);
    add(r, "nextEvent", {I}, S, u_nextEvent);
    add(r, "simClick", {I}, V, u_simClick);
    add(r, "simSetText", {I, S}, V, u_simSetText);
    add(r, "simClose", {I}, V, u_simClose);
}

}  // namespace pvm
