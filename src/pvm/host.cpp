#include "host.hpp"

namespace pvm {

namespace {

Value writeText(const std::string& text, HostContext& ctx) {
    ctx.out << text;
    if (!ctx.out) throw HostError{"failed to write to standard output"};
    return Value::none();
}

} // namespace

void registerConsoleHosts(HostRegistry& r) {
    Sig oneString;
    oneString.params = {Type::String};
    oneString.ret = Type::Void;

    r.add(HostFunction{"con.print", oneString, "console",
                       [](const std::vector<Value>& a, HostContext& c) { return writeText(a[0].s, c); }});
    r.add(HostFunction{"con.println", oneString, "console",
                       [](const std::vector<Value>& a, HostContext& c) { return writeText(a[0].s + "\n", c); }});
}

} // namespace pvm

// ---- standard host modules: con (input), math, text, time, random ----

#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
#include <thread>

#include "ops.hpp"

namespace pvm {

namespace {

Sig sig(std::vector<Type> params, Type ret) {
    Sig s;
    s.params = std::move(params);
    s.ret = ret;
    return s;
}

// Strings are UTF-8; text.* works on code points, not bytes.
std::vector<size_t> cpStarts(const std::string& s) {
    std::vector<size_t> starts;
    for (size_t i = 0; i < s.size(); ++i)
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) starts.push_back(i);
    return starts;
}

long long decodeAt(const std::string& s, size_t i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 0;
    long long cp = n == 0 ? c : (c & (0x3F >> n));
    for (int k = 1; k <= n && i + static_cast<size_t>(k) < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<size_t>(k)]) & 0x3F);
    return cp;
}

std::string substringCp(const std::string& s, long long start, long long len) {
    std::vector<size_t> st = cpStarts(s);
    long long n = static_cast<long long>(st.size());
    if (start < 0 || start > n) throw HostError{"substring start " + std::to_string(start) + " is outside the text (length " + std::to_string(n) + ")"};
    if (len < 0) throw HostError{"substring length " + std::to_string(len) + " is negative"};
    long long end = start + len > n ? n : start + len;
    size_t b = start == n ? s.size() : st[static_cast<size_t>(start)];
    size_t e = end == n ? s.size() : st[static_cast<size_t>(end)];
    return s.substr(b, e - b);
}

std::mt19937_64& rng() {
    static std::mt19937_64 g{std::random_device{}()};
    return g;
}

double checkedDouble(double v, const char* fn) {
    if (std::isnan(v)) throw HostError{std::string(fn) + " produced a value that is not a number"};
    return v;
}

} // namespace

