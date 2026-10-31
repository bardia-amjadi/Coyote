#include "ops.hpp"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace pvm {

std::string formatDouble(double v) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v < 0 ? "-Infinity" : "Infinity";
    char buf[40];
    // The shortest of 15, 16 or 17 significant digits that reads back as exactly the same double.
    for (int prec = 15; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof buf, "%.*g", prec, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    std::string s = buf;
    if (s.find_first_of(".en") == std::string::npos) s += ".0";  // "3" -> "3.0"; exponent forms are left alone
    return s;
}

void appendUtf8(std::string& out, long long cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool parseIntStrict(const std::string& s, long long& out) {
    size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) { neg = s[i] == '-'; ++i; }
    if (i == s.size() || s.size() - i > 10) return false;
    long long v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    if (neg) v = -v;
    if (v < INT_MIN || v > INT_MAX) return false;
    out = v;
    return true;
}

bool parseDoubleStrict(const std::string& s, double& out) {
    if (s.empty()) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E')) return false;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || std::isinf(v) || std::isnan(v)) return false;
    out = v;
    return true;
}

bool parseGuidStrict(const std::string& s, std::string& canonical) {
    if (s.size() != 36) return false;
    std::string r;
    for (size_t i = 0; i < 36; ++i) {
        char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
            r += '-';
        } else if (c >= '0' && c <= '9') {
            r += c;
        } else if (c >= 'a' && c <= 'f') {
            r += c;
        } else if (c >= 'A' && c <= 'F') {
            r += static_cast<char>(c - 'A' + 'a');
        } else {
            return false;
        }
    }
    canonical = r;
    return true;
}

std::string mapKeyText(const Value& k) {
    switch (k.kind) {
        case Type::Int: case Type::Char: case Type::Bool: return std::to_string(k.i);
        default: return k.s;
    }
}

std::string runtimeTypeName(const Value& v) {
    const Value* p = &v;
    if (p->kind == Type::Obj) {
        if (!p->boxed) return "null";
        p = p->boxed.get();
    }
    switch (p->kind) {
        case Type::Int: return "int";
        case Type::Bool: return "boolean";
        case Type::Char: return "char";
        case Type::Double: return "double";
        case Type::String: return "string";
        case Type::Guid: return "Guid";
        case Type::Array: case Type::Map: case Type::Class: return p->ref && p->ref->type ? typeName(*p->ref->type) : "null";
        default: return "?";
    }
}

std::string display(const Value& v) {
    const Value* p = &v;
    if (p->kind == Type::Obj) {
        if (!p->boxed) return "null";
        p = p->boxed.get();
    }
    if (p->isNull) return "null";
    switch (p->kind) {
        case Type::Int: return std::to_string(p->i);
        case Type::Bool: return p->i ? "true" : "false";
        case Type::Char: { std::string s; appendUtf8(s, p->i); return s; }
        case Type::Double: return formatDouble(p->d);
        case Type::String: case Type::Guid: return p->s;
        case Type::Array:
            if (!p->ref) return "null";
            return typeName(*p->ref->type) + "[" + std::to_string(p->ref->items.size()) + " items]";
        case Type::Map:
            if (!p->ref) return "null";
            return typeName(*p->ref->type) + "[" + std::to_string(p->ref->live) + " entries]";
        case Type::Class:
            if (!p->ref) return "null";
            return typeName(*p->ref->type) + " object";
        default: return "?";
    }
}

bool hasType(const Value& v, const Type& t) {
    const Value* p = &v;
    if (p->kind == Type::Obj) {
        if (!p->boxed) return false;
        p = p->boxed.get();
    }
    if (p->isNull) return false;
    if (p->kind != t.kind) return false;
    if (t.kind == Type::Array || t.kind == Type::Map || t.kind == Type::Class) return p->kind == t.kind && p->ref && p->ref->type && *p->ref->type == t;
    return true;
}

Value boxValue(const Value& inner) {
    if (inner.kind == Type::Obj) return inner;  // already an obj
    if (inner.isNull || ((inner.kind == Type::Array || inner.kind == Type::Map || inner.kind == Type::Class) && !inner.ref)) return Value::nullObj();
    Value o;
    o.kind = Type::Obj;
    o.boxed = std::make_shared<const Value>(inner);
    return o;
}

Value unboxTo(const Value& obj, const Type& t) {
    if (t.kind == Type::Obj) return obj;
    const bool nullable = t.kind == Type::Nullable;
    const Type& want = nullable ? *t.elem : t;
    if (!obj.boxed) {
        if (nullable) return Value::defaultOf(t);
        throw OpError{"cannot convert null to " + typeName(t)};
    }
    const Value& in = *obj.boxed;
    if (hasType(obj, want)) {
        Value v = in;
        v.isNull = false;
        return v;
    }
    if (want.kind == Type::Double && in.kind == Type::Int) return Value::ofDouble(static_cast<double>(in.i));  // implicit widening
    throw OpError{"cannot convert " + runtimeTypeName(obj) + " to " + typeName(t)};
}

