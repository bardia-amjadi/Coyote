#include "verifier.hpp"

#include <deque>
#include <functional>
#include <map>

namespace pvm {

namespace {

[[noreturn]] void fail(const std::string& file, int line, const std::string& msg) {
    throw MlError{"verification error", file, line, msg};
}

// Operand-stack state used during verification. A state is a persistent (immutable, shared)
// linked stack: pushing allocates one node that points at the previous state, and popping just
// follows `parent`. Every instruction therefore costs O(1) extra memory regardless of how deep
// the stack is, so verification memory is linear in program size. (Copying a full stack per
// instruction would be quadratic: a long run of pushes could exhaust memory.)
struct SNode {
    Type type;
    const SNode* parent;
    size_t depth;
};
using SRef = const SNode*;  // nullptr is the empty stack

size_t sz(SRef s) { return s ? s->depth : 0; }

// The element `fromTop` places below the top (1 = top). The caller has checked the size.
const Type& at(SRef s, size_t fromTop) {
    while (--fromTop) s = s->parent;
    return s->type;
}

SRef drop(SRef s, size_t n) {
    while (n--) s = s->parent;
    return s;
}

// The top n element types, bottom to top. The caller has checked the size.
std::vector<const Type*> topTypes(SRef s, size_t n) {
    std::vector<const Type*> v(n);
    for (size_t j = 0; j < n; ++j) {
        v[n - 1 - j] = &s->type;
        s = s->parent;
    }
    return v;
}

bool sameStack(SRef a, SRef b) {
    while (a != b) {  // shared tails compare equal immediately
        if (!a || !b || a->depth != b->depth || a->type != b->type) return false;
        a = a->parent;
        b = b->parent;
    }
    return true;
}

// Shows at most the top 12 entries so error messages stay short for very deep stacks.
std::string stackStr(SRef s) {
    constexpr size_t kShown = 12;
    size_t total = sz(s), n = total < kShown ? total : kShown;
    std::string r = "(";
    if (total > n) r += "..., ";
    auto v = topTypes(s, n);
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) r += ", ";
        r += typeName(*v[i]);
    }
    return r + ")";
}

struct Ctx {
    Module& m;
    const std::map<std::string, int>& classIndex;
    const std::map<std::string, int>& globalIndex;
    const std::map<std::string, int>& hostIndex;
    const std::map<std::string, int>& funcIndex;
    const std::string& file;
};

