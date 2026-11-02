// Host modules that touch the outside world: file, dir ("file"); env, sys ("system"); http ("net"); time.format ("time").
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "host.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace pvm {

namespace {

namespace fs = std::filesystem;

constexpr size_t kMaxFileBytes = 64u * 1024u * 1024u;
constexpr size_t kMaxHttpBytes = 32u * 1024u * 1024u;

Sig sig(std::vector<Type> params, Type ret) {
    Sig s;
    s.params = std::move(params);
    s.ret = ret;
    return s;
}

using Args = std::vector<Value>;

void add(HostRegistry& r, const std::string& name, std::vector<Type> params, Type ret, const std::string& perm,
         std::function<Value(const Args&, HostContext&)> fn) {
    r.add(HostFunction{name, sig(std::move(params), ret), perm, std::move(fn)});
}

fs::path pathOf(const std::string& s) {
    if (s.empty()) throw HostError{"the path is empty"};
    if (s.find('\0') != std::string::npos) throw HostError{"the path contains a NUL character"};
    return fs::u8path(s);
}

[[noreturn]] void fail(const std::string& what, const std::string& path, const std::error_code& ec) {
    throw HostError{what + " '" + path + "': " + (ec ? ec.message() : "failed")};
}

std::string readFile(const std::string& path) {
    fs::path p = pathOf(path);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) throw HostError{"cannot read '" + path + "': no such file"};
    auto size = fs::file_size(p, ec);
    if (ec) fail("cannot read", path, ec);
    if (size > kMaxFileBytes) throw HostError{"'" + path + "' is larger than the " + std::to_string(kMaxFileBytes / (1024 * 1024)) + " MiB read limit"};
    std::ifstream in(p, std::ios::binary);
    if (!in) throw HostError{"cannot open '" + path + "' for reading"};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFile(const std::string& path, const std::string& text, bool append) {
    fs::path p = pathOf(path);
    if (text.size() > kMaxFileBytes) throw HostError{"refusing to write more than " + std::to_string(kMaxFileBytes / (1024 * 1024)) + " MiB at once"};
    std::ofstream out(p, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!out) throw HostError{"cannot open '" + path + "' for writing"};
    out << text;
    out.flush();
    if (!out) throw HostError{"failed to write '" + path + "'"};
}

long long unixOf(fs::file_time_type t) {
    using namespace std::chrono;
    auto sctp = time_point_cast<system_clock::duration>(t - fs::file_time_type::clock::now() + system_clock::now());
    return static_cast<long long>(duration_cast<seconds>(sctp.time_since_epoch()).count());
}

// ---------- http ----------

struct HttpState {
    int status = 0;
    std::string body, headers, error;  // headers: "Name: value\n" lines
} g_http;

std::string lowerCopy(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

std::string headerOf(const std::string& headers, const std::string& name) {
    const std::string want = lowerCopy(name);
    size_t pos = 0;
    while (pos < headers.size()) {
        size_t nl = headers.find('\n', pos);
        if (nl == std::string::npos) nl = headers.size();
        std::string line = headers.substr(pos, nl - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos && lowerCopy(line.substr(0, colon)) == want) {
            size_t v = colon + 1;
            while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
            return line.substr(v);
        }
        pos = nl + 1;
    }
    return "";
}

struct Url {
    std::string scheme, host, path = "/";
    int port = 80;
};

bool parseUrl(const std::string& u, Url& out, std::string& why) {
    size_t s = u.find("://");
    if (s == std::string::npos) { why = "the URL must start with http:// or https://"; return false; }
    out.scheme = lowerCopy(u.substr(0, s));
    if (out.scheme != "http" && out.scheme != "https") { why = "unsupported URL scheme '" + out.scheme + "'"; return false; }
    out.port = out.scheme == "https" ? 443 : 80;
    size_t hostStart = s + 3;
    size_t pathStart = u.find_first_of("/?#", hostStart);
    std::string authority = u.substr(hostStart, pathStart == std::string::npos ? std::string::npos : pathStart - hostStart);
    if (pathStart != std::string::npos) {
        out.path = u.substr(pathStart);
        if (out.path[0] != '/') out.path = "/" + out.path;
        size_t hash = out.path.find('#');
        if (hash != std::string::npos) out.path.erase(hash);
    }
    size_t at = authority.rfind('@');
    if (at != std::string::npos) authority.erase(0, at + 1);
    size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        std::string pt = authority.substr(colon + 1);
        if (pt.empty() || pt.size() > 5 || pt.find_first_not_of("0123456789") != std::string::npos) { why = "invalid port in the URL"; return false; }
        out.port = std::stoi(pt);
        authority.erase(colon);
    }
    out.host = authority;
    if (out.host.empty()) { why = "the URL has no host"; return false; }
    return true;
}

#ifdef _WIN32

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::string winError(const char* what) {
    return std::string(what) + " failed (error " + std::to_string(GetLastError()) + ")";
}

bool doRequest(const std::string& method, const std::string& url, const std::string& headers, const std::string& body, int timeoutMs, HttpState& st) {
    Url u;
    if (!parseUrl(url, u, st.error)) return false;
    HINTERNET ses = WinHttpOpen(L"Coyote/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { st.error = winError("WinHttpOpen"); return false; }
    WinHttpSetTimeouts(ses, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    HINTERNET con = WinHttpConnect(ses, widen(u.host).c_str(), static_cast<INTERNET_PORT>(u.port), 0);
    if (!con) { st.error = winError("WinHttpConnect"); WinHttpCloseHandle(ses); return false; }
    HINTERNET req = WinHttpOpenRequest(con, widen(method).c_str(), widen(u.path).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       u.scheme == "https" ? WINHTTP_FLAG_SECURE : 0);
    bool ok = false;
    if (!req) {
        st.error = winError("WinHttpOpenRequest");
    } else {
        std::wstring hdr;
        size_t pos = 0;
        while (pos < headers.size()) {
            size_t nl = headers.find('\n', pos);
            if (nl == std::string::npos) nl = headers.size();
            std::string line = headers.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) hdr += widen(line) + L"\r\n";
            pos = nl + 1;
        }
        if (WinHttpSendRequest(req, hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(), hdr.empty() ? 0 : static_cast<DWORD>(-1L),
                               body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                               static_cast<DWORD>(body.size()), 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD status = 0, len = sizeof status;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
            st.status = static_cast<int>(status);
            DWORD hl = 0;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &hl, WINHTTP_NO_HEADER_INDEX);
            if (hl > 0) {
                std::wstring raw(hl / sizeof(wchar_t), L'\0');
                if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, &raw[0], &hl, WINHTTP_NO_HEADER_INDEX)) {
                    raw.resize(hl / sizeof(wchar_t));
                    std::string h = narrow(raw);
                    std::string out;
                    bool first = true;
                    size_t p = 0;
                    while (p < h.size()) {
                        size_t nl = h.find("\r\n", p);
                        if (nl == std::string::npos) nl = h.size();
                        if (!first && nl > p) out += h.substr(p, nl - p) + "\n";
                        first = false;
                        p = nl + 2;
                    }
                    st.headers = out;
                }
            }
            ok = true;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
                std::string chunk(avail, '\0');
                DWORD got = 0;
                if (!WinHttpReadData(req, &chunk[0], avail, &got) || got == 0) break;
                st.body.append(chunk, 0, got);
                if (st.body.size() > kMaxHttpBytes) { st.error = "the response is larger than the 32 MiB limit"; ok = false; break; }
            }
        } else {
            st.error = winError("the request");
        }
        WinHttpCloseHandle(req);
    }
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