bool objEquals(const Value& a, const Value& b) {
    if (!a.boxed || !b.boxed) return !a.boxed && !b.boxed;
    const Value& x = *a.boxed;
    const Value& y = *b.boxed;
    if (x.kind != y.kind) return false;
    switch (x.kind) {
        case Type::Int: case Type::Bool: case Type::Char: return x.i == y.i;
        case Type::Double: return x.d == y.d;
        case Type::String: case Type::Guid: return x.s == y.s;
        case Type::Array: case Type::Map: case Type::Class: return x.ref == y.ref;
        default: return false;
    }
}

namespace {

bool isNumber(const Value& v) { return v.kind == Type::Int || v.kind == Type::Double; }
double asDouble(const Value& v) { return v.kind == Type::Int ? static_cast<double>(v.i) : v.d; }

const char* opText(Op op) {
    switch (op) {
        case Op::DynAdd: return "+";
        case Op::DynSub: return "-";
        case Op::DynMul: return "*";
        case Op::DynDiv: return "/";
        case Op::DynRem: return "%";
        case Op::DynLt: return "<";
        case Op::DynLe: return "<=";
        case Op::DynGt: return ">";
        case Op::DynGe: return ">=";
        case Op::DynNeg: return "-";
        case Op::DynNot: return "!";
        default: return "?";
    }
}

[[noreturn]] void badOperands(Op op, const Value& a, const Value& b) {
    throw OpError{std::string("operator '") + opText(op) + "' cannot be applied to " + runtimeTypeName(a) + " and " + runtimeTypeName(b)};
}

Value checkedInt(long long v) {
    if (v < INT_MIN || v > INT_MAX) throw OpError{"integer overflow"};
    return Value::ofInt(v);
}

} // namespace

Value dynBinary(Op op, const Value& oa, const Value& ob) {
    // Equality accepts null and compares numbers across int/double.
    if (op == Op::DynEq || op == Op::DynNe) {
        bool eq;
        if (oa.boxed && ob.boxed && isNumber(*oa.boxed) && isNumber(*ob.boxed) && oa.boxed->kind != ob.boxed->kind)
            eq = asDouble(*oa.boxed) == asDouble(*ob.boxed);
        else
            eq = objEquals(oa, ob);
        return boxValue(Value::ofBool(op == Op::DynEq ? eq : !eq));
    }
    if (op == Op::DynAdd && ((oa.boxed && oa.boxed->kind == Type::String) || (ob.boxed && ob.boxed->kind == Type::String)))
        return boxValue(Value::ofString(display(oa) + display(ob)));  // string + anything concatenates

    if (!oa.boxed || !ob.boxed) badOperands(op, oa, ob);
    const Value& a = *oa.boxed;
    const Value& b = *ob.boxed;

    if (op == Op::DynLt || op == Op::DynLe || op == Op::DynGt || op == Op::DynGe) {
        int cmp;
        if (isNumber(a) && isNumber(b)) {
            double x = asDouble(a), y = asDouble(b);
            if (std::isnan(x) || std::isnan(y)) return boxValue(Value::ofBool(false));
            cmp = x < y ? -1 : (x > y ? 1 : 0);
        } else if (a.kind == Type::Char && b.kind == Type::Char) {
            cmp = a.i < b.i ? -1 : (a.i > b.i ? 1 : 0);
        } else if (a.kind == Type::String && b.kind == Type::String) {
            int c = a.s.compare(b.s);
            cmp = c < 0 ? -1 : (c > 0 ? 1 : 0);
        } else {
            badOperands(op, oa, ob);
        }
        bool r = op == Op::DynLt ? cmp < 0 : op == Op::DynLe ? cmp <= 0 : op == Op::DynGt ? cmp > 0 : cmp >= 0;
        return boxValue(Value::ofBool(r));
    }

    if (!isNumber(a) || !isNumber(b)) badOperands(op, oa, ob);
    if (a.kind == Type::Int && b.kind == Type::Int) {
        switch (op) {
            case Op::DynAdd: return boxValue(checkedInt(a.i + b.i));
            case Op::DynSub: return boxValue(checkedInt(a.i - b.i));
            case Op::DynMul: return boxValue(checkedInt(a.i * b.i));
            case Op::DynDiv:
                if (b.i == 0) throw OpError{"division by zero"};
                return boxValue(checkedInt(a.i / b.i));
            default:
                if (b.i == 0) throw OpError{"remainder by zero"};
                return boxValue(checkedInt(a.i % b.i));
        }
    }
    double x = asDouble(a), y = asDouble(b);
    switch (op) {
        case Op::DynAdd: return boxValue(Value::ofDouble(x + y));
        case Op::DynSub: return boxValue(Value::ofDouble(x - y));
        case Op::DynMul: return boxValue(Value::ofDouble(x * y));
        case Op::DynDiv: return boxValue(Value::ofDouble(x / y));
        default: return boxValue(Value::ofDouble(std::fmod(x, y)));
    }
}

Value dynUnary(Op op, const Value& oa) {
    if (oa.boxed) {
        const Value& a = *oa.boxed;
        if (op == Op::DynNeg && a.kind == Type::Int) return boxValue(checkedInt(-a.i));
        if (op == Op::DynNeg && a.kind == Type::Double) return boxValue(Value::ofDouble(-a.d));
        if (op == Op::DynNot && a.kind == Type::Bool) return boxValue(Value::ofBool(a.i == 0));
    }
    throw OpError{std::string("operator '") + opText(op) + "' cannot be applied to " + runtimeTypeName(oa)};
}

} // namespace pvm