// Resolves names and indices inside one function and checks static operands. Applies to every
// instruction, reachable or not.
void resolve(Ctx& c, Function& f, size_t nslots) {
    for (Instr& in : f.code) {
        switch (in.op) {
            case Op::HostCall: {
                auto it = c.hostIndex.find(in.str);
                if (it == c.hostIndex.end())
                    fail(c.file, in.mlLine, "hostcall to undeclared host function '" + in.str + "'");
                in.host = it->second;
                size_t argc = c.m.hosts[static_cast<size_t>(in.host)].sig.params.size();
                if (static_cast<size_t>(in.ival) != argc)
                    fail(c.file, in.mlLine, "'" + in.str + "' takes " + std::to_string(argc) +
                                                " argument(s) but the hostcall passes " + std::to_string(in.ival));
                break;
            }
            case Op::Call: {
                auto it = c.funcIndex.find(in.str);
                if (it == c.funcIndex.end())
                    fail(c.file, in.mlLine, "call to undefined function '" + in.str + "'");
                in.target = it->second;
                size_t argc = c.m.funcs[static_cast<size_t>(in.target)].sig.params.size();
                if (static_cast<size_t>(in.ival) != argc)
                    fail(c.file, in.mlLine, "'" + in.str + "' takes " + std::to_string(argc) +
                                                " argument(s) but the call passes " + std::to_string(in.ival));
                break;
            }
            case Op::MkClosure: {
                auto it = c.funcIndex.find(in.str);
                if (it == c.funcIndex.end()) fail(c.file, in.mlLine, "'mkclosure' of undefined function '" + in.str + "'");
                in.target = it->second;
                size_t np = c.m.funcs[static_cast<size_t>(in.target)].sig.params.size();
                if (static_cast<size_t>(in.ival) > np)
                    fail(c.file, in.mlLine, "'mkclosure' captures " + std::to_string(in.ival) + " value(s) but '" + in.str + "' has only " +
                                                std::to_string(np) + " parameter(s)");
                break;
            }
            case Op::NewObj: {
                auto it = c.classIndex.find(in.str);
                if (it == c.classIndex.end()) fail(c.file, in.mlLine, "'newobj' of undeclared class '" + in.str + "'");
                in.cls = it->second;
                break;
            }
            case Op::GetField:
            case Op::SetField: {
                size_t dot = in.str.rfind('.');
                if (dot == std::string::npos || dot == 0 || dot + 1 == in.str.size())
                    fail(c.file, in.mlLine, "'" + std::string(opName(in.op)) + "' needs Class.field, got '" + in.str + "'");
                auto it = c.classIndex.find(in.str.substr(0, dot));
                if (it == c.classIndex.end())
                    fail(c.file, in.mlLine, "'" + std::string(opName(in.op)) + "' on undeclared class '" + in.str.substr(0, dot) + "'");
                in.cls = it->second;
                const ClassDecl& cd = c.m.classes[static_cast<size_t>(in.cls)];
                std::string field = in.str.substr(dot + 1);
                size_t k = 0;
                while (k < cd.fields.size() && cd.fields[k].name != field) ++k;
                if (k == cd.fields.size())
                    fail(c.file, in.mlLine, "class '" + cd.name + "' has no field '" + field + "'");
                in.ival = static_cast<long long>(k);
                break;
            }
            case Op::GetGlobal:
            case Op::SetGlobal: {
                auto it = c.globalIndex.find(in.str);
                if (it == c.globalIndex.end()) fail(c.file, in.mlLine, "undeclared global '" + in.str + "'");
                in.target = it->second;
                break;
            }
            case Op::Jump:
            case Op::JumpFalse: {
                auto it = f.labels.find(in.str);
                if (it == f.labels.end())
                    fail(c.file, in.mlLine, "jump to undefined label '" + in.str + "' in function '" + f.name + "'");
                in.target = it->second.first;
                break;
            }
            case Op::Load:
            case Op::Store:
                if (static_cast<size_t>(in.ival) >= nslots)
                    fail(c.file, in.mlLine, "local index " + std::to_string(in.ival) + " is out of range (function '" +
                                                f.name + "' has " + std::to_string(nslots) + " local slot(s))");
                break;
            default: break;
        }
    }
}