#else

// Plain HTTP over sockets. HTTPS needs a TLS library, which this build does not have.
bool socketRequest(const std::string& method, const Url& u, const std::string& headers, const std::string& body, int timeoutMs, HttpState& st) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    int gai = getaddrinfo(u.host.c_str(), std::to_string(u.port).c_str(), &hints, &res);
    if (gai != 0) { st.error = "cannot resolve '" + u.host + "': " + gai_strerror(gai); return false; }
    int fd = -1;
    for (addrinfo* a = res; a; a = a->ai_next) {
        fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (::connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) { st.error = "cannot connect to " + u.host + ":" + std::to_string(u.port); return false; }

    std::string req = method + " " + u.path + " HTTP/1.1\r\nHost: " + u.host + (u.port == 80 ? "" : ":" + std::to_string(u.port)) +
                      "\r\nConnection: close\r\nUser-Agent: Coyote/1.0\r\nAccept: */*\r\n";
    bool haveLen = false;
    size_t pos = 0;
    while (pos < headers.size()) {
        size_t nl = headers.find('\n', pos);
        if (nl == std::string::npos) nl = headers.size();
        std::string line = headers.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) {
            if (lowerCopy(line).rfind("content-length:", 0) == 0) haveLen = true;
            req += line + "\r\n";
        }
        pos = nl + 1;
    }
    if (!haveLen && (!body.empty() || method == "POST" || method == "PUT" || method == "PATCH")) req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    for (size_t sent = 0; sent < req.size();) {
        ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, 0);
        if (n <= 0) { st.error = "sending the request failed"; ::close(fd); return false; }
        sent += static_cast<size_t>(n);
    }
    std::string raw;
    char buf[16384];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n < 0) { st.error = "the server did not answer in time"; ::close(fd); return false; }
        if (n == 0) break;
        raw.append(buf, static_cast<size_t>(n));
        if (raw.size() > kMaxHttpBytes + 65536) { st.error = "the response is larger than the 32 MiB limit"; ::close(fd); return false; }
    }
    ::close(fd);

    size_t headEnd = raw.find("\r\n\r\n");
    if (headEnd == std::string::npos || raw.compare(0, 5, "HTTP/") != 0) { st.error = "the server sent an invalid response"; return false; }
    std::string head = raw.substr(0, headEnd);
    size_t sp = head.find(' ');
    st.status = sp == std::string::npos ? 0 : std::atoi(head.c_str() + sp + 1);
    size_t firstNl = head.find("\r\n");
    std::string hs = firstNl == std::string::npos ? "" : head.substr(firstNl + 2);
    std::string clean;
    for (char c : hs) if (c != '\r') clean += c;
    st.headers = clean;
    std::string rest = raw.substr(headEnd + 4);
    if (lowerCopy(headerOf(st.headers, "Transfer-Encoding")).find("chunked") != std::string::npos) {
        std::string out;
        size_t p = 0;
        for (;;) {
            size_t nl = rest.find("\r\n", p);
            if (nl == std::string::npos) break;
            unsigned long n = std::strtoul(rest.substr(p, nl - p).c_str(), nullptr, 16);
            if (n == 0) break;
            p = nl + 2;
            if (p + n > rest.size()) { st.error = "the chunked response is truncated"; return false; }
            out.append(rest, p, n);
            p += n + 2;
        }
        st.body = out;
    } else {
        st.body = rest;
    }
    return true;
}