void registerAllHosts(HostRegistry& r) {
    registerConsoleHosts(r);

    // con: input
    r.add(HostFunction{"con.readLine", sig({}, Type::String), "console", [](const std::vector<Value>&, HostContext& c) {
        if (!c.in) throw HostError{"no input is available in this mode"};
        std::string line;
        if (!std::getline(*c.in, line)) return Value::ofString("");
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return Value::ofString(line);
    }});
    r.add(HostFunction{"con.hasInput", sig({}, Type::Bool), "console", [](const std::vector<Value>&, HostContext& c) {
        return Value::ofBool(c.in && c.in->peek() != std::char_traits<char>::eof());
    }});

    // math: pure
    auto m1 = [&](const char* name, double (*f)(double)) {
        r.add(HostFunction{std::string("math.") + name, sig({Type::Double}, Type::Double), "",
                           [f, name](const std::vector<Value>& a, HostContext&) { return Value::ofDouble(checkedDouble(f(a[0].d), name)); }});
    };
    m1("sqrt", [](double x) { return std::sqrt(x); });
    m1("sin", [](double x) { return std::sin(x); });
    m1("cos", [](double x) { return std::cos(x); });
    m1("tan", [](double x) { return std::tan(x); });
    m1("asin", [](double x) { return std::asin(x); });
    m1("acos", [](double x) { return std::acos(x); });
    m1("atan", [](double x) { return std::atan(x); });
    m1("exp", [](double x) { return std::exp(x); });
    m1("log", [](double x) { return std::log(x); });
    m1("log10", [](double x) { return std::log10(x); });
    m1("floor", [](double x) { return std::floor(x); });
    m1("ceil", [](double x) { return std::ceil(x); });
    m1("round", [](double x) { return std::round(x); });
    r.add(HostFunction{"math.pow", sig({Type::Double, Type::Double}, Type::Double), "",
                       [](const std::vector<Value>& a, HostContext&) { return Value::ofDouble(checkedDouble(std::pow(a[0].d, a[1].d), "pow")); }});
    r.add(HostFunction{"math.atan2", sig({Type::Double, Type::Double}, Type::Double), "",
                       [](const std::vector<Value>& a, HostContext&) { return Value::ofDouble(std::atan2(a[0].d, a[1].d)); }});

    // text: pure, code-point based
    r.add(HostFunction{"text.length", sig({Type::String}, Type::Int), "",
                       [](const std::vector<Value>& a, HostContext&) { return Value::ofInt(static_cast<long long>(cpStarts(a[0].s).size())); }});
    r.add(HostFunction{"text.substring", sig({Type::String, Type::Int, Type::Int}, Type::String), "",
                       [](const std::vector<Value>& a, HostContext&) { return Value::ofString(substringCp(a[0].s, a[1].i, a[2].i)); }});
    r.add(HostFunction{"text.charAt", sig({Type::String, Type::Int}, Type::Char), "", [](const std::vector<Value>& a, HostContext&) {
        std::vector<size_t> st = cpStarts(a[0].s);
        if (a[1].i < 0 || a[1].i >= static_cast<long long>(st.size()))
            throw HostError{"charAt index " + std::to_string(a[1].i) + " is outside the text (length " + std::to_string(st.size()) + ")"};
        return Value::ofChar(decodeAt(a[0].s, st[static_cast<size_t>(a[1].i)]));
    }});
    r.add(HostFunction{"text.indexOf", sig({Type::String, Type::String}, Type::Int), "", [](const std::vector<Value>& a, HostContext&) {
        size_t p = a[0].s.find(a[1].s);
        if (p == std::string::npos) return Value::ofInt(-1);
        return Value::ofInt(static_cast<long long>(cpStarts(a[0].s.substr(0, p)).size()));
    }});
    r.add(HostFunction{"text.upper", sig({Type::String}, Type::String), "", [](const std::vector<Value>& a, HostContext&) {
        std::string s = a[0].s;
        for (char& ch : s) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 32);  // ASCII only
        return Value::ofString(s);
    }});
    r.add(HostFunction{"text.lower", sig({Type::String}, Type::String), "", [](const std::vector<Value>& a, HostContext&) {
        std::string s = a[0].s;
        for (char& ch : s) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 32);  // ASCII only
        return Value::ofString(s);
    }});
    r.add(HostFunction{"text.trim", sig({Type::String}, Type::String), "", [](const std::vector<Value>& a, HostContext&) {
        const std::string& s = a[0].s;
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return Value::ofString("");
        size_t e = s.find_last_not_of(" \t\r\n");
        return Value::ofString(s.substr(b, e - b + 1));
    }});

    // time ("time")
    r.add(HostFunction{"time.uptimeMs", sig({}, Type::Int), "time", [](const std::vector<Value>&, HostContext&) {
        // Milliseconds since the first call in this process; wraps to 0 after about 24 days (int is 32-bit).
        using namespace std::chrono;
        static const steady_clock::time_point start = steady_clock::now();
        return Value::ofInt(static_cast<long long>(duration_cast<milliseconds>(steady_clock::now() - start).count() % 2147483647LL));
    }});
    r.add(HostFunction{"time.unixSeconds", sig({}, Type::Int), "time", [](const std::vector<Value>&, HostContext&) {
        using namespace std::chrono;
        long long s = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
        if (s > 2147483647LL) throw HostError{"the Unix time no longer fits in an int"};
        return Value::ofInt(s);
    }});
    r.add(HostFunction{"time.sleep", sig({Type::Int}, Type::Void), "time", [](const std::vector<Value>& a, HostContext& c) {
        if (a[0].i < 0 || a[0].i > 60000) throw HostError{"sleep takes 0 to 60000 milliseconds"};
        c.out.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(a[0].i));
        return Value::none();
    }});

    // random ("random")
    r.add(HostFunction{"random.seed", sig({Type::Int}, Type::Void), "random", [](const std::vector<Value>& a, HostContext&) {
        rng().seed(static_cast<unsigned long long>(a[0].i));
        return Value::none();
    }});
    r.add(HostFunction{"random.nextInt", sig({Type::Int, Type::Int}, Type::Int), "random", [](const std::vector<Value>& a, HostContext&) {
        if (a[0].i > a[1].i) throw HostError{"random range is empty (" + std::to_string(a[0].i) + " > " + std::to_string(a[1].i) + ")"};
        return Value::ofInt(std::uniform_int_distribution<long long>(a[0].i, a[1].i)(rng()));
    }});
    r.add(HostFunction{"random.nextDouble", sig({}, Type::Double), "random", [](const std::vector<Value>&, HostContext&) {
        return Value::ofDouble(std::uniform_real_distribution<double>(0.0, 1.0)(rng()));
    }});
}

void grantDefaultPermissions(HostRegistry& r) {
    r.grant("console");
    r.grant("time");
    r.grant("random");
}

} // namespace pvm