void verifyFunction(Ctx& c, Function& f) {
    const std::string& file = c.file;
    if (f.code.empty()) fail(file, f.mlLine, "function '" + f.name + "' has no instructions");

    std::vector<Type> slots = f.sig.params;  // slot types: parameters first, then declared locals
    slots.insert(slots.end(), f.locals.begin(), f.locals.end());
    resolve(c, f, slots.size());

    // Abstract interpretation over operand-stack types. Each instruction is visited once: the
    // first path to reach it fixes its incoming stack, and any later path must agree. Code that
    // no path reaches is never executed and is not stack-checked.
    std::deque<SNode> arena;  // stable addresses; owns every node of this function
    auto push = [&](SRef parent, Type t) -> SRef {
        arena.push_back(SNode{std::move(t), parent, sz(parent) + 1});
        return &arena.back();
    };

    std::vector<SRef> state(f.code.size(), nullptr);
    std::vector<char> reached(f.code.size(), 0);
    std::vector<size_t> work;

    auto flow = [&](size_t from, size_t to, SRef s) {
        if (to >= f.code.size())
            fail(file, f.code[from].mlLine,
                 "execution can fall off the end of function '" + f.name + "' (missing 'ret')");
        if (!reached[to]) {
            reached[to] = 1;
            state[to] = s;
            work.push_back(to);
        } else if (!sameStack(state[to], s)) {
            fail(file, f.code[from].mlLine,
                 "inconsistent operand stack where paths meet: expected " + stackStr(state[to]) + " but found " +
                     stackStr(s));
        }
    };

    reached[0] = 1;
    work.push_back(0);
    while (!work.empty()) {
        size_t k = work.back();
        work.pop_back();
        const Instr& in = f.code[k];
        SRef st = state[k];
        const char* nm = opName(in.op);

        // Operand checks. `fromTop` counts from the top of the stack (1 = top); `operandNo` is the
        // operand's position as written in docs/ml-spec.md (bottom to top).
        auto need = [&](size_t n, const char* noun = "operand", const std::string& what = "") {
            if (sz(st) < n)
                fail(file, in.mlLine, "stack underflow: '" + (what.empty() ? std::string(nm) : what) + "' needs " +
                                          std::to_string(n) + " " + noun + "(s) but the stack holds " +
                                          std::to_string(sz(st)));
        };
        auto expectAt = [&](size_t fromTop, size_t operandNo, const Type& want) {
            const Type& have = at(st, fromTop);
            if (have != want)
                fail(file, in.mlLine, "operand " + std::to_string(operandNo) + " of '" + nm + "' expects " +
                                          typeName(want) + " but the stack holds " + typeName(have));
        };
        auto expectArrayAt = [&](size_t fromTop, size_t operandNo) -> Type {
            const Type& have = at(st, fromTop);
            if (!have.isArray())
                fail(file, in.mlLine, "operand " + std::to_string(operandNo) + " of '" + nm +
                                          "' expects an array but the stack holds " + typeName(have));
            return have;
        };
        // Pops operands of the given types (bottom to top) in one go.
        auto popExpect = [&](const std::vector<Type>& want, const char* noun, const std::string& what) {
            need(want.size(), noun, what);
            auto have = topTypes(st, want.size());
            for (size_t p = 0; p < want.size(); ++p)
                if (*have[p] != want[p])
                    fail(file, in.mlLine, std::string(noun) + " " + std::to_string(p + 1) + " of '" + what +
                                              "' expects " + typeName(want[p]) + " but the stack holds " +
                                              typeName(*have[p]));
            st = drop(st, want.size());
        };
        auto op = [&](std::initializer_list<Type> want, const Type& result) {  // typed operator
            popExpect(std::vector<Type>(want), "operand", nm);
            st = push(st, result);
        };

        switch (in.op) {
            case Op::PushStr: st = push(st, Type::String); break;
            case Op::PushInt: st = push(st, Type::Int); break;
            case Op::PushBool: st = push(st, Type::Bool); break;
            case Op::Load: st = push(st, slots[static_cast<size_t>(in.ival)]); break;
            case Op::Store: popExpect({slots[static_cast<size_t>(in.ival)]}, "operand", nm); break;
            case Op::Pop:
                if (sz(st) == 0) fail(file, in.mlLine, "'pop' on an empty stack");
                st = st->parent;
                break;
            case Op::AddI: case Op::SubI: case Op::MulI: case Op::DivI: case Op::RemI:
                op({Type::Int, Type::Int}, Type::Int);
                break;
            case Op::NegI: op({Type::Int}, Type::Int); break;
            case Op::Concat: op({Type::String, Type::String}, Type::String); break;
            case Op::StrInt: op({Type::Int}, Type::String); break;
            case Op::StrBool: op({Type::Bool}, Type::String); break;
            case Op::EqI: case Op::NeI: case Op::LtI: case Op::LeI: case Op::GtI: case Op::GeI:
                op({Type::Int, Type::Int}, Type::Bool);
                break;
            case Op::EqS: case Op::NeS: op({Type::String, Type::String}, Type::Bool); break;
            case Op::EqB: case Op::NeB: op({Type::Bool, Type::Bool}, Type::Bool); break;
            case Op::Not: op({Type::Bool}, Type::Bool); break;

            case Op::NewArray:
                need(1);
                expectAt(1, 1, Type::Int);
                st = push(drop(st, 1), Type::arrayOf(in.type));
                break;
            case Op::ArrayLit: {
                size_t n = static_cast<size_t>(in.ival);
                need(n);
                auto have = topTypes(st, n);
                for (size_t p = 0; p < n; ++p)
                    if (*have[p] != in.type)
                        fail(file, in.mlLine, "operand " + std::to_string(p + 1) + " of '" + nm + "' expects " +
                                                  typeName(in.type) + " but the stack holds " + typeName(*have[p]));
                st = push(drop(st, n), Type::arrayOf(in.type));
                break;
            }
            case Op::ALoad: {
                need(2);
                expectAt(1, 2, Type::Int);
                Type arr = expectArrayAt(2, 1);
                st = push(drop(st, 2), *arr.elem);
                break;
            }
            case Op::AStore: {
                need(3);
                Type arr = expectArrayAt(3, 1);
                expectAt(2, 2, Type::Int);
                expectAt(1, 3, *arr.elem);
                st = drop(st, 3);
                break;
            }
            case Op::ALen:
                need(1);
                expectArrayAt(1, 1);
                st = push(drop(st, 1), Type::Int);
                break;
            case Op::APush: {
                need(2);
                Type arr = expectArrayAt(2, 1);
                expectAt(1, 2, *arr.elem);
                st = drop(st, 2);
                break;
            }
            case Op::ACopy:
                need(1);
                expectArrayAt(1, 1);  // the array type is unchanged
                break;
            case Op::EqRef:
            case Op::NeRef: {
                need(2);
                Type a = at(st, 2);
                if (!a.isRef())
                    fail(file, in.mlLine, std::string("operand 1 of '") + nm + "' expects an array or map but the stack holds " + typeName(a));
                expectAt(1, 2, a);
                st = push(drop(st, 2), Type::Bool);
                break;
            }

            // ---- version 4: double, char, Guid ----
            case Op::PushDbl: st = push(st, Type::Double); break;
            case Op::PushChr: st = push(st, Type::Char); break;
            case Op::AddD: case Op::SubD: case Op::MulD: case Op::DivD: case Op::RemD:
                op({Type::Double, Type::Double}, Type::Double);
                break;
            case Op::NegD: op({Type::Double}, Type::Double); break;
            case Op::EqD: case Op::NeD: case Op::LtD: case Op::LeD: case Op::GtD: case Op::GeD:
                op({Type::Double, Type::Double}, Type::Bool);
                break;
            case Op::EqC: case Op::NeC: case Op::LtC: case Op::LeC: case Op::GtC: case Op::GeC:
                op({Type::Char, Type::Char}, Type::Bool);
                break;
            case Op::EqG: case Op::NeG: op({Type::Guid, Type::Guid}, Type::Bool); break;
            case Op::I2D: op({Type::Int}, Type::Double); break;
            case Op::D2I: op({Type::Double}, Type::Int); break;
            case Op::C2I: op({Type::Char}, Type::Int); break;
            case Op::I2C: op({Type::Int}, Type::Char); break;
            case Op::StrDbl: op({Type::Double}, Type::String); break;
            case Op::StrChr: op({Type::Char}, Type::String); break;
            case Op::StrGuid: op({Type::Guid}, Type::String); break;
            case Op::ParseInt: op({Type::String}, Type::Int); break;
            case Op::ParseDbl: op({Type::String}, Type::Double); break;
            case Op::ParseGuid: op({Type::String}, Type::Guid); break;

            // ---- version 4: nullable types ----
            case Op::PushNull: st = push(st, in.type); break;
            case Op::Wrap: {
                need(1);
                Type t = at(st, 1);
                if (t.kind == Type::Nullable || t.kind == Type::Obj || t.kind == Type::Void || t.kind == Type::Fn)
                    fail(file, in.mlLine, "'wrap' expects a value that is not nullable, not obj and not a function but the stack holds " + typeName(t));
                st = push(drop(st, 1), Type::nullableOf(t));
                break;
            }
            case Op::Unwrap: {
                need(1);
                Type t = at(st, 1);
                if (t.kind != Type::Nullable)
                    fail(file, in.mlLine, "'unwrap' expects a nullable value (T?) but the stack holds " + typeName(t));
                st = push(drop(st, 1), *t.elem);
                break;
            }
            case Op::IsNull: {
                need(1);
                Type t = at(st, 1);
                if (t.kind != Type::Nullable && t.kind != Type::Obj)
                    fail(file, in.mlLine, "'isnull' expects a nullable value or obj but the stack holds " + typeName(t));
                st = push(drop(st, 1), Type::Bool);
                break;
            }
            case Op::Dup: {
                need(1);
                Type t = at(st, 1);
                st = push(st, t);
                break;
            }

            // ---- version 4: maps ----
            case Op::NewMap: st = push(st, in.type); break;

            // ---- version 5: classes and globals ----
            case Op::NewObj: st = push(st, in.type); break;
            case Op::GetField: {
                need(1);
                const ClassDecl& cd = c.m.classes[static_cast<size_t>(in.cls)];
                expectAt(1, 1, Type::classOf(cd.name));
                st = push(drop(st, 1), cd.fields[static_cast<size_t>(in.ival)].type);
                break;
            }
            case Op::SetField: {
                need(2);
                const ClassDecl& cd = c.m.classes[static_cast<size_t>(in.cls)];
                expectAt(2, 1, Type::classOf(cd.name));
                expectAt(1, 2, cd.fields[static_cast<size_t>(in.ival)].type);
                st = drop(st, 2);
                break;
            }
            case Op::GetGlobal: st = push(st, c.m.globals[static_cast<size_t>(in.target)].type); break;
            case Op::SetGlobal: {
                need(1);
                expectAt(1, 1, c.m.globals[static_cast<size_t>(in.target)].type);
                st = drop(st, 1);
                break;
            }
            case Op::MapGet: case Op::MapTry: {
                need(2);
                Type m = at(st, 2);
                if (!m.isMap()) fail(file, in.mlLine, std::string("operand 1 of '") + nm + "' expects a map but the stack holds " + typeName(m));
                expectAt(1, 2, *m.key);
                Type v = *m.elem;
                if (in.op == Op::MapTry && v.kind != Type::Nullable && v.kind != Type::Obj) v = Type::nullableOf(v);
                st = push(drop(st, 2), v);
                break;
            }
            case Op::MapSet: {
                need(3);
                Type m = at(st, 3);
                if (!m.isMap()) fail(file, in.mlLine, "operand 1 of 'mapset' expects a map but the stack holds " + typeName(m));
                expectAt(2, 2, *m.key);
                expectAt(1, 3, *m.elem);
                st = drop(st, 3);
                break;
            }
            case Op::MapHas: case Op::MapDel: {
                need(2);
                Type m = at(st, 2);
                if (!m.isMap()) fail(file, in.mlLine, std::string("operand 1 of '") + nm + "' expects a map but the stack holds " + typeName(m));
                expectAt(1, 2, *m.key);
                st = push(drop(st, 2), Type::Bool);
                break;
            }
            case Op::MapLen: case Op::MapKeys: case Op::MapValues: {
                need(1);
                Type m = at(st, 1);
                if (!m.isMap()) fail(file, in.mlLine, std::string("operand 1 of '") + nm + "' expects a map but the stack holds " + typeName(m));
                Type r = in.op == Op::MapLen ? Type(Type::Int) : Type::arrayOf(in.op == Op::MapKeys ? *m.key : *m.elem);
                st = push(drop(st, 1), r);
                break;
            }

            // ---- version 4: obj and dynamic operations ----
            case Op::Box: {
                need(1);
                Type t = at(st, 1);
                if (t.kind == Type::Void) fail(file, in.mlLine, "'box' cannot box void");
                if (t.kind == Type::Fn) fail(file, in.mlLine, "'box' cannot box a function value");
                st = push(drop(st, 1), Type::Obj);
                break;
            }
            case Op::Unbox: op({Type::Obj}, in.type); break;
            case Op::IsType: op({Type::Obj}, Type::Bool); break;
            case Op::ToStrObj: op({Type::Obj}, Type::String); break;
            case Op::EqObj: case Op::NeObj: op({Type::Obj, Type::Obj}, Type::Bool); break;
            case Op::DynAdd: case Op::DynSub: case Op::DynMul: case Op::DynDiv: case Op::DynRem:
            case Op::DynEq: case Op::DynNe: case Op::DynLt: case Op::DynLe: case Op::DynGt: case Op::DynGe:
            case Op::DynIndex:
                op({Type::Obj, Type::Obj}, Type::Obj);
                break;
            case Op::DynNeg: case Op::DynNot: case Op::DynMember: op({Type::Obj}, Type::Obj); break;
            case Op::DynSetIndex: popExpect({Type::Obj, Type::Obj, Type::Obj}, "operand", nm); break;
            case Op::DynInvoke: {
                std::vector<Type> want(static_cast<size_t>(in.ival) + 1, Type(Type::Obj));
                popExpect(want, "operand", nm);
                st = push(st, Type::Obj);
                break;
            }

            case Op::Jump:
                flow(k, static_cast<size_t>(in.target), st);
                continue;  // no fall-through
            case Op::JumpFalse:
                popExpect({Type::Bool}, "operand", nm);
                flow(k, static_cast<size_t>(in.target), st);
                break;  // falls through to k + 1 as well
            case Op::HostCall: {
                const HostDecl& h = c.m.hosts[static_cast<size_t>(in.host)];
                popExpect(h.sig.params, "argument", in.str);
                if (h.sig.ret.kind != Type::Void) st = push(st, h.sig.ret);
                break;
            }
            case Op::Call: {
                const Sig& sig = c.m.funcs[static_cast<size_t>(in.target)].sig;
                popExpect(sig.params, "argument", in.str);
                if (sig.ret.kind != Type::Void) st = push(st, sig.ret);
                break;
            }
            case Op::MkClosure: {
                const Sig& sig = c.m.funcs[static_cast<size_t>(in.target)].sig;
                const size_t ncap = static_cast<size_t>(in.ival);
                popExpect(std::vector<Type>(sig.params.begin(), sig.params.begin() + static_cast<std::ptrdiff_t>(ncap)), "captured value", in.str);
                st = push(st, Type::fnOf(std::vector<Type>(sig.params.begin() + static_cast<std::ptrdiff_t>(ncap), sig.params.end()), sig.ret));
                break;
            }
            case Op::CallFn: {
                const size_t n = static_cast<size_t>(in.ival);
                need(n + 1);
                Type fnT = at(st, n + 1);
                if (fnT.kind != Type::Fn)
                    fail(file, in.mlLine, "'callfn' expects a function value below its " + std::to_string(n) + " argument(s) but the stack holds " + typeName(fnT));
                if (fnT.params->size() != n)
                    fail(file, in.mlLine, "'callfn' passes " + std::to_string(n) + " argument(s) but " + typeName(fnT) + " takes " + std::to_string(fnT.params->size()));
                auto have = topTypes(st, n);
                for (size_t p = 0; p < n; ++p)
                    if (*have[p] != (*fnT.params)[p])
                        fail(file, in.mlLine, "argument " + std::to_string(p + 1) + " of 'callfn' expects " + typeName((*fnT.params)[p]) +
                                                  " but the stack holds " + typeName(*have[p]));
                st = drop(st, n + 1);
                if (fnT.elem->kind != Type::Void) st = push(st, *fnT.elem);
                break;
            }
            case Op::Ret: {
                size_t want = f.sig.ret.kind == Type::Void ? 0 : 1;
                if (sz(st) != want)
                    fail(file, in.mlLine, "'ret' in '" + f.name + "' expects " + std::to_string(want) +
                                              " value(s) on the stack but found " + std::to_string(sz(st)));
                if (want == 1 && st->type != f.sig.ret)
                    fail(file, in.mlLine, "'ret' in '" + f.name + "' must return " + typeName(f.sig.ret) +
                                              " but the stack holds " + typeName(st->type));
                continue;  // no successor
            }
        }
        flow(k, k + 1, st);
    }
}

} // namespace