bool doRequest(const std::string& method, const std::string& url, const std::string& headers, const std::string& body, int timeoutMs, HttpState& st) {
    std::string current = url;
    for (int hop = 0; hop < 6; ++hop) {
        Url u;
        if (!parseUrl(current, u, st.error)) return false;
        if (u.scheme == "https") {
            st.error = "HTTPS is not available in this build on this platform (Windows builds support it; use http:// here)";
            return false;
        }
        st.status = 0; st.body.clear(); st.headers.clear();
        if (!socketRequest(method, u, headers, body, timeoutMs, st)) return false;
        bool redirect = st.status == 301 || st.status == 302 || st.status == 303 || st.status == 307 || st.status == 308;
        std::string loc = headerOf(st.headers, "Location");
        if (!redirect || loc.empty() || (method != "GET" && method != "HEAD")) return true;
        if (loc[0] == '/') current = u.scheme + "://" + u.host + (u.port == 80 ? "" : ":" + std::to_string(u.port)) + loc;
        else current = loc;
    }
    st.error = "too many redirects";
    return false;
}

#endif

} // namespace

void registerSystemHosts(HostRegistry& r) {
    // ---- file ----
    add(r, "file.readAll", {Type::String}, Type::String, "file", [](const Args& a, HostContext&) { return Value::ofString(readFile(a[0].s)); });
    add(r, "file.writeAll", {Type::String, Type::String}, Type::Void, "file", [](const Args& a, HostContext&) { writeFile(a[0].s, a[1].s, false); return Value::none(); });
    add(r, "file.append", {Type::String, Type::String}, Type::Void, "file", [](const Args& a, HostContext&) { writeFile(a[0].s, a[1].s, true); return Value::none(); });
    add(r, "file.exists", {Type::String}, Type::Bool, "file", [](const Args& a, HostContext&) {
        std::error_code ec;
        return Value::ofBool(fs::is_regular_file(pathOf(a[0].s), ec));
    });
    add(r, "file.delete", {Type::String}, Type::Bool, "file", [](const Args& a, HostContext&) {
        std::error_code ec;
        fs::path p = pathOf(a[0].s);
        if (!fs::is_regular_file(p, ec)) return Value::ofBool(false);
        if (!fs::remove(p, ec)) fail("cannot delete", a[0].s, ec);
        return Value::ofBool(true);
    });
    add(r, "file.copy", {Type::String, Type::String}, Type::Void, "file", [](const Args& a, HostContext&) {
        std::error_code ec;
        fs::copy_file(pathOf(a[0].s), pathOf(a[1].s), fs::copy_options::overwrite_existing, ec);
        if (ec) fail("cannot copy", a[0].s, ec);
        return Value::none();
    });
    add(r, "file.move", {Type::String, Type::String}, Type::Void, "file", [](const Args& a, HostContext&) {
        std::error_code ec;
        fs::rename(pathOf(a[0].s), pathOf(a[1].s), ec);
        if (ec) fail("cannot move", a[0].s, ec);
        return Value::none();
    });
    add(r, "file.size", {Type::String}, Type::Int, "file", [](const Args& a, HostContext&) {
        std::error_code ec;
        auto n = fs::file_size(pathOf(a[0].s), ec);
        if (ec) fail("cannot get the size of", a[0].s, ec);
        if (n > 2147483647ull) throw HostError{"'" + a[0].s + "' is larger than 2 GiB, which does not fit an int"};
        return Value::ofInt(static_cast<long long>(n));
    });
    add(r, "file.modified", {Type::String}, Type::Int, "file", [](const Args& a, HostContext&) {
        std::error_code ec;
        auto t = fs::last_write_time(pathOf(a[0].s), ec);
        if (ec) fail("cannot get the time of", a[0].s, ec);
        return Value::ofInt(unixOf(t));
    });
    add(r, "file.fullPath", {Type::String}, Type::String, "file", [](const Args& a, HostContext&) {
        std::error_code ec;
        fs::path p = fs::weakly_canonical(pathOf(a[0].s), ec);
        if (ec) fail("cannot resolve", a[0].s, ec);
        return Value::ofString(p.u8string());
    });

    // ---- dir ----
    add(r, "dir.exists", {Type::String}, Type::Bool, "file", [](const Args& a, HostContext&) {
        std::error_code ec;
        return Value::ofBool(fs::is_directory(pathOf(a[0].s), ec));
    });
    add(r, "dir.create", {Type::String}, Type::Void, "file", [](const Args& a, HostContext&) {
        std::error_code ec;
        fs::create_directories(pathOf(a[0].s), ec);
        if (ec) fail("cannot create the folder", a[0].s, ec);
        return Value::none();
    });
    add(r, "dir.delete", {Type::String, Type::Bool}, Type::Bool, "file", [](const Args& a, HostContext&) {
        std::error_code ec;
        fs::path p = pathOf(a[0].s);
        if (!fs::is_directory(p, ec)) return Value::ofBool(false);
        if (a[1].i) fs::remove_all(p, ec); else fs::remove(p, ec);
        if (ec) fail("cannot delete the folder", a[0].s, ec);
        return Value::ofBool(true);
    });
    add(r, "dir.list", {Type::String}, Type::String, "file", [](const Args& a, HostContext&) {
        std::error_code ec;
        fs::path p = pathOf(a[0].s);
        if (!fs::is_directory(p, ec)) throw HostError{"cannot list '" + a[0].s + "': not a folder"};
        std::vector<std::string> names;
        for (fs::directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec)) {
            std::string n = it->path().filename().u8string();
            std::error_code e2;
            if (it->is_directory(e2)) n += "/";
            names.push_back(n);
        }
        if (ec) fail("cannot list", a[0].s, ec);
        std::sort(names.begin(), names.end());
        std::string out;
        for (size_t i = 0; i < names.size(); ++i) out += (i ? "\n" : "") + names[i];
        return Value::ofString(out);
    });

    // ---- env / sys ----
    add(r, "env.get", {Type::String}, Type::String, "system", [](const Args& a, HostContext&) {
        const char* v = std::getenv(a[0].s.c_str());
        return Value::ofString(v ? v : "");
    });
    add(r, "env.has", {Type::String}, Type::Bool, "system", [](const Args& a, HostContext&) { return Value::ofBool(std::getenv(a[0].s.c_str()) != nullptr); });
    add(r, "env.set", {Type::String, Type::String}, Type::Void, "system", [](const Args& a, HostContext&) {
        if (a[0].s.empty() || a[0].s.find('=') != std::string::npos) throw HostError{"invalid environment variable name '" + a[0].s + "'"};
#ifdef _WIN32
        _putenv_s(a[0].s.c_str(), a[1].s.c_str());
#else
        setenv(a[0].s.c_str(), a[1].s.c_str(), 1);
#endif
        return Value::none();
    });
    add(r, "sys.argCount", {}, Type::Int, "system", [](const Args&, HostContext& c) { return Value::ofInt(c.args ? static_cast<long long>(c.args->size()) : 0); });
    add(r, "sys.arg", {Type::Int}, Type::String, "system", [](const Args& a, HostContext& c) {
        if (!c.args || a[0].i < 0 || a[0].i >= static_cast<long long>(c.args->size())) throw HostError{"argument " + std::to_string(a[0].i) + " does not exist"};
        return Value::ofString((*c.args)[static_cast<size_t>(a[0].i)]);
    });
    add(r, "sys.exit", {Type::Int}, Type::Void, "system", [](const Args& a, HostContext& c) -> Value {
        c.out.flush();
        throw HostExit{static_cast<int>(a[0].i & 0xFF)};
    });
    add(r, "sys.cwd", {}, Type::String, "system", [](const Args&, HostContext&) {
        std::error_code ec;
        return Value::ofString(fs::current_path(ec).u8string());
    });
    add(r, "sys.os", {}, Type::String, "system", [](const Args&, HostContext&) {
#ifdef _WIN32
        return Value::ofString("windows");
#elif defined(__APPLE__)
        return Value::ofString("macos");
#else
        return Value::ofString("linux");
#endif
    });
    add(r, "sys.homeDir", {}, Type::String, "system", [](const Args&, HostContext&) {
        const char* h = std::getenv("USERPROFILE");
        if (!h || !*h) h = std::getenv("HOME");
        return Value::ofString(h ? h : "");
    });

    // ---- time.format ----
    add(r, "time.format", {Type::Int, Type::String, Type::Bool}, Type::String, "time", [](const Args& a, HostContext&) {
        if (a[1].s.empty() || a[1].s.size() > 200) throw HostError{"the time format must be 1 to 200 characters (strftime codes such as %Y-%m-%d %H:%M:%S)"};
        std::time_t t = static_cast<std::time_t>(a[0].i);
        std::tm tmv{};
#ifdef _WIN32
        if (a[2].i) gmtime_s(&tmv, &t); else localtime_s(&tmv, &t);
#else
        if (a[2].i) gmtime_r(&t, &tmv); else localtime_r(&t, &tmv);
#endif
        char buf[512];
        size_t n = std::strftime(buf, sizeof buf, a[1].s.c_str(), &tmv);
        return Value::ofString(std::string(buf, n));
    });

    // ---- http ----
    // http.request performs the call and keeps the answer; http.body/header/headers/error read it.
    add(r, "http.request", {Type::String, Type::String, Type::String, Type::String, Type::Int}, Type::Int, "net", [](const Args& a, HostContext& c) {
        c.out.flush();
        if (a[4].i < 100 || a[4].i > 600000) throw HostError{"the timeout must be 100 to 600000 milliseconds"};
        std::string method = a[0].s;
        for (char& ch : method) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 32);
        if (method.empty() || method.find_first_of(" \r\n") != std::string::npos) throw HostError{"invalid HTTP method"};
        g_http = HttpState{};
        bool ok = doRequest(method, a[1].s, a[2].s, a[3].s, static_cast<int>(a[4].i), g_http);
        if (!ok) { g_http.status = 0; if (g_http.error.empty()) g_http.error = "the request failed"; }
        return Value::ofInt(g_http.status);
    });
    add(r, "http.body", {}, Type::String, "net", [](const Args&, HostContext&) { return Value::ofString(g_http.body); });
    add(r, "http.headers", {}, Type::String, "net", [](const Args&, HostContext&) { return Value::ofString(g_http.headers); });
    add(r, "http.header", {Type::String}, Type::String, "net", [](const Args& a, HostContext&) { return Value::ofString(headerOf(g_http.headers, a[0].s)); });
    add(r, "http.error", {}, Type::String, "net", [](const Args&, HostContext&) { return Value::ofString(g_http.error); });
}

} // namespace pvm
