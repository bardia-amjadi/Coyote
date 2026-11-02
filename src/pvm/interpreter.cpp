#include "interpreter.hpp"

#include <climits>
#include <cmath>
#include <vector>

#include "ops.hpp"

namespace pvm {

namespace {

// Maximum depth of nested method calls. Deeper recursion is a runtime error, not a crash.
constexpr size_t kMaxCallDepth = 5000;

struct Frame {
    const Function* fn = nullptr;
    size_t pc = 0;
    size_t base = 0;            // operand-stack height when the frame was entered
    std::vector<Value> locals;  // parameters first, then declared locals
};

} // namespace

namespace {

std::string describeValue(const Module& m, const Value& v, int depth) {
    if (v.kind == Type::Obj) {
        if (!v.boxed) return "null";
        return describeValue(m, *v.boxed, depth);
    }
    if (v.isNull) return "null";
    switch (v.kind) {
        case Type::String: {
            std::string s = v.s.size() > 60 ? v.s.substr(0, 60) + "..." : v.s;
            return "\"" + s + "\"";
        }
        case Type::Char: { std::string s; appendUtf8(s, v.i); return "'" + s + "'"; }
        case Type::Array:
        case Type::Map:
        case Type::Class:
        case Type::Fn: {
            const Obj* o = v.ref;
            if (!o) return "null";
            if (v.kind == Type::Fn) return "function #" + std::to_string(o->func) + " (" + std::to_string(o->items.size()) + " captured)";
            if (depth <= 0) return v.kind == Type::Class ? "{...}" : "[...]";
            constexpr size_t kShown = 8;
            std::string r;
            if (v.kind == Type::Class) {
                const ClassDecl* cd = nullptr;
                if (o->type)
                    for (const ClassDecl& c : m.classes)
                        if (c.name == o->type->name) cd = &c;
                r = (cd ? cd->name : std::string("object")) + " {";
                for (size_t i = 0; i < o->items.size() && i < kShown; ++i) {
                    if (i) r += ", ";
                    r += (cd && i < cd->fields.size() ? cd->fields[i].name : std::to_string(i)) + ": " + describeValue(m, o->items[i], depth - 1);
                }
                if (o->items.size() > kShown) r += ", ...";
                return r + "}";
            }
            if (v.kind == Type::Map) {
                r = "{";
                size_t shown = 0;
                for (size_t i = 0; i < o->items.size() && shown < kShown; ++i) {
                    if (!o->alive[i]) continue;
                    if (shown++) r += ", ";
                    r += describeValue(m, o->keys[i], 0) + ": " + describeValue(m, o->items[i], depth - 1);
                }
                if (o->live > kShown) r += ", ...";
                return r + "} (" + std::to_string(o->live) + ")";
            }
            r = "[";
            for (size_t i = 0; i < o->items.size() && i < kShown; ++i) {
                if (i) r += ", ";
                r += describeValue(m, o->items[i], depth - 1);
            }
            if (o->items.size() > kShown) r += ", ...";
            return r + "] (" + std::to_string(o->items.size()) + ")";
        }
        default: return display(v);
    }
}

} // namespace

void run(const Module& m, const HostRegistry& registry, std::ostream& out, Heap& heap, DebugHook* hook) {
    HostContext ctx = registry.context(out);
    std::vector<Value> stack;
    std::vector<Frame> frames;
    std::vector<Value> globals;  // version 5; GC roots
    for (const GlobalDecl& g : m.globals) globals.push_back(Value::defaultOf(g.type));

    auto enter = [&](const Function& fn, size_t argc) {
        Frame f;
        f.fn = &fn;
        f.locals.reserve(argc + fn.locals.size());
        for (size_t i = stack.size() - argc; i < stack.size(); ++i) f.locals.push_back(std::move(stack[i]));
        stack.resize(stack.size() - argc);
        for (Type t : fn.locals) f.locals.push_back(Value::defaultOf(t));
        f.base = stack.size();
        frames.push_back(std::move(f));
    };
    enter(m.funcs[static_cast<size_t>(m.entryFunc)], 0);

    // GC roots: everything reachable without going through the heap.
    RootsFn roots = [&](MarkFn mark) {
        for (const Value& v : globals) mark(v);
        for (const Value& v : stack) mark(v);
        for (const Frame& f : frames)
            for (const Value& v : f.locals) mark(v);
    };

    const Instr* current = nullptr;  // for reporting heap errors at the right source line

    struct View : DebugView {
        const Module& m;
        const std::vector<Frame>& frames;
        const std::vector<Value>& gl;
        View(const Module& mm, const std::vector<Frame>& f, const std::vector<Value>& g) : m(mm), frames(f), gl(g) {}
        std::vector<DebugFrame> stack() const override {
            std::vector<DebugFrame> r;
            for (size_t i = frames.size(); i-- > 0;) {
                const Frame& f = frames[i];
                // The innermost frame is at its pending instruction; an outer frame already advanced past its call.
                size_t pc = f.pc > 0 && i + 1 < frames.size() ? f.pc - 1 : f.pc;
                if (pc >= f.fn->code.size()) pc = f.fn->code.size() - 1;
                const Instr& in = f.fn->code[pc];
                r.push_back(DebugFrame{f.fn->name, m.fileName(in.srcFile), in.srcLine});
            }
            return r;
        }
        std::vector<DebugVar> variables(size_t frame) const override {
            std::vector<DebugVar> r;
            if (frame >= frames.size()) return r;
            const Frame& f = frames[frames.size() - 1 - frame];
            for (size_t i = 0; i < f.locals.size(); ++i) {
                auto n = f.fn->varNames.find(static_cast<int>(i));
                if (n != f.fn->varNames.end() && !n->second.empty() && n->second[0] == '$') continue;  // hidden compiler local
                const Type& t = i < f.fn->sig.params.size() ? f.fn->sig.params[i] : f.fn->locals[i - f.fn->sig.params.size()];
                r.push_back(DebugVar{n != f.fn->varNames.end() ? n->second : "#" + std::to_string(i), typeName(t), describeValue(m, f.locals[i], 3)});
            }
            return r;
        }
        std::vector<DebugVar> globals() const override {
            std::vector<DebugVar> r;
            for (size_t i = 0; i < globals_().size() && i < m.globals.size(); ++i)
                r.push_back(DebugVar{m.globals[i].name, typeName(m.globals[i].type), describeValue(m, globals_()[i], 3)});
            return r;
        }
        const std::vector<Value>& globals_() const { return gl; }
    } view(m, frames, globals);
    const Function* lastFn = nullptr;
    int lastLine = -1, lastFile = -1;
    size_t lastDepth = 0;

    try {
    while (!frames.empty()) {
        Frame& fr = frames.back();
        const Instr& in = fr.fn->code[fr.pc++];
        current = &in;
        if (hook && in.srcLine > 0 && (fr.fn != lastFn || in.srcLine != lastLine || in.srcFile != lastFile || frames.size() != lastDepth)) {
            lastFn = fr.fn;
            lastLine = in.srcLine;
            lastFile = in.srcFile;
            lastDepth = frames.size();
            --fr.pc;  // the stack view reports this instruction as pending
            bool go = hook->onLine(view, m.fileName(in.srcFile), in.srcLine, frames.size());
            if (!go) return;
            ++frames.back().pc;
        }

        auto rtError = [&](const std::string& msg) { return RuntimeError{m.fileName(in.srcFile), in.srcLine, msg}; };
        auto popInt = [&]() { long long v = stack.back().i; stack.pop_back(); return v; };
        auto checked = [&](long long v) {
            if (v < INT_MIN || v > INT_MAX) throw rtError("integer overflow");
            return Value::ofInt(v);
        };

        switch (in.op) {
            case Op::PushStr: stack.push_back(Value::ofString(in.str)); break;
            case Op::PushInt: stack.push_back(Value::ofInt(in.ival)); break;
            case Op::PushBool: stack.push_back(Value::ofBool(in.ival != 0)); break;
            case Op::Load: stack.push_back(fr.locals[static_cast<size_t>(in.ival)]); break;
            case Op::Store:
                fr.locals[static_cast<size_t>(in.ival)] = std::move(stack.back());
                stack.pop_back();
                break;
            case Op::Pop: stack.pop_back(); break;

            case Op::AddI: { long long b = popInt(), a = popInt(); stack.push_back(checked(a + b)); break; }
            case Op::SubI: { long long b = popInt(), a = popInt(); stack.push_back(checked(a - b)); break; }
            case Op::MulI: { long long b = popInt(), a = popInt(); stack.push_back(checked(a * b)); break; }
            case Op::DivI:
            case Op::RemI: {
                long long b = popInt(), a = popInt();
                if (b == 0) throw rtError(in.op == Op::DivI ? "division by zero" : "remainder by zero");
                // Truncates toward zero; the remainder has the sign of the dividend. (INT_MIN / -1
                // does not fit in 32 bits and is reported as overflow.)
                stack.push_back(checked(in.op == Op::DivI ? a / b : a % b));
                break;
            }
            case Op::NegI: { long long a = popInt(); stack.push_back(checked(-a)); break; }

            case Op::Concat: {
                std::string b = std::move(stack.back().s);
                stack.pop_back();
                stack.back().s += b;
                break;
            }
            case Op::StrInt: { long long a = popInt(); stack.push_back(Value::ofString(std::to_string(a))); break; }
            case Op::StrBool: { long long a = popInt(); stack.push_back(Value::ofString(a ? "true" : "false")); break; }

            case Op::EqI: case Op::NeI: case Op::LtI: case Op::LeI: case Op::GtI: case Op::GeI:
            case Op::EqB: case Op::NeB: {
                long long b = popInt(), a = popInt();
                bool r = false;
                switch (in.op) {
                    case Op::EqI: case Op::EqB: r = a == b; break;
                    case Op::NeI: case Op::NeB: r = a != b; break;
                    case Op::LtI: r = a < b; break;
                    case Op::LeI: r = a <= b; break;
                    case Op::GtI: r = a > b; break;
                    default: r = a >= b; break;
                }
                stack.push_back(Value::ofBool(r));
                break;
            }
            case Op::EqS: case Op::NeS: {
                std::string b = std::move(stack.back().s);
                stack.pop_back();
                bool eq = stack.back().s == b;
                stack.pop_back();
                stack.push_back(Value::ofBool(in.op == Op::EqS ? eq : !eq));
                break;
            }
            case Op::Not: { long long a = popInt(); stack.push_back(Value::ofBool(a == 0)); break; }

            case Op::Jump: fr.pc = static_cast<size_t>(in.target); break;
            case Op::JumpFalse:
                if (popInt() == 0) fr.pc = static_cast<size_t>(in.target);
                break;

            case Op::Call: {
                if (frames.size() >= kMaxCallDepth)
                    throw rtError("call stack overflow (more than " + std::to_string(kMaxCallDepth) +
                                  " nested calls; infinite recursion?)");
                enter(m.funcs[static_cast<size_t>(in.target)], static_cast<size_t>(in.ival));
                break;  // 'fr' may now dangle; the loop re-fetches frames.back()
            }
            case Op::MkClosure: {
                const size_t ncap = static_cast<size_t>(in.ival);
                Obj* o = heap.allocate(std::shared_ptr<const Type>(), ncap, Value::none(), roots);  // captured values stay rooted on the stack
                for (size_t i = 0; i < ncap; ++i) o->items[i] = std::move(stack[stack.size() - ncap + i]);
                stack.resize(stack.size() - ncap);
                o->func = in.target;
                stack.push_back(Value::ofClosure(o));
                break;
            }
            case Op::CallFn: {
                const size_t n = static_cast<size_t>(in.ival);
                Obj* c = stack[stack.size() - n - 1].ref;
                if (!c) throw rtError("null reference: cannot call a null function value");
                if (frames.size() >= kMaxCallDepth)
                    throw rtError("call stack overflow (more than " + std::to_string(kMaxCallDepth) +
                                  " nested calls; infinite recursion?)");
                std::vector<Value> args(std::make_move_iterator(stack.end() - static_cast<std::ptrdiff_t>(n)), std::make_move_iterator(stack.end()));
                stack.resize(stack.size() - n - 1);
                for (const Value& v : c->items) stack.push_back(v);
                for (Value& v : args) stack.push_back(std::move(v));
                enter(m.funcs[static_cast<size_t>(c->func)], c->items.size() + n);
                break;
            }
            case Op::HostCall: {
                const HostDecl& decl = m.hosts[static_cast<size_t>(in.host)];
                const HostFunction* hf = registry.find(decl.name);  // presence checked by verify()
                if (!registry.isGranted(hf->permission))
                    throw rtError("host function '" + decl.name + "' needs the '" + hf->permission +
                                  "' permission, which was not granted (run with --allow " + hf->permission + ", or use `coyote run`)");
                size_t argc = static_cast<size_t>(in.ival);
                std::vector<Value> args(stack.end() - static_cast<std::ptrdiff_t>(argc), stack.end());
                stack.resize(stack.size() - argc);
                try {
                    Value result = hf->fn(args, ctx);
                    if (decl.sig.ret != Type::Void) stack.push_back(std::move(result));
                } catch (const HostError& e) {
                    throw rtError("host function '" + decl.name + "' failed: " + e.message);
                }
                break;
            }
            case Op::NewArray: {
                long long n = stack.back().i;
                if (n < 0) throw rtError("negative array size " + std::to_string(n));
                Obj* o = heap.allocate(in.objType, static_cast<size_t>(n), Value::defaultOf(in.type), roots);  // operand stays rooted
                stack.pop_back();
                stack.push_back(Value::ofArray(o));
                break;
            }
            case Op::ArrayLit: {
                size_t n = static_cast<size_t>(in.ival);
                Obj* o = heap.allocate(in.objType, n, Value::none(), roots);  // elements are still on the stack, hence rooted
                for (size_t k = 0; k < n; ++k) o->items[k] = std::move(stack[stack.size() - n + k]);
                stack.resize(stack.size() - n);
                stack.push_back(Value::ofArray(o));
                break;
            }
            case Op::ALoad: {
                long long idx = popInt();
                Obj* o = stack.back().ref;
                stack.pop_back();
                if (!o) throw rtError("null array reference");
                if (idx < 0 || idx >= static_cast<long long>(o->items.size()))
                    throw rtError("array index " + std::to_string(idx) + " is out of range for an array of length " +
                                  std::to_string(o->items.size()));
                stack.push_back(o->items[static_cast<size_t>(idx)]);
                break;
            }
            case Op::AStore: {
                Value v = std::move(stack.back());
                stack.pop_back();
                long long idx = popInt();
                Obj* o = stack.back().ref;
                stack.pop_back();
                if (!o) throw rtError("null array reference");
                if (idx < 0 || idx >= static_cast<long long>(o->items.size()))
                    throw rtError("array index " + std::to_string(idx) + " is out of range for an array of length " +
                                  std::to_string(o->items.size()));
                o->items[static_cast<size_t>(idx)] = std::move(v);
                break;
            }
            case Op::ALen: {
                Obj* o = stack.back().ref;
                stack.pop_back();
                if (!o) throw rtError("null array reference");
                stack.push_back(Value::ofInt(static_cast<long long>(o->items.size())));
                break;
            }
            case Op::APush: {
                Obj* o = stack[stack.size() - 2].ref;
                if (!o) throw rtError("null array reference");
                heap.reserve(1, roots);  // may collect; the array and the value are still on the stack
                Value v = std::move(stack.back());
                stack.pop_back();
                stack.pop_back();
                o->items.push_back(std::move(v));
                break;
            }
            case Op::ACopy: {
                Obj* src = stack.back().ref;
                if (!src) throw rtError("null array reference");
                Obj* o = heap.allocate(src->type, src->items.size(), Value::none(), roots);  // src is still rooted
                o->items = src->items;
                stack.pop_back();
                stack.push_back(Value::ofArray(o));
                break;
            }
            case Op::EqRef:
            case Op::NeRef: {
                Obj* b = stack.back().ref;
                stack.pop_back();
                Obj* a = stack.back().ref;
                stack.pop_back();
                stack.push_back(Value::ofBool(in.op == Op::EqRef ? a == b : a != b));
                break;
            }

            // ---- double, char, Guid ----
            case Op::PushDbl: stack.push_back(Value::ofDouble(in.dval)); break;
            case Op::PushChr: stack.push_back(Value::ofChar(in.ival)); break;
            case Op::AddD: case Op::SubD: case Op::MulD: case Op::DivD: case Op::RemD: {
                double b = stack.back().d; stack.pop_back();
                double a = stack.back().d; stack.pop_back();
                double r = in.op == Op::AddD ? a + b : in.op == Op::SubD ? a - b : in.op == Op::MulD ? a * b
                         : in.op == Op::DivD ? a / b : std::fmod(a, b);
                stack.push_back(Value::ofDouble(r));
                break;
            }
            case Op::NegD: stack.back().d = -stack.back().d; break;
            case Op::EqD: case Op::NeD: case Op::LtD: case Op::LeD: case Op::GtD: case Op::GeD: {
                double b = stack.back().d; stack.pop_back();
                double a = stack.back().d; stack.pop_back();
                bool r = in.op == Op::EqD ? a == b : in.op == Op::NeD ? a != b : in.op == Op::LtD ? a < b
                       : in.op == Op::LeD ? a <= b : in.op == Op::GtD ? a > b : a >= b;
                stack.push_back(Value::ofBool(r));
                break;
            }
            case Op::EqC: case Op::NeC: case Op::LtC: case Op::LeC: case Op::GtC: case Op::GeC: {
                long long b = popInt(), a = popInt();
                bool r = in.op == Op::EqC ? a == b : in.op == Op::NeC ? a != b : in.op == Op::LtC ? a < b
                       : in.op == Op::LeC ? a <= b : in.op == Op::GtC ? a > b : a >= b;
                stack.push_back(Value::ofBool(r));
                break;
            }
            case Op::EqG: case Op::NeG: {
                std::string b = std::move(stack.back().s);
                stack.pop_back();
                bool eq = stack.back().s == b;
                stack.pop_back();
                stack.push_back(Value::ofBool(in.op == Op::EqG ? eq : !eq));
                break;
            }
            case Op::I2D: { long long a = popInt(); stack.push_back(Value::ofDouble(static_cast<double>(a))); break; }
            case Op::D2I: {
                double d = stack.back().d;
                stack.pop_back();
                if (!(d > -2147483649.0 && d < 2147483648.0)) throw rtError("cannot convert " + formatDouble(d) + " to int (out of range)");
                stack.push_back(Value::ofInt(static_cast<long long>(d)));  // truncates toward zero
                break;
            }
            case Op::C2I: { long long a = popInt(); stack.push_back(Value::ofInt(a)); break; }
            case Op::I2C: {
                long long a = popInt();
                if (a < 0 || a > 0x10FFFF || (a >= 0xD800 && a <= 0xDFFF)) throw rtError("invalid character code " + std::to_string(a));
                stack.push_back(Value::ofChar(a));
                break;
            }
            case Op::StrDbl: { double d = stack.back().d; stack.pop_back(); stack.push_back(Value::ofString(formatDouble(d))); break; }
            case Op::StrChr: { long long a = popInt(); std::string t; appendUtf8(t, a); stack.push_back(Value::ofString(std::move(t))); break; }
            case Op::StrGuid: { std::string g = std::move(stack.back().s); stack.pop_back(); stack.push_back(Value::ofString(std::move(g))); break; }
            case Op::ParseInt: {
                std::string t = std::move(stack.back().s);
                stack.pop_back();
                long long v;
                if (!parseIntStrict(t, v)) throw rtError("cannot convert \"" + t + "\" to int");
                stack.push_back(Value::ofInt(v));
                break;
            }
            case Op::ParseDbl: {
                std::string t = std::move(stack.back().s);
                stack.pop_back();
                double v;
                if (!parseDoubleStrict(t, v)) throw rtError("cannot convert \"" + t + "\" to double");
                stack.push_back(Value::ofDouble(v));
                break;
            }
            case Op::ParseGuid: {
                std::string t = std::move(stack.back().s);
                stack.pop_back();
                std::string canon;
                if (!parseGuidStrict(t, canon)) throw rtError("cannot convert \"" + t + "\" to Guid (expected xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)");
                stack.push_back(Value::ofGuid(std::move(canon)));
                break;
            }

            // ---- nullable types ----
            case Op::PushNull: stack.push_back(Value::defaultOf(in.type)); break;
            case Op::Wrap: {
                Value& v = stack.back();
                v.isNull = (v.kind == Type::Array || v.kind == Type::Map || v.kind == Type::Class) && !v.ref;
                break;
            }
            case Op::Unwrap: {
                Value& v = stack.back();
                if (v.isNull) throw rtError("null value: cannot unwrap null (use the null-coalescing operator or check for null first)");
                break;
            }
            case Op::IsNull: {
                bool n = stack.back().isNull || (stack.back().kind == Type::Obj && !stack.back().boxed);
                stack.pop_back();
                stack.push_back(Value::ofBool(n));
                break;
            }
            case Op::Dup: stack.push_back(stack.back()); break;

            // ---- maps ----
            case Op::NewMap: stack.push_back(Value::ofMap(heap.allocateMap(in.objType, roots))); break;
            case Op::MapGet: case Op::MapTry: case Op::MapHas: case Op::MapDel: {
                Value key = std::move(stack.back());
                stack.pop_back();
                Obj* o = stack.back().ref;
                if (!o) throw rtError("null map reference");
                auto it = o->index.find(mapKeyText(key));
                bool found = it != o->index.end();
                stack.pop_back();
                if (in.op == Op::MapHas) { stack.push_back(Value::ofBool(found)); break; }
                if (in.op == Op::MapDel) {
                    if (found) {
                        size_t i = it->second;
                        o->index.erase(it);
                        o->alive[i] = 0;
                        o->items[i] = Value::none();
                        --o->live;
                        if (o->items.size() > 32 && o->items.size() - o->live > o->live) heap.compactMap(o);
                    }
                    stack.push_back(Value::ofBool(found));
                    break;
                }
                if (in.op == Op::MapGet) {
                    if (!found) throw rtError("key not found: " + display(key));
                    stack.push_back(o->items[it->second]);
                    break;
                }
                const Type& vt = *o->type->elem;  // MapTry
                if (found) {
                    Value v = o->items[it->second];
                    if (vt.kind != Type::Nullable && vt.kind != Type::Obj) v.isNull = false;
                    stack.push_back(std::move(v));
                } else {
                    stack.push_back(Value::defaultOf(vt.kind == Type::Nullable || vt.kind == Type::Obj ? vt : Type::nullableOf(vt)));
                }
                break;
            }
            case Op::MapSet: {
                Obj* o = stack[stack.size() - 3].ref;
                if (!o) throw rtError("null map reference");
                std::string kt = mapKeyText(stack[stack.size() - 2]);
                auto it = o->index.find(kt);
                if (it == o->index.end()) heap.reserve(1, roots);  // may collect; operands stay on the stack
                Value v = std::move(stack.back());
                stack.pop_back();
                Value key = std::move(stack.back());
                stack.pop_back();
                stack.pop_back();
                if (it != o->index.end()) {
                    o->items[it->second] = std::move(v);
                } else {
                    o->index.emplace(std::move(kt), o->items.size());
                    o->keys.push_back(std::move(key));
                    o->items.push_back(std::move(v));
                    o->alive.push_back(1);
                    ++o->live;
                }
                break;
            }
            case Op::MapLen: {
                Obj* o = stack.back().ref;
                if (!o) throw rtError("null map reference");
                stack.pop_back();
                stack.push_back(Value::ofInt(static_cast<long long>(o->live)));
                break;
            }
            case Op::MapKeys: case Op::MapValues: {
                Obj* mp = stack.back().ref;
                if (!mp) throw rtError("null map reference");
                bool keys = in.op == Op::MapKeys;
                auto at = std::make_shared<const Type>(Type::arrayOf(keys ? *mp->type->key : *mp->type->elem));
                Obj* o = heap.allocate(at, mp->live, Value::none(), roots);  // the map is still on the stack
                size_t k = 0;
                for (size_t i = 0; i < mp->items.size(); ++i)
                    if (mp->alive[i]) o->items[k++] = keys ? mp->keys[i] : mp->items[i];
                stack.pop_back();
                stack.push_back(Value::ofArray(o));
                break;
            }

            // ---- classes and globals ----
            case Op::NewObj: {
                const ClassDecl& cd = m.classes[static_cast<size_t>(in.cls)];
                Obj* o = heap.allocate(in.objType, cd.fields.size(), Value::none(), roots);
                for (size_t i = 0; i < cd.fields.size(); ++i) o->items[i] = Value::defaultOf(cd.fields[i].type);
                stack.push_back(Value::ofObject(o));
                break;
            }
            case Op::GetField: {
                Obj* o = stack.back().ref;
                if (!o) throw rtError("null reference: cannot read field '" + in.str.substr(in.str.rfind('.') + 1) + "' of a null object");
                Value v = o->items[static_cast<size_t>(in.ival)];
                stack.back() = std::move(v);
                break;
            }
            case Op::SetField: {
                Value v = std::move(stack.back());
                stack.pop_back();
                Obj* o = stack.back().ref;
                if (!o) throw rtError("null reference: cannot assign field '" + in.str.substr(in.str.rfind('.') + 1) + "' of a null object");
                o->items[static_cast<size_t>(in.ival)] = std::move(v);
                stack.pop_back();
                break;
            }
            case Op::GetGlobal: stack.push_back(globals[static_cast<size_t>(in.target)]); break;
            case Op::SetGlobal:
                globals[static_cast<size_t>(in.target)] = std::move(stack.back());
                stack.pop_back();
                break;

            // ---- obj and dynamic operations ----
            case Op::Box: stack.back() = boxValue(stack.back()); break;
            case Op::Unbox: {
                try {
                    Value r = unboxTo(stack.back(), in.type);
                    stack.back() = std::move(r);
                } catch (const OpError& e) {
                    throw rtError(e.message);
                }
                break;
            }
            case Op::IsType: { bool r = hasType(stack.back(), in.type); stack.back() = Value::ofBool(r); break; }
            case Op::ToStrObj: { std::string t = display(stack.back()); stack.back() = Value::ofString(std::move(t)); break; }
            case Op::EqObj: case Op::NeObj: {
                Value b = std::move(stack.back());
                stack.pop_back();
                bool eq = objEquals(stack.back(), b);
                stack.back() = Value::ofBool(in.op == Op::EqObj ? eq : !eq);
                break;
            }
            case Op::DynAdd: case Op::DynSub: case Op::DynMul: case Op::DynDiv: case Op::DynRem:
            case Op::DynEq: case Op::DynNe: case Op::DynLt: case Op::DynLe: case Op::DynGt: case Op::DynGe: {
                Value b = std::move(stack.back());
                stack.pop_back();
                try {
                    Value r = dynBinary(in.op, stack.back(), b);
                    stack.back() = std::move(r);
                } catch (const OpError& e) {
                    throw rtError(e.message);
                }
                break;
            }
            case Op::DynNeg: case Op::DynNot: {
                try {
                    Value r = dynUnary(in.op, stack.back());
                    stack.back() = std::move(r);
                } catch (const OpError& e) {
                    throw rtError(e.message);
                }
                break;
            }
            case Op::DynIndex: {
                Value idx = std::move(stack.back());
                stack.pop_back();
                const Value& c = stack.back();
                if (!c.boxed) throw rtError("null reference: cannot index null");
                const Value& inner = *c.boxed;
                Obj* o = inner.ref;
                if (inner.kind == Type::Array && o) {
                    if (!idx.boxed || idx.boxed->kind != Type::Int) throw rtError("array index must be int but got " + runtimeTypeName(idx));
                    long long i = idx.boxed->i;
                    if (i < 0 || i >= static_cast<long long>(o->items.size()))
                        throw rtError("array index " + std::to_string(i) + " is out of range for an array of length " + std::to_string(o->items.size()));
                    Value r = boxValue(o->items[static_cast<size_t>(i)]);
                    stack.back() = std::move(r);
                } else if (inner.kind == Type::Map && o) {
                    Value key;
                    try { key = unboxTo(idx, *o->type->key); } catch (const OpError& e) { throw rtError("map key: " + e.message); }
                    auto it = o->index.find(mapKeyText(key));
                    if (it == o->index.end()) throw rtError("key not found: " + display(key));
                    Value r = boxValue(o->items[it->second]);
                    stack.back() = std::move(r);
                } else {
                    throw rtError("cannot index a value of type " + runtimeTypeName(c));
                }
                break;
            }
            case Op::DynSetIndex: {
                Value val = std::move(stack.back());
                stack.pop_back();
                Value idx = std::move(stack.back());
                stack.pop_back();
                const Value c = stack.back();  // keep a copy: the container stays rooted via the copy below
                if (!c.boxed) throw rtError("null reference: cannot index null");
                const Value& inner = *c.boxed;
                Obj* o = inner.ref;
                try {
                    if (inner.kind == Type::Array && o) {
                        if (!idx.boxed || idx.boxed->kind != Type::Int) throw rtError("array index must be int but got " + runtimeTypeName(idx));
                        long long i = idx.boxed->i;
                        if (i < 0 || i >= static_cast<long long>(o->items.size()))
                            throw rtError("array index " + std::to_string(i) + " is out of range for an array of length " + std::to_string(o->items.size()));
                        o->items[static_cast<size_t>(i)] = unboxTo(val, *o->type->elem);
                    } else if (inner.kind == Type::Map && o) {
                        Value key = unboxTo(idx, *o->type->key);
                        Value v = unboxTo(val, *o->type->elem);
                        std::string kt = mapKeyText(key);
                        auto it = o->index.find(kt);
                        if (it != o->index.end()) {
                            o->items[it->second] = std::move(v);
                        } else {
                            heap.reserve(1, roots);  // the container is still on the stack
                            o->index.emplace(std::move(kt), o->items.size());
                            o->keys.push_back(std::move(key));
                            o->items.push_back(std::move(v));
                            o->alive.push_back(1);
                            ++o->live;
                        }
                    } else {
                        throw rtError("cannot index a value of type " + runtimeTypeName(c));
                    }
                } catch (const OpError& e) {
                    throw rtError(e.message);
                }
                stack.pop_back();
                break;
            }
            case Op::DynMember: {
                const Value& c = stack.back();
                if (!c.boxed) throw rtError("null reference: cannot read property '" + in.str + "' of null");
                const Value& inner = *c.boxed;
                long long n;
                if (in.str == "length" && inner.kind == Type::Array && inner.ref) n = static_cast<long long>(inner.ref->items.size());
                else if (in.str == "length" && inner.kind == Type::Map && inner.ref) n = static_cast<long long>(inner.ref->live);
                else if (in.str == "length" && inner.kind == Type::String) n = static_cast<long long>(inner.s.size());
                else throw rtError(runtimeTypeName(c) + " has no property '" + in.str + "'");
                stack.back() = boxValue(Value::ofInt(n));
                break;
            }
            case Op::DynInvoke: {
                size_t argc = static_cast<size_t>(in.ival);
                std::vector<Value> args(stack.end() - static_cast<std::ptrdiff_t>(argc), stack.end());
                const Value c = stack[stack.size() - argc - 1];  // receiver (stays rooted by the stack until the end)
                if (!c.boxed) throw rtError("null reference: cannot call '" + in.str + "' on null");
                const Value& inner = *c.boxed;
                Obj* o = inner.ref;
                auto expectArgs = [&](size_t n) {
                    if (argc != n) throw rtError("method '" + in.str + "' of " + runtimeTypeName(c) + " expects " + std::to_string(n) + " argument(s) but got " + std::to_string(argc));
                };
                Value result = Value::nullObj();
                try {
                    if (inner.kind == Type::Array && o && in.str == "push") {
                        expectArgs(1);
                        Value v = unboxTo(args[0], *o->type->elem);
                        heap.reserve(1, roots);
                        o->items.push_back(std::move(v));
                    } else if (inner.kind == Type::Array && o && in.str == "copy") {
                        expectArgs(0);
                        Obj* n = heap.allocate(o->type, o->items.size(), Value::none(), roots);
                        n->items = o->items;
                        result = boxValue(Value::ofArray(n));
                    } else if (inner.kind == Type::Map && o && in.str == "containsKey") {
                        expectArgs(1);
                        Value key = unboxTo(args[0], *o->type->key);
                        result = boxValue(Value::ofBool(o->index.count(mapKeyText(key)) != 0));
                    } else if (inner.kind == Type::Map && o && in.str == "get") {
                        expectArgs(1);
                        Value key = unboxTo(args[0], *o->type->key);
                        auto it = o->index.find(mapKeyText(key));
                        if (it != o->index.end()) result = boxValue(o->items[it->second]);
                    } else if (inner.kind == Type::Map && o && in.str == "remove") {
                        expectArgs(1);
                        Value key = unboxTo(args[0], *o->type->key);
                        auto it = o->index.find(mapKeyText(key));
                        bool found = it != o->index.end();
                        if (found) {
                            size_t i = it->second;
                            o->index.erase(it);
                            o->alive[i] = 0;
                            o->items[i] = Value::none();
                            --o->live;
                            if (o->items.size() > 32 && o->items.size() - o->live > o->live) heap.compactMap(o);
                        }
                        result = boxValue(Value::ofBool(found));
                    } else if (inner.kind == Type::Map && o && (in.str == "keys" || in.str == "values")) {
                        expectArgs(0);
                        bool keys = in.str == "keys";
                        auto at = std::make_shared<const Type>(Type::arrayOf(keys ? *o->type->key : *o->type->elem));
                        Obj* n = heap.allocate(at, o->live, Value::none(), roots);
                        size_t k = 0;
                        for (size_t i = 0; i < o->items.size(); ++i)
                            if (o->alive[i]) n->items[k++] = keys ? o->keys[i] : o->items[i];
                        result = boxValue(Value::ofArray(n));
                    } else {
                        throw rtError(runtimeTypeName(c) + " has no method '" + in.str + "'");
                    }
                } catch (const OpError& e) {
                    throw rtError(e.message);
                }
                stack.resize(stack.size() - argc - 1);
                stack.push_back(std::move(result));
                break;
            }

            case Op::Ret: {
                bool hasValue = fr.fn->sig.ret != Type::Void;
                Value result;
                if (hasValue) {
                    result = std::move(stack.back());
                    stack.pop_back();
                }
                stack.resize(fr.base);
                frames.pop_back();
                if (hasValue) stack.push_back(std::move(result));
                break;
            }
        }
    }
    } catch (const HeapError& e) {
        throw RuntimeError{m.fileName(current ? current->srcFile : 0), current ? current->srcLine : 0, e.message};
    }
}

} // namespace pvm