void verify(Module& m, const HostRegistry& registry, const std::string& file) {
    if (!m.hasSource) fail(file, 1, "missing 'source' directive");

    std::map<std::string, int> hostIndex;
    for (size_t i = 0; i < m.hosts.size(); ++i) {
        const HostDecl& h = m.hosts[i];
        if (!hostIndex.emplace(h.name, static_cast<int>(i)).second)
            fail(file, h.mlLine, "duplicate host declaration '" + h.name + "'");
        const HostFunction* hf = registry.find(h.name);
        if (!hf) fail(file, h.mlLine, "host function '" + h.name + "' is not registered with this PVM");
        if (!sameSig(hf->sig, h.sig))
            fail(file, h.mlLine, "host function '" + h.name + "' is declared as " + sigToString(h.sig) +
                                     " but the PVM provides " + sigToString(hf->sig));
        // The permission is checked when the function is *called* (interpreter.cpp), so a program may declare
        // host functions it never reaches (a library bundles many) without being granted all of them.
    }

    // Classes and globals (version 5): names are unique and every class type refers to a declared class.
    std::map<std::string, int> classIndex, globalIndex;
    for (size_t i = 0; i < m.classes.size(); ++i) classIndex.emplace(m.classes[i].name, static_cast<int>(i));
    for (size_t i = 0; i < m.globals.size(); ++i) globalIndex.emplace(m.globals[i].name, static_cast<int>(i));
    std::function<void(const Type&, int, const std::string&)> checkType = [&](const Type& t, int line, const std::string& where) {
        if (t.kind == Type::Class && !classIndex.count(t.name))
            fail(file, line, "unknown class '" + t.name + "' used in " + where);
        if (t.params) for (const Type& p : *t.params) checkType(p, line, where);
        if (t.elem) checkType(*t.elem, line, where);
        if (t.key) checkType(*t.key, line, where);
    };
    for (const ClassDecl& c : m.classes)
        for (const FieldDecl& f : c.fields) checkType(f.type, c.mlLine, "class '" + c.name + "'");
    for (const GlobalDecl& g : m.globals) checkType(g.type, g.mlLine, "global '" + g.name + "'");
    for (const HostDecl& h : m.hosts) {
        for (const Type& t : h.sig.params)
            if (t.kind == Type::Class || t.kind == Type::Fn || (t.elem && t.elem->kind == Type::Class)) fail(file, h.mlLine, "host function '" + h.name + "' cannot take class or function types");
    }
    for (const Function& f : m.funcs) {
        for (const Type& t : f.sig.params) checkType(t, f.mlLine, "the signature of '" + f.name + "'");
        checkType(f.sig.ret, f.mlLine, "the signature of '" + f.name + "'");
        for (const Type& t : f.locals) checkType(t, f.mlLine, "a local of '" + f.name + "'");
        for (const Instr& in : f.code) checkType(in.type, in.mlLine, "'" + f.name + "'");
    }

    std::map<std::string, int> funcIndex;
    for (size_t i = 0; i < m.funcs.size(); ++i) {
        if (!funcIndex.emplace(m.funcs[i].name, static_cast<int>(i)).second)
            fail(file, m.funcs[i].mlLine, "duplicate function '" + m.funcs[i].name + "'");
    }

    if (m.entry.empty()) fail(file, 1, "missing 'entry' directive");
    auto e = funcIndex.find(m.entry);
    if (e == funcIndex.end()) fail(file, m.entryLine, "entry function '" + m.entry + "' is not defined");
    const Function& ef = m.funcs[static_cast<size_t>(e->second)];
    if (!ef.sig.params.empty() || ef.sig.ret != Type::Void)
        fail(file, m.entryLine, "entry function '" + m.entry + "' must have signature () -> void");
    m.entryFunc = e->second;

    Ctx ctx{m, classIndex, globalIndex, hostIndex, funcIndex, file};
    for (Function& f : m.funcs) verifyFunction(ctx, f);
}

} // namespace pvm
