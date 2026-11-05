#include "sema.hpp"

#include "generics.hpp"

#include <algorithm>
#include <functional>
#include <iterator>
#include <map>
#include <set>

namespace coyote {

namespace {

struct VarInfo {
    Type type;
    int slot;
};

struct FnCtx {
    bool inMethod = false;
    std::string name;
    Type ret = Type::Void;
    int paramCount = 0;
    std::vector<Type>* locals = nullptr;  // non-parameter locals
    std::vector<std::string>* localNames = nullptr;
    int cls = -1;                         // the class whose method is being checked
    bool isStatic = false;                // static method / static initializer (no `this`)
    bool isCtor = false;
};

// What semantic analysis knows about one class (the declaration itself is Program::classes[i]).
struct ClassInfo {
    Type type;                            // Type::classOf(unique ML name)
    std::map<std::string, int> inst;      // instance field name -> slot (position among instance fields)
    std::vector<int> instDecl;            // slot -> index into ClassDecl::fields
    std::map<std::string, int> stat;      // static field name -> index into ClassDecl::fields
    std::map<std::string, int> props;     // property name -> index into ClassDecl::properties
    std::map<std::string, int> methods;   // method name -> index into Program::methods (includes accessors)
    int ctor = -1;
    int staticInit = -1;
    int init = -1;                        // `$init` (constructor body) of a class that is part of a hierarchy
    bool hasSub = false;                  // some class extends this one
    int depth = 0;                        // length of the longest inheritance chain above this class
    std::set<int> isa;                    // this class and every class / interface it extends, directly or not
};

struct Target {
    bool isMethod;
    int index;
};

class Sema {
public:
    Sema(Program& p, std::vector<Diagnostic>& out) : prog_(p), out_(out) {}

    void run() {
        const size_t nmod = prog_.modules.size();
        tables_.assign(nmod, {});
        classTables_.assign(nmod, {});

        mangleOverloads();
        registerClasses();
        importClasses();
        resolveSupers();
        synthesizeMembers();
        resolveDeclaredTypes();
        flattenFields();
        buildClassInfo();
        inheritMethods();
        buildDispatchers();

        // Hand declarations and methods are visible throughout their file, regardless of position
        // (so methods can call each other and recurse).
        std::map<std::string, std::pair<std::string, std::string>> hostSigs;  // host name -> (signature, file)
        for (size_t i = 0; i < prog_.hands.size(); ++i) {
            const HandDecl& h = prog_.hands[i];
            curModule_ = h.unit;
            if (!tables_[static_cast<size_t>(h.unit)].emplace(h.name, Target{false, static_cast<int>(i)}).second)
                error(h.loc, "duplicate declaration of '" + h.name + "'");
            checkDuplicateParams(h.params, h.name);
            std::string sig;
            for (const Param& prm : h.params) sig += typeName(prm.type) + ",";
            sig += "->" + typeName(h.ret);
            auto ins = hostSigs.emplace(h.module + "." + h.name, std::make_pair(sig, file()));
            if (!ins.second && ins.first->second.first != sig)
                error(h.loc, "host function '" + h.module + "." + h.name + "' is declared with a different signature in " +
                                 ins.first->second.second);
        }
        for (size_t i = 0; i < prog_.methods.size(); ++i) {
            const MethodDecl& m = prog_.methods[i];
            curModule_ = m.unit;
            if (m.classIndex >= 0) continue;  // class members are reached through their class
            if (!tables_[static_cast<size_t>(m.unit)].emplace(m.name, Target{true, static_cast<int>(i)}).second)
                error(m.loc, "duplicate declaration of '" + m.name + "'");
            checkDuplicateParams(m.params, m.name);
        }

        // Imports bring the exported methods and classes of the imported file into the importing file's scope.
        for (size_t mi = 0; mi < nmod; ++mi) {
            curModule_ = static_cast<int>(mi);
            const Module& mod = prog_.modules[mi];
            for (size_t k = 0; k < mod.imports.size() && k < mod.importIndex.size(); ++k) {
                int t = mod.importIndex[k];
                if (t < 0 || static_cast<size_t>(t) == mi) continue;
                for (size_t i = 0; i < prog_.methods.size(); ++i) {
                    const MethodDecl& m = prog_.methods[i];
                    if (m.unit != t || !m.exported || m.classIndex >= 0) continue;
                    auto ins = tables_[mi].emplace(m.name, Target{true, static_cast<int>(i)});
                    if (!ins.second && !(ins.first->second.isMethod && ins.first->second.index == static_cast<int>(i)))
                        error(mod.imports[k].loc, "use \"" + mod.imports[k].spec + "\" brings '" + m.name +
                                                      "', which conflicts with another declaration of that name");
                }
            }
        }

        // Overload sets: the methods a file can call by one written name (its own and the exports of what it uses).
        topOvl_.assign(nmod, {});
        for (size_t mi = 0; mi < nmod; ++mi) {
            const Module& mod = prog_.modules[mi];
            for (size_t i = 0; i < prog_.methods.size(); ++i) {
                const MethodDecl& m = prog_.methods[i];
                if (m.srcName.empty() || m.classIndex >= 0) continue;
                bool visible = m.unit == static_cast<int>(mi);
                if (!visible && m.exported)
                    for (size_t k = 0; k < mod.imports.size() && k < mod.importIndex.size(); ++k)
                        if (mod.importIndex[k] == m.unit) { visible = true; break; }
                if (visible) topOvl_[mi][m.srcName].push_back(static_cast<int>(i));
            }
        }

        for (MethodDecl& m : prog_.methods) {
            curModule_ = m.unit;
            if (m.isDispatcher || m.isAbstract) continue;  // no body to check
            checkMethod(m);
        }

        curModule_ = 0;
        FnCtx ctx;
        ctx.locals = &prog_.mainLocals;
        ctx.localNames = &prog_.mainLocalNames;
        ctx_ = &ctx;
        scopes_.clear();
        scopes_.emplace_back();
        checkStmts(prog_.stmts);
        scopes_.clear();

        std::map<std::string, size_t> order;
        for (size_t i = 0; i < nmod; ++i) order.emplace(prog_.modules[i].path, i);
        std::stable_sort(out_.begin() + static_cast<std::ptrdiff_t>(firstDiag_), out_.end(), [&](const Diagnostic& a, const Diagnostic& b) {
            size_t fa = order.count(a.file) ? order[a.file] : 0, fb = order.count(b.file) ? order[b.file] : 0;
            if (fa != fb) return fa < fb;
            if (a.loc.line != b.loc.line) return a.loc.line < b.loc.line;
            return a.loc.col < b.loc.col;
        });
    }

    void setFirstDiag(size_t n) { firstDiag_ = n; }

private:
    Program& prog_;
    std::vector<Diagnostic>& out_;
    size_t firstDiag_ = 0;
    std::vector<std::map<std::string, Target>> tables_;  // per module: name -> callable
    std::vector<std::map<std::string, std::vector<int>>> topOvl_;  // per module: written name -> overloaded top-level methods it can call
    std::vector<std::map<std::string, int>> classTables_;  // per module: class name -> index into Program::classes
    std::vector<ClassInfo> cinfo_;
    std::map<std::string, int> uniqueClass_;             // ML class name -> index
    std::vector<int> hierOrder_;                         // classes, supertypes before subtypes
    std::map<std::pair<int, std::string>, int> dispatch_;  // (static class, method name) -> dispatcher method
    int curModule_ = 0;
    FnCtx* ctx_ = nullptr;
    std::vector<std::map<std::string, VarInfo>> scopes_;
    int loopDepth_ = 0;

    const std::string& file() const { return prog_.modules[static_cast<size_t>(curModule_)].path; }
    std::map<std::string, Target>& table() { return tables_[static_cast<size_t>(curModule_)]; }

    void error(SourceLoc loc, std::string msg) { out_.push_back(Diagnostic{file(), loc, std::move(msg)}); }

    std::map<std::string, int>& classTable() { return classTables_[static_cast<size_t>(curModule_)]; }

    // ---- classes: declarations ----

    void registerClasses() {
        cinfo_.assign(prog_.classes.size(), {});
        for (size_t i = 0; i < prog_.classes.size(); ++i) {
            ClassDecl& c = prog_.classes[i];
            curModule_ = c.unit;
            if (!classTable().emplace(c.name, static_cast<int>(i)).second)
                error(c.loc, "duplicate declaration of class '" + c.name + "'");
            std::string unique = classMlName(c);
            uniqueClass_.emplace(unique, static_cast<int>(i));
            cinfo_[i].type = Type::classOf(unique);
        }
    }

    void importClasses() {
        for (size_t mi = 0; mi < prog_.modules.size(); ++mi) {
            curModule_ = static_cast<int>(mi);
            const Module& mod = prog_.modules[mi];
            for (size_t k = 0; k < mod.imports.size() && k < mod.importIndex.size(); ++k) {
                int t = mod.importIndex[k];
                if (t < 0 || static_cast<size_t>(t) == mi) continue;
                for (size_t ci = 0; ci < prog_.classes.size(); ++ci) {
                    const ClassDecl& c = prog_.classes[ci];
                    if (c.unit != t || !c.exported) continue;
                    auto ins = classTables_[mi].emplace(c.name, static_cast<int>(ci));
                    if (!ins.second && ins.first->second != static_cast<int>(ci))
                        error(mod.imports[k].loc, "use \"" + mod.imports[k].spec + "\" brings class '" + c.name +
                                                      "', which conflicts with another class of that name");
                }
            }
        }
    }

    MethodDecl* findMethod(int ci, const std::function<bool(const MethodDecl&)>& pred) {
        for (MethodDecl& m : prog_.methods)
            if (m.classIndex == ci && pred(m)) return &m;
        return nullptr;
    }

    static StmtPtr makeAssign(ExprPtr target, ExprPtr value, SourceLoc loc) {
        auto a = std::make_unique<Stmt>();
        a->kind = Stmt::Kind::Assign;
        a->loc = loc;
        a->target = std::move(target);
        a->expr = std::move(value);
        return a;
    }

    static ExprPtr makeVar(const std::string& name, SourceLoc loc) {
        auto v = std::make_unique<Expr>();
        v->kind = Expr::Kind::Var;
        v->loc = loc;
        v->text = name;
        return v;
    }

    static ExprPtr makeMember(ExprPtr recv, const std::string& name, SourceLoc loc) {
        auto m = std::make_unique<Expr>();
        m->kind = Expr::Kind::Member;
        m->loc = loc;
        m->text = name;
        m->depth = recv->depth + 1;
        m->lhs = std::move(recv);
        return m;
    }

    static StmtPtr makeExprStmt(ExprPtr e, SourceLoc loc) {
        auto st = std::make_unique<Stmt>();
        st->kind = Stmt::Kind::ExprStmt;
        st->loc = loc;
        st->expr = std::move(e);
        return st;
    }

    bool hasBaseClass(const ClassDecl& c) const { return !c.supers.empty() && !prog_.classes[static_cast<size_t>(c.supers[0])].isInterface; }

    // Resolves `class D : Base, I1, I2`: the base class (at most one) and the interfaces; orders the classes
    // so supertypes come first, rejects cycles, and computes each class's set of supertypes.
    void resolveSupers() {
        const size_t n = prog_.classes.size();
        for (size_t i = 0; i < n; ++i) {
            ClassDecl& c = prog_.classes[i];
            curModule_ = c.unit;
            int base = -1;
            std::vector<int> ifs;
            for (size_t k = 0; k < c.superNames.size(); ++k) {
                auto it = classTable().find(c.superNames[k]);
                if (it == classTable().end()) { error(c.superLocs[k], "unknown base class or interface '" + c.superNames[k] + "'"); continue; }
                const int sidx = it->second;
                if (sidx == static_cast<int>(i)) { error(c.superLocs[k], "'" + c.name + "' cannot extend itself"); continue; }
                if (sidx == base || std::find(ifs.begin(), ifs.end(), sidx) != ifs.end()) { error(c.superLocs[k], "'" + c.superNames[k] + "' is listed twice"); continue; }
                if (prog_.classes[static_cast<size_t>(sidx)].isInterface) {
                    ifs.push_back(sidx);
                } else if (c.isInterface) {
                    error(c.superLocs[k], "interface '" + c.name + "' can only extend interfaces, but '" + c.superNames[k] + "' is a class");
                } else if (base >= 0) {
                    error(c.superLocs[k], "class '" + c.name + "' already extends '" + className(base) + "'; a class has one base class (add interfaces for more)");
                } else {
                    base = sidx;
                }
            }
            c.supers.clear();
            if (base >= 0) c.supers.push_back(base);
            for (int x : ifs) c.supers.push_back(x);
        }
        std::vector<int> state(n, 0);
        std::function<void(int)> visit = [&](int i) {
            if (state[static_cast<size_t>(i)] == 2) return;
            state[static_cast<size_t>(i)] = 1;
            ClassDecl& c = prog_.classes[static_cast<size_t>(i)];
            for (size_t k = 0; k < c.supers.size();) {
                const int sidx = c.supers[k];
                if (state[static_cast<size_t>(sidx)] == 1) {
                    curModule_ = c.unit;
                    error(c.loc, "inheritance cycle: '" + c.name + "' and '" + className(sidx) + "' extend each other");
                    c.supers.erase(c.supers.begin() + static_cast<std::ptrdiff_t>(k));
                    continue;
                }
                visit(sidx);
                ++k;
            }
            state[static_cast<size_t>(i)] = 2;
            hierOrder_.push_back(i);
        };
        for (size_t i = 0; i < n; ++i) visit(static_cast<int>(i));
        for (int i : hierOrder_) {
            ClassInfo& info = cinfo_[static_cast<size_t>(i)];
            info.isa.insert(i);
            for (int sidx : prog_.classes[static_cast<size_t>(i)].supers) {
                const ClassInfo& si = cinfo_[static_cast<size_t>(sidx)];
                info.isa.insert(si.isa.begin(), si.isa.end());
                info.depth = std::max(info.depth, si.depth + 1);
            }
            if (hasBaseClass(prog_.classes[static_cast<size_t>(i)])) cinfo_[static_cast<size_t>(prog_.classes[static_cast<size_t>(i)].supers[0])].hasSub = true;
        }
    }

    // Adds the constructor and static initializer every class needs, and moves field initializers
    // into them (instance field initializers run first in the constructor, in declaration order).
    // A class that takes part in an inheritance hierarchy gets a `$init` method instead: the
    // constructor (`$new`) allocates the object and calls `$init`, which first runs the base class's
    // `$init` (`: base(args)`), then the field initializers, then the written constructor body.
    void synthesizeMembers() {
        for (size_t ci = 0; ci < prog_.classes.size(); ++ci) {
            ClassDecl& c = prog_.classes[ci];
            curModule_ = c.unit;
            const int cidx = static_cast<int>(ci);
            if (c.isInterface) continue;
            size_t ctorIdx = prog_.methods.size();
            for (size_t mi = 0; mi < prog_.methods.size(); ++mi)
                if (prog_.methods[mi].classIndex == cidx && prog_.methods[mi].isCtor) { ctorIdx = mi; break; }
            if (ctorIdx == prog_.methods.size()) {
                MethodDecl m;
                m.unit = c.unit;
                m.classIndex = cidx;
                m.isCtor = true;
                m.name = "$new";
                m.loc = c.loc;
                if (c.isModel) {  // model: one parameter per field that has no initializer, in order
                    for (const FieldDecl& f : c.fields) {
                        if (f.isStatic || f.init || f.name[0] == '$') continue;
                        Param p;
                        p.type = f.type;
                        p.name = f.name;
                        p.loc = f.loc;
                        m.params.push_back(std::move(p));
                        m.body.push_back(makeAssign(makeMember(makeVar("this", f.loc), f.name, f.loc), makeVar(f.name, f.loc), f.loc));
                    }
                }
                prog_.methods.push_back(std::move(m));
            }
            std::vector<StmtPtr> inits;
            MethodDecl staticInit;
            for (FieldDecl& f : c.fields) {
                if (!f.init) continue;
                if (f.isStatic)
                    staticInit.body.push_back(makeAssign(makeMember(makeVar(c.name, f.loc), f.name, f.loc), std::move(f.init), f.loc));
                else
                    inits.push_back(makeAssign(makeMember(makeVar("this", f.loc), f.name, f.loc), std::move(f.init), f.loc));
            }
            const bool hasBase = hasBaseClass(c);
            if (prog_.methods[ctorIdx].hasBaseCall && !hasBase)
                error(prog_.methods[ctorIdx].baseLoc, "class '" + c.name + "' has no base class, so its constructor cannot call base(...)");
            if (hasBase || cinfo_[ci].hasSub) {
                MethodDecl init;
                init.unit = c.unit;
                init.classIndex = cidx;
                init.isInit = true;
                init.name = "$init";
                init.loc = c.loc;
                {
                    MethodDecl& ctor = prog_.methods[ctorIdx];
                    init.params = ctor.params;
                    if (hasBase) {
                        auto call = std::make_unique<Expr>();
                        call->kind = Expr::Kind::MethodCall;
                        call->loc = ctor.hasBaseCall ? ctor.baseLoc : ctor.loc;
                        call->text = "$init";
                        call->lhs = makeVar("base", call->loc);
                        call->args = std::move(ctor.baseArgs);
                        call->depth = 2;
                        for (const auto& a : call->args) call->depth = std::max(call->depth, a->depth + 1);
                        const SourceLoc callLoc = call->loc;
                        init.body.push_back(makeExprStmt(std::move(call), callLoc));
                    }
                    for (auto& st : inits) init.body.push_back(std::move(st));
                    for (auto& st : ctor.body) init.body.push_back(std::move(st));
                    ctor.body.clear();
                }
                prog_.methods.push_back(std::move(init));
                prog_.methods[ctorIdx].initMethod = static_cast<int>(prog_.methods.size()) - 1;
            } else {
                MethodDecl& ctor = prog_.methods[ctorIdx];
                ctor.body.insert(ctor.body.begin(), std::make_move_iterator(inits.begin()), std::make_move_iterator(inits.end()));
            }
            if (!staticInit.body.empty()) {
                staticInit.unit = c.unit;
                staticInit.classIndex = cidx;
                staticInit.isStatic = true;
                staticInit.isStaticInit = true;
                staticInit.name = "$static_init";
                staticInit.loc = c.loc;
                prog_.methods.push_back(std::move(staticInit));
            }
        }
    }

    // Replaces class names written in a type with the unique ML class name; reports unknown names.
    void resolveType(Type& t, SourceLoc loc) {
        switch (t.kind) {
            case Type::Array: case Type::Nullable: {
                Type inner = *t.elem;
                resolveType(inner, loc);
                t.elem = std::make_shared<const Type>(inner);
                return;
            }
            case Type::Map: {
                Type k = *t.key, v = *t.elem;
                resolveType(k, loc);
                resolveType(v, loc);
                t.key = std::make_shared<const Type>(k);
                t.elem = std::make_shared<const Type>(v);
                return;
            }
            case Type::Class: {
                auto it = classTable().find(t.name);
                if (it != classTable().end()) {
                    t = cinfo_[static_cast<size_t>(it->second)].type;
                } else if (!uniqueClass_.count(t.name)) {  // not already resolved
                    error(loc, "unknown type '" + t.name + "'");
                    t = Type::Invalid;
                }
                return;
            }
            default: return;
        }
    }

    void resolveDeclaredTypes() {
        for (size_t ci = 0; ci < prog_.classes.size(); ++ci) {
            ClassDecl& c = prog_.classes[ci];
            curModule_ = c.unit;
            for (FieldDecl& f : c.fields) resolveType(f.type, f.loc);
            for (PropertyDecl& p : c.properties) resolveType(p.type, p.loc);
        }
        for (HandDecl& h : prog_.hands) {
            curModule_ = h.unit;
            for (Param& p : h.params) {
                resolveType(p.type, p.loc);
                if (p.type.kind == Type::Class || (p.type.elem && p.type.elem->kind == Type::Class))
                    error(p.loc, "host functions cannot take class types");
            }
            resolveType(h.ret, h.loc);
            if (h.ret != Type::Invalid && !(h.ret == Type::Void || h.ret == Type::Int || h.ret == Type::Double || h.ret == Type::Bool ||
                  h.ret == Type::Char || h.ret == Type::String))
                error(h.loc, "a host function can return void, int, double, boolean, char or string, not " + typeName(h.ret));
        }
        for (MethodDecl& m : prog_.methods) {
            curModule_ = m.unit;
            for (Param& p : m.params) resolveType(p.type, p.loc);
            resolveType(m.ret, m.loc);
            if (m.classIndex < 0) continue;
            const Type& self = cinfo_[static_cast<size_t>(m.classIndex)].type;
            if (m.isCtor) {
                m.ret = self;
            } else if (!m.isStatic) {
                Param thisParam;
                thisParam.type = self;
                thisParam.name = "this";
                thisParam.loc = m.loc;
                m.params.insert(m.params.begin(), std::move(thisParam));
            }
        }
    }

    // A subclass's instance fields (and properties) start with a copy of its base class's, in the same
    // order: that is the object layout the ML hierarchy rules require.
    void flattenFields() {
        for (int ci : hierOrder_) {
            ClassDecl& c = prog_.classes[static_cast<size_t>(ci)];
            if (!hasBaseClass(c)) continue;
            curModule_ = c.unit;
            const int bi = c.supers[0];
            const ClassDecl& b = prog_.classes[static_cast<size_t>(bi)];
            std::vector<FieldDecl> fields;
            std::set<std::string> names;
            for (const FieldDecl& f : b.fields) {
                if (f.isStatic) continue;
                FieldDecl cp;
                cp.loc = f.loc; cp.name = f.name; cp.type = f.type;
                cp.isPrivate = f.isPrivate; cp.isProtected = f.isProtected;
                cp.owner = f.owner >= 0 ? f.owner : bi;
                names.insert(f.name);
                fields.push_back(std::move(cp));
            }
            for (FieldDecl& f : c.fields) {
                if (f.isStatic) continue;
                if (names.count(f.name)) error(f.loc, "field '" + f.name + "' of '" + c.name + "' hides a field inherited from '" + b.name + "'");
                fields.push_back(std::move(f));
            }
            for (FieldDecl& f : c.fields)
                if (f.isStatic) fields.push_back(std::move(f));
            c.fields = std::move(fields);
            std::vector<PropertyDecl> props;
            std::set<std::string> pnames;
            for (const PropertyDecl& p : b.properties) {
                PropertyDecl cp = p;
                cp.owner = p.owner >= 0 ? p.owner : bi;
                pnames.insert(p.name);
                props.push_back(std::move(cp));
            }
            for (PropertyDecl& p : c.properties) {
                if (pnames.count(p.name)) error(p.loc, "property '" + p.name + "' of '" + c.name + "' hides a property inherited from '" + b.name + "'");
                props.push_back(std::move(p));
            }
            c.properties = std::move(props);
        }
    }

    static std::string sigOf(const MethodDecl& m) {
        std::string s = shown(m.name) + "(";
        for (size_t i = m.isStatic ? 0 : 1; i < m.params.size(); ++i) {
            if (i > (m.isStatic ? 0u : 1u)) s += ", ";
            s += typeName(m.params[i].type);
        }
        return s + ")" + (m.ret == Type::Void ? "" : ": " + typeName(m.ret));
    }

    static bool sameSig(const MethodDecl& a, const MethodDecl& b) {
        if (a.isStatic != b.isStatic || a.params.size() != b.params.size() || !(a.ret == b.ret)) return false;
        for (size_t i = a.isStatic ? 0 : 1; i < a.params.size(); ++i)
            if (!(a.params[i].type == b.params[i].type)) return false;
        return true;
    }

    // ---- overloading ----
    //
    // Methods that share a name but differ in their parameters get unique internal names `Name$$<types>` before anything else
    // looks at them, so every table below keeps working with one name per method. Call sites that use the written name pick
    // the best candidate from the argument types (pickOverload) and switch to the internal name. Programs without
    // overloads are not touched.
    static std::string shown(const std::string& n) {
        const size_t p = n.find("$$");
        return p == std::string::npos ? n : n.substr(0, p);
    }
    static std::string declSig(const MethodDecl& m) {
        std::string s;
        for (size_t i = 0; i < m.params.size(); ++i) s += (i ? "+" : "") + mlTypeName(m.params[i].type) + (m.params[i].variadic ? "..." : "");
        return s;
    }
    static std::string writtenName(const MethodDecl& m) { return m.srcName.empty() ? m.name : m.srcName; }

    void mangleOverloads() {
        const size_t n = prog_.classes.size();
        std::vector<int> parent(n);
        for (size_t i = 0; i < n; ++i) parent[i] = static_cast<int>(i);
        std::function<int(int)> find = [&](int x) { while (parent[static_cast<size_t>(x)] != x) { parent[static_cast<size_t>(x)] = parent[static_cast<size_t>(parent[static_cast<size_t>(x)])]; x = parent[static_cast<size_t>(x)]; } return x; };
        std::map<std::string, std::vector<int>> byName;
        for (size_t i = 0; i < n; ++i) byName[prog_.classes[i].name].push_back(static_cast<int>(i));
        for (size_t i = 0; i < n; ++i)
            for (const std::string& sn : prog_.classes[i].superNames) {
                auto it = byName.find(sn);
                if (it == byName.end()) continue;
                for (int j : it->second) parent[static_cast<size_t>(find(static_cast<int>(i)))] = find(j);
            }
        // Methods of one name in a connected class hierarchy: if their signatures differ anywhere, all of them are renamed by
        // signature, so overrides and interface implementations (same signature) keep matching by name.
        std::map<std::pair<int, std::string>, std::set<std::string>> sigs;
        std::map<std::pair<int, std::string>, std::vector<size_t>> members;
        std::map<std::string, std::set<std::string>> topSigs;
        std::map<std::string, std::vector<size_t>> topMembers;
        for (size_t mi = 0; mi < prog_.methods.size(); ++mi) {
            const MethodDecl& m = prog_.methods[mi];
            if (m.isCtor || m.isInit || m.isDispatcher || m.isStaticInit || !m.typeParams.empty()) continue;
            if (m.classIndex >= 0) {
                auto key = std::make_pair(find(m.classIndex), m.name);
                sigs[key].insert(declSig(m));
                members[key].push_back(mi);
            } else {
                topSigs[m.name].insert(declSig(m));
                topMembers[m.name].push_back(mi);
            }
        }
        auto rename = [&](size_t mi) {
            MethodDecl& m = prog_.methods[mi];
            m.srcName = m.name;
            m.name = m.name + "$$" + declSig(m);
        };
        for (const auto& kv : sigs)
            if (kv.second.size() > 1) for (size_t mi : members[kv.first]) rename(mi);
        for (const auto& kv : topSigs)
            if (kv.second.size() > 1) for (size_t mi : topMembers[kv.first]) rename(mi);
    }

    // Cost of passing a value of type `from` where `to` is expected, or -1 when that is not an implicit conversion
    // (mirrors coerce()). Lower is better.
    int convCost(const Type& from, const Type& to) const {
        if (from == Type::Invalid || to == Type::Invalid || from == to) return 0;
        if (from == Type::Void) return -1;
        if (from == Type::Null) return (to.isNullable() || to.isRefLike()) ? 1 : -1;
        if (to == Type::Dyn || to == Type::Obj) return from.isRefLike() ? 1 : 4;
        if (from == Type::Dyn) return 5;
        if (from == Type::Obj) return -1;
        if (to == Type::Double && from == Type::Int) return 2;
        if (from.kind == Type::Class && to.kind == Type::Class) {
            const int fi = classOfType(from), ti = classOfType(to);
            return fi >= 0 && ti >= 0 && cinfo_[static_cast<size_t>(fi)].isa.count(ti) ? 3 : -1;
        }
        if (to.isNullable() && !from.isNullable()) {
            const int c = convCost(from, *to.elem);
            return c < 0 ? -1 : c + 1;
        }
        return -1;
    }

    // The methods of class `ci` (own and inherited) that are overloads of the written name `name`.
    std::vector<int> classOverloads(int ci, const std::string& name) const {
        std::vector<int> out;
        if (ci < 0) return out;
        for (const auto& kv : cinfo_[static_cast<size_t>(ci)].methods)
            if (prog_.methods[static_cast<size_t>(kv.second)].srcName == name) out.push_back(kv.second);
        return out;
    }

    void preCheckArgs(Expr& e) {
        for (auto& a : e.args) {
            checkValue(a, "an argument");
            a->preChecked = true;
        }
    }

    static std::string argTypesText(const std::vector<Type>& ts) {
        std::string s = "(";
        for (size_t i = 0; i < ts.size(); ++i) s += (i ? ", " : "") + typeName(ts[i]);
        return s + ")";
    }

    std::string candidateText(int mi) const {
        const MethodDecl& m = prog_.methods[static_cast<size_t>(mi)];
        std::string s = writtenName(m) + "(";
        const size_t skip = (m.classIndex >= 0 && !m.isStatic) ? 1 : 0;
        for (size_t i = skip; i < m.params.size(); ++i) s += (i > skip ? ", " : "") + typeName(m.params[i].type) + (m.params[i].variadic ? "..." : "");
        return s + ")";
    }

    // Chooses the overload that `argTypes` fit best. Returns its method index, or -1: with `quiet` nothing fits (no error);
    // otherwise an error was reported (nothing fits, or two candidates fit equally well).
    int pickOverload(Expr& e, const std::vector<int>& cands, const std::vector<Type>& argTypes, bool quiet) {
        for (const Type& t : argTypes) if (t == Type::Invalid) return -1;  // already reported
        struct Fit { int mi; std::vector<int> costs; int extra; int sum; };
        std::vector<Fit> fits;
        for (int mi : cands) {
            const MethodDecl& m = prog_.methods[static_cast<size_t>(mi)];
            const size_t skip = (m.classIndex >= 0 && !m.isStatic) ? 1 : 0;
            std::vector<Param> ps(m.params.begin() + static_cast<long>(skip), m.params.end());
            const bool variadic = !ps.empty() && ps.back().variadic;
            const size_t fixed = variadic ? ps.size() - 1 : ps.size();
            Fit f{mi, {}, 0, 0};
            bool ok = argTypes.size() >= fixed && (variadic || argTypes.size() == ps.size());
            for (size_t i = 0; ok && i < fixed; ++i) {
                const int c = convCost(argTypes[i], ps[i].type);
                if (c < 0) ok = false; else f.costs.push_back(c);
            }
            if (ok && variadic) {
                const Type& arr = ps.back().type;
                if (argTypes.size() == fixed + 1 && convCost(argTypes[fixed], arr) >= 0) {
                    f.costs.push_back(convCost(argTypes[fixed], arr));
                } else if (arr.isArray()) {
                    f.extra = 10;
                    for (size_t i = fixed; ok && i < argTypes.size(); ++i) {
                        const int c = convCost(argTypes[i], *arr.elem);
                        if (c < 0) ok = false; else f.costs.push_back(c);
                    }
                } else {
                    ok = false;
                }
            }
            if (!ok) continue;
            for (int c : f.costs) f.sum += c;
            f.sum += f.extra;
            fits.push_back(std::move(f));
        }
        const std::string name = writtenName(prog_.methods[static_cast<size_t>(cands[0])]);
        if (fits.empty()) {
            if (quiet) return -1;
            std::string msg = "no overload of '" + name + "' accepts " + argTypesText(argTypes) + "; candidates:";
            for (int mi : cands) msg += " " + candidateText(mi) + ";";
            error(e.loc, msg);
            e.type = Type::Invalid;
            return -1;
        }
        auto dominates = [](const Fit& a, const Fit& b) {  // a is at least as good everywhere and better somewhere
            if (a.costs.size() == b.costs.size()) {
                bool better = a.extra < b.extra;
                for (size_t i = 0; i < a.costs.size(); ++i) {
                    if (a.costs[i] > b.costs[i]) return false;
                    if (a.costs[i] < b.costs[i]) better = true;
                }
                return a.extra <= b.extra && better;
            }
            return a.sum < b.sum;
        };
        std::vector<const Fit*> best;
        for (const Fit& f : fits) {
            bool beaten = false;
            for (const Fit& g : fits) if (&g != &f && dominates(g, f)) { beaten = true; break; }
            if (!beaten) best.push_back(&f);
        }
        if (best.size() == 1) return best[0]->mi;
        if (quiet) return -1;
        std::string msg = "call to '" + name + "' " + argTypesText(argTypes) + " is ambiguous between";
        for (const Fit* f : best) msg += " " + candidateText(f->mi) + ";";
        error(e.loc, msg + " add a conversion such as double(x) to choose one");
        e.type = Type::Invalid;
        return -1;
    }

    // Overloads of a method name seen from class `ci` for a call whose arguments are already checked.
    bool resolveClassOverload(Expr& e, int ci) {
        const std::vector<int> ov = classOverloads(ci, e.text);
        if (ov.empty()) return true;
        std::vector<Type> ts;
        for (const auto& a : e.args) ts.push_back(a->type);
        const int mi = pickOverload(e, ov, ts, false);
        if (mi < 0) { e.type = Type::Invalid; return false; }
        e.text = prog_.methods[static_cast<size_t>(mi)].name;
        return true;
    }

    // Methods are inherited: a class sees its supertypes' methods unless it declares one with the same
    // name, which must then have the same signature (an override / implementation).
    void inheritMethods() {
        for (int ci : hierOrder_) {
            ClassDecl& c = prog_.classes[static_cast<size_t>(ci)];
            ClassInfo& info = cinfo_[static_cast<size_t>(ci)];
            curModule_ = c.unit;
            std::set<std::string> matched;
            for (int sidx : c.supers) {
                const ClassInfo& si = cinfo_[static_cast<size_t>(sidx)];
                for (const auto& [name, mi] : si.methods) {
                    const MethodDecl& M = prog_.methods[static_cast<size_t>(mi)];
                    if (M.isPrivate) continue;
                    auto own = info.methods.find(name);
                    if (own == info.methods.end()) { info.methods.emplace(name, mi); continue; }
                    const MethodDecl& O = prog_.methods[static_cast<size_t>(own->second)];
                    if (O.classIndex == ci) {
                        if (!sameSig(O, M))
                            error(O.loc, "method '" + sigOf(O) + "' of '" + c.name + "' does not match '" + sigOf(M) + "' of '" + className(M.classIndex) + "'");
                        matched.insert(name);
                    } else {
                        if (!sameSig(O, M))
                            error(c.loc, "'" + c.name + "' inherits method '" + shown(name) + "' with different signatures from '" + className(O.classIndex) + "' and '" + className(M.classIndex) + "'");
                        else if (O.isAbstract && !M.isAbstract) own->second = mi;
                    }
                }
            }
            for (const auto& [name, mi] : info.methods) {
                const MethodDecl& m = prog_.methods[static_cast<size_t>(mi)];
                if (m.classIndex == ci && m.isOverride && !matched.count(name))
                    error(m.loc, "method '" + shown(name) + "' is marked 'override' but '" + c.name + "' inherits no method of that name");
                if (!c.isInterface && m.isAbstract)
                    error(c.loc, "class '" + c.name + "' must implement '" + sigOf(m) + "' required by '" + className(m.classIndex) + "'");
            }
        }
    }

    // Calls through a class or interface type may reach an override in a subclass. For every method that
    // some subclass replaces (and every interface method) a dispatcher `T.m$v` is synthesized: a chain of
    // `isinst` tests, most derived class first, ending in the method T itself uses.
    void buildDispatchers() {
        const int n = static_cast<int>(prog_.classes.size());
        for (int t = 0; t < n; ++t) {
            ClassDecl& tc = prog_.classes[static_cast<size_t>(t)];
            curModule_ = tc.unit;
            std::vector<std::pair<std::string, int>> entries(cinfo_[static_cast<size_t>(t)].methods.begin(), cinfo_[static_cast<size_t>(t)].methods.end());
            for (const auto& [name, xi] : entries) {
                const MethodDecl& X = prog_.methods[static_cast<size_t>(xi)];
                if (X.isStatic || X.isPrivate || (X.isAbstract && !tc.isInterface)) continue;
                std::vector<std::pair<int, int>> cases;
                for (int c = 0; c < n; ++c) {
                    if (c == t || prog_.classes[static_cast<size_t>(c)].isInterface || !cinfo_[static_cast<size_t>(c)].isa.count(t)) continue;
                    auto it = cinfo_[static_cast<size_t>(c)].methods.find(name);
                    if (it == cinfo_[static_cast<size_t>(c)].methods.end() || prog_.methods[static_cast<size_t>(it->second)].isAbstract) continue;
                    const int y = it->second;
                    if (tc.isInterface) {
                        const ClassDecl& cc = prog_.classes[static_cast<size_t>(c)];
                        if (hasBaseClass(cc) && cinfo_[static_cast<size_t>(cc.supers[0])].isa.count(t)) {
                            auto bi = cinfo_[static_cast<size_t>(cc.supers[0])].methods.find(name);
                            if (bi != cinfo_[static_cast<size_t>(cc.supers[0])].methods.end() && bi->second == y) continue;  // the base's test covers it
                        }
                    } else if (y == xi) {
                        continue;
                    }
                    cases.emplace_back(c, y);
                }
                if (!tc.isInterface && cases.empty()) continue;
                std::stable_sort(cases.begin(), cases.end(), [&](const auto& l, const auto& r) {
                    return cinfo_[static_cast<size_t>(l.first)].depth > cinfo_[static_cast<size_t>(r.first)].depth;
                });
                MethodDecl d;
                d.isDispatcher = true;
                d.classIndex = t;
                d.unit = tc.unit;
                d.name = name + "$v";
                d.loc = X.loc;
                d.params = X.params;
                d.params[0].type = cinfo_[static_cast<size_t>(t)].type;
                d.ret = X.ret;
                d.cases = std::move(cases);
                d.defaultImpl = tc.isInterface ? -1 : xi;
                prog_.methods.push_back(std::move(d));
                dispatch_[{t, name}] = static_cast<int>(prog_.methods.size()) - 1;
            }
        }
    }

    void buildClassInfo() {
        for (size_t ci = 0; ci < prog_.classes.size(); ++ci) {
            ClassDecl& c = prog_.classes[ci];
            ClassInfo& info = cinfo_[ci];
            curModule_ = c.unit;
            for (size_t i = 0; i < c.fields.size(); ++i) {
                const FieldDecl& f = c.fields[i];
                if (f.isStatic) {
                    info.stat.emplace(f.name, static_cast<int>(i));
                } else {
                    info.inst.emplace(f.name, static_cast<int>(info.instDecl.size()));
                    info.instDecl.push_back(static_cast<int>(i));
                }
            }
            for (size_t i = 0; i < c.properties.size(); ++i) info.props.emplace(c.properties[i].name, static_cast<int>(i));
            for (size_t mi = 0; mi < prog_.methods.size(); ++mi) {
                const MethodDecl& m = prog_.methods[mi];
                if (m.classIndex != static_cast<int>(ci)) continue;
                if (m.isCtor) { info.ctor = static_cast<int>(mi); continue; }
                if (m.isInit) { info.init = static_cast<int>(mi); continue; }
                if (m.isDispatcher) continue;
                if (m.isStaticInit) { info.staticInit = static_cast<int>(mi); continue; }
                if (!info.methods.emplace(m.name, static_cast<int>(mi)).second)
                    error(m.loc, "duplicate method '" + shown(m.name) + "' in class '" + c.name + "' (same parameter types)");
                checkDuplicateParams(m.params, m.name);
                if (info.inst.count(m.name) || info.stat.count(m.name))
                    error(m.loc, "'" + m.name + "' is both a field and a method of class '" + c.name + "'");
            }
            if (info.ctor >= 0) checkDuplicateParams(prog_.methods[static_cast<size_t>(info.ctor)].params, c.name);
        }
    }

    void checkDuplicateParams(const std::vector<Param>& params, const std::string& fn) {
        std::map<std::string, bool> seen;
        for (const Param& prm : params)
            if (!seen.emplace(prm.name, true).second)
                error(prm.loc, "duplicate parameter name '" + prm.name + "' in '" + fn + "'");
    }

    // ---- scopes ----

    const VarInfo* lookup(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }

    // Declares a variable in the innermost scope; returns its slot. Redeclaring a name that is
    // visible in an enclosing scope of the same function is an error.
    int declare(const std::string& name, Type type, SourceLoc loc) {
        if (lookup(name)) {
            error(loc, "variable '" + name + "' is already declared");
            return -1;
        }
        int slot = ctx_->paramCount + static_cast<int>(ctx_->locals->size());
        ctx_->locals->push_back(type);
        ctx_->localNames->push_back(name);
        scopes_.back().emplace(name, VarInfo{type, slot});
        return slot;
    }

    // ---- methods and statements ----

    static bool alwaysReturns(const std::vector<StmtPtr>& stmts) {
        for (const auto& s : stmts) {
            switch (s->kind) {
                case Stmt::Kind::Return: return true;
                case Stmt::Kind::Block:
                    if (alwaysReturns(s->body)) return true;
                    break;
                case Stmt::Kind::If:
                    if (s->hasElse && alwaysReturns(s->body) && alwaysReturns(s->elseBody)) return true;
                    break;
                default: break;
            }
        }
        return false;
    }

    void checkMethod(MethodDecl& m) {
        FnCtx ctx;
        ctx.inMethod = true;
        ctx.name = m.name;
        ctx.ret = m.ret;
        ctx.paramCount = static_cast<int>(m.params.size());
        ctx.locals = &m.localTypes;
        ctx.localNames = &m.localNames;
        ctx.cls = m.classIndex;
        ctx.isStatic = m.isStatic || m.isStaticInit;
        ctx.isCtor = m.isCtor;
        ctx_ = &ctx;

        scopes_.clear();
        scopes_.emplace_back();
        for (size_t i = 0; i < m.params.size(); ++i) {
            const Param& prm = m.params[i];
            if (!scopes_.back().emplace(prm.name, VarInfo{prm.type, static_cast<int>(i)}).second) continue;  // duplicate already reported
        }
        scopes_.emplace_back();
        if (m.isCtor) m.thisSlot = declare("this", m.ret, m.loc);  // the object under construction
        checkStmts(m.body);
        scopes_.clear();

        if (m.ret != Type::Void && !m.isCtor && !alwaysReturns(m.body))
            error(m.loc, "method '" + m.name + "' does not return a value on all code paths");
    }

    void checkStmts(std::vector<StmtPtr>& stmts) {
        for (auto& s : stmts) checkStmt(*s);
    }

    void checkScoped(std::vector<StmtPtr>& stmts) {
        scopes_.emplace_back();
        checkStmts(stmts);
        scopes_.pop_back();
    }

    void checkCondition(ExprPtr& e, const char* kw) {
        Type t = checkValue(e, "a condition");
        if (t != Type::Invalid && !coerce(e, Type::Bool))
            error(e->loc, std::string("the condition of '") + kw + "' must be boolean but got " + typeName(t));
    }

    int hidden_ = 0;
    bool reportedImplicit_ = false;  // rewriteImplicitMember already reported an error
    bool writeTarget_ = false;  // checking the left side of an assignment

    void checkStmt(Stmt& s) {
        switch (s.kind) {
            case Stmt::Kind::ExprStmt:
                check(*s.expr);
                return;
            case Stmt::Kind::VarDecl: {
                if (!s.inferred) resolveType(s.declType, s.loc);
                Type init = checkValue(s.expr, "an initializer", s.inferred ? nullptr : &s.declType);
                Type vt = s.inferred ? init : s.declType;
                if (!s.inferred && s.declType == Type::Invalid) {
                    s.slot = declare(s.name, vt, s.loc);
                    return;
                }
                if (s.inferred && init == Type::Null) {
                    error(s.expr->loc, "cannot infer the type of '" + s.name + "' from null; declare it with a nullable type (e.g. int? " + s.name + " = null)");
                    vt = Type::Invalid;
                } else if (!s.inferred && init != Type::Invalid && !coerce(s.expr, s.declType)) {
                    error(s.expr->loc, "cannot initialize '" + s.name + "' (" + typeName(s.declType) +
                                           ") with a value of type " + typeName(init) + conversionHint(init, s.declType));
                }
                s.slot = declare(s.name, vt, s.loc);
                return;
            }
            case Stmt::Kind::Assign: {
                Expr& target = *s.target;
                writeTarget_ = true;
                check(target);  // reports unknown variables and bad indexing
                writeTarget_ = false;
                if (target.kind == Expr::Kind::Member && target.classIndex < 0) {
                    if (target.type != Type::Invalid)
                        error(target.loc, "invalid assignment target: only variables, elements and fields can be assigned ('" + target.text + "' is read-only)");
                    checkValue(s.expr, "a value to assign");
                    return;
                }
                if (target.kind == Expr::Kind::Member && target.methodIndex >= 0) {  // property: call its setter
                    const MethodDecl& setter = prog_.methods[static_cast<size_t>(target.methodIndex)];
                    Type pt = target.type;
                    checkValue(s.expr, "a value to assign", &pt);
                    if (s.expr->type != Type::Invalid && !coerce(s.expr, pt))
                        error(s.expr->loc, "cannot assign " + typeName(s.expr->type) + " to property '" + target.text + "' of type " +
                                               typeName(pt) + conversionHint(s.expr->type, pt));
                    auto call = std::make_unique<Expr>();
                    call->kind = Expr::Kind::MethodCall;
                    call->loc = target.loc;
                    call->text = setter.name;
                    call->methodIndex = target.methodIndex;
                    call->classIndex = target.classIndex;
                    call->type = Type::Void;
                    call->depth = target.lhs->depth + 2;
                    call->lhs = std::move(target.lhs);
                    call->args.push_back(std::move(s.expr));
                    s.kind = Stmt::Kind::ExprStmt;
                    s.expr = std::move(call);
                    s.target.reset();
                    return;
                }
                Type val = checkValue(s.expr, "a value to assign", target.type == Type::Invalid ? nullptr : &target.type);
                if (target.type == Type::Invalid || val == Type::Invalid) return;
                if (coerce(s.expr, target.type)) return;
                if (target.kind == Expr::Kind::Var)
                    error(s.expr->loc, "cannot assign " + typeName(val) + " to variable '" + target.text +
                                           "' of type " + typeName(target.type) + conversionHint(val, target.type));
                else
                    error(s.expr->loc, "cannot assign " + typeName(val) + " to " +
                                           (target.lhs->type.isMap() ? "a map value" : "an array element") + " of type " +
                                           typeName(target.type) + conversionHint(val, target.type));
                return;
            }
            case Stmt::Kind::If:
                checkCondition(s.expr, "if");
                checkScoped(s.body);
                if (s.hasElse) checkScoped(s.elseBody);
                return;
            case Stmt::Kind::While:
                checkCondition(s.expr, "while");
                ++loopDepth_;
                checkScoped(s.body);
                --loopDepth_;
                return;
            case Stmt::Kind::For:
                scopes_.emplace_back();  // the initializer's variable lives only in the loop
                if (s.init) checkStmt(*s.init);
                if (s.expr) checkCondition(s.expr, "for");
                if (s.step) checkStmt(*s.step);
                ++loopDepth_;
                checkScoped(s.body);
                --loopDepth_;
                scopes_.pop_back();
                return;
            case Stmt::Kind::ForEach: {
                if (!s.inferred) resolveType(s.declType, s.loc);
                Type coll = checkValue(s.expr, "a collection");
                scopes_.emplace_back();
                Type elem = Type::Invalid;
                if (coll != Type::Invalid) {
                    if (coll.isArray()) {
                        elem = *coll.elem;
                    } else if (coll.isMap()) {
                        // 0.6.1: "for (k in map)" walks the keys, exactly like "for (k in map.keys())".
                        auto call = std::make_unique<Expr>();
                        call->kind = Expr::Kind::MethodCall;
                        call->loc = s.expr->loc;
                        call->text = "keys";
                        s.expr->preChecked = true;
                        call->lhs = std::move(s.expr);
                        s.expr = std::move(call);
                        coll = checkValue(s.expr, "a collection");
                        if (coll.isArray()) elem = *coll.elem;
                    } else {
                        error(s.expr->loc, "cannot iterate over a value of type " + typeName(coll) + "; 'for ... in' needs an array");
                    }
                }
                Type vt = s.inferred ? elem : s.declType;
                if (!s.inferred && elem != Type::Invalid && elem != s.declType) {
                    error(s.loc, "loop variable '" + s.name + "' is declared " + typeName(s.declType) +
                                     " but the array holds " + typeName(elem));
                    vt = Type::Invalid;
                }
                s.slotArr = declare("$arr" + std::to_string(hidden_), coll == Type::Invalid ? Type(Type::Invalid) : coll, s.loc);
                s.slotIdx = declare("$idx" + std::to_string(hidden_), Type::Int, s.loc);
                ++hidden_;
                s.slot = declare(s.name, vt, s.loc);
                ++loopDepth_;
                checkScoped(s.body);
                --loopDepth_;
                scopes_.pop_back();
                return;
            }
            case Stmt::Kind::Break:
                if (loopDepth_ == 0) error(s.loc, "'break' is only allowed inside a loop");
                return;
            case Stmt::Kind::Continue:
                if (loopDepth_ == 0) error(s.loc, "'continue' is only allowed inside a loop");
                return;
            case Stmt::Kind::Block:
                checkScoped(s.body);
                return;
            case Stmt::Kind::Return: {
                if (ctx_->isCtor) {
                    error(s.loc, "a constructor cannot use 'return'; it always returns the new object");
                    if (s.expr) checkValue(s.expr, "a return value");
                    return;
                }
                if (!ctx_->inMethod) {
                    if (s.expr) checkValue(s.expr, "a return value");
                    error(s.loc, "'return' is only allowed inside a method");
                    return;
                }
                if (s.expr) {
                    Type t = checkValue(s.expr, "a return value", &ctx_->ret);
                    if (ctx_->ret == Type::Void)
                        error(s.loc, "method '" + ctx_->name + "' returns no value, so 'return' cannot have a value");
                    else if (t != Type::Invalid && !coerce(s.expr, ctx_->ret))
                        error(s.expr->loc, "method '" + ctx_->name + "' must return " + typeName(ctx_->ret) +
                                               " but got " + typeName(t) + conversionHint(t, ctx_->ret));
                } else if (ctx_->ret != Type::Void) {
                    error(s.loc, "method '" + ctx_->name + "' must return a value of type " + typeName(ctx_->ret));
                }
                return;
            }
        }
    }

    // ---- implicit conversions ----

    static ExprPtr makeConvert(ExprPtr inner, Expr::Conv conv, const Type& to) {
        auto n = std::make_unique<Expr>();
        n->kind = Expr::Kind::Convert;
        n->loc = inner->loc;
        n->conv = conv;
        n->type = to;
        n->depth = inner->depth + 1;
        n->lhs = std::move(inner);
        return n;
    }

    // Converts `e` to `target` when the language allows it implicitly, by wrapping it in a Convert
    // node (or retyping a null literal). Returns false, leaving `e` untouched, when it does not.
    //   int -> double          T -> T?          null -> T? / obj / dyn
    //   anything -> obj / dyn  dyn -> anything (checked at run time)   obj <-> dyn
    bool coerce(ExprPtr& e, const Type& target) {
        const Type from = e->type;
        if (from == Type::Invalid || target == Type::Invalid || from == target) return true;
        if (from == Type::Void) return false;
        if (from == Type::Null) {
            if (target.isNullable() || target.isRefLike()) { e->type = target; return true; }
            return false;
        }
        if (target == Type::Dyn || target == Type::Obj) {
            if (from.isRefLike()) { e = makeConvert(std::move(e), Expr::Conv::Retype, target); return true; }
            e = makeConvert(std::move(e), Expr::Conv::Box, target);
            return true;
        }
        if (from == Type::Dyn) {
            e = makeConvert(std::move(e), Expr::Conv::Unbox, target);
            return true;
        }
        if (from == Type::Obj) return false;  // obj needs an explicit conversion, e.g. int(x)
        if (target == Type::Double && from == Type::Int) {
            e = makeConvert(std::move(e), Expr::Conv::IntToDouble, target);
            return true;
        }
        if (from.kind == Type::Class && target.kind == Type::Class) {  // a subclass value where its base class or interface is expected
            const int fi = classOfType(from), ti = classOfType(target);
            if (fi >= 0 && ti >= 0 && cinfo_[static_cast<size_t>(fi)].isa.count(ti)) {
                e = makeConvert(std::move(e), Expr::Conv::Upcast, target);
                return true;
            }
            return false;
        }
        if (target.isNullable() && !from.isNullable()) {
            if (!coerce(e, *target.elem)) return false;
            e = makeConvert(std::move(e), Expr::Conv::Wrap, target);
            return true;
        }
        return false;
    }

    static std::string conversionHint(const Type& from, const Type& to) {
        if (from == Type::Double && to == Type::Int) return " (use int(x) to convert; this truncates)";
        if (from.isNullable() && !to.isNullable()) return " (the value may be null; use '?" "?' or '!')";
        if (from == Type::Obj) return " (obj needs an explicit conversion such as int(x), or an 'is' check)";
        if (from == Type::Null) return " (only nullable types, obj and dyn can be null)";
        return "";
    }

    static bool isNumeric(const Type& t) { return t == Type::Int || t == Type::Double; }

    // ---- expressions ----

    // Checks an expression that must produce a value. `expected` (optional) gives array literals
    // their element type.
    Type checkValue(ExprPtr& e, const char* context, const Type* expected = nullptr) {
        if (expected && e->kind == Expr::Kind::ArrayLit && expected->isArray())
            checkArrayLit(*e, expected->elem.get());
        else
            check(*e);
        if (e->type == Type::Void) {
            error(e->loc, "call to '" + e->text + "' returns no value and cannot be used as " + context);
            e->type = Type::Invalid;
        }
        return e->type;
    }

    void check(Expr& e) {
        if (e.preChecked) return;
        switch (e.kind) {
            case Expr::Kind::String: e.type = Type::String; return;
            case Expr::Kind::Int: e.type = Type::Int; return;
            case Expr::Kind::Bool: e.type = Type::Bool; return;
            case Expr::Kind::Double: e.type = Type::Double; return;
            case Expr::Kind::Char: e.type = Type::Char; return;
            case Expr::Kind::Null: e.type = Type::Null; return;
            case Expr::Kind::Var: {
                const VarInfo* v = lookup(e.text);
                if (!v) {
                    if (e.text == "this") {
                        error(e.loc, "'this' is only available inside instance methods and constructors");
                        e.type = Type::Invalid;
                    } else if (rewriteImplicitMember(e)) {
                        check(e);
                    } else if (reportedImplicit_) {
                        reportedImplicit_ = false;
                        e.type = Type::Invalid;
                    } else {
                        error(e.loc, "unknown variable '" + e.text + "'");
                        e.type = Type::Invalid;
                    }
                } else {
                    e.type = v->type;
                    e.slot = v->slot;
                }
                return;
            }
            case Expr::Kind::Call: checkCall(e); return;
            case Expr::Kind::Unary: checkUnary(e); return;
            case Expr::Kind::Binary: checkBinary(e); return;
            case Expr::Kind::ArrayLit: checkArrayLit(e, nullptr); return;
            case Expr::Kind::NewArray: {
                resolveType(e.newType, e.loc);
                Type size = checkValue(e.lhs, "an array size");
                if (size != Type::Invalid && !coerce(e.lhs, Type::Int))
                    error(e.lhs->loc, "array size must be int but got " + typeName(size));
                e.type = e.newType;
                return;
            }
            case Expr::Kind::NewMap: resolveType(e.newType, e.loc); e.type = e.newType; return;
            case Expr::Kind::NewObj: checkNewObj(e); return;
            case Expr::Kind::Index: checkIndex(e); return;
            case Expr::Kind::Member: checkMember(e); return;
            case Expr::Kind::MethodCall: checkMethodCall(e); return;
            case Expr::Kind::Convert: checkConvert(e); return;
            case Expr::Kind::Unwrap: {
                Type t = checkValue(e.lhs, "an operand");
                if (t == Type::Invalid) { e.type = Type::Invalid; return; }
                if (!t.isNullable()) {
                    error(e.loc, "operator '!' (unwrap) needs a nullable value but got " + typeName(t));
                    e.type = Type::Invalid;
                    return;
                }
                e.type = *t.elem;
                return;
            }
            case Expr::Kind::IsNull: e.type = Type::Bool; return;  // only created by checkBinary
            case Expr::Kind::Is: {
                resolveType(e.newType, e.loc);
                Type t = checkValue(e.lhs, "an operand");
                e.type = Type::Bool;
                if (t == Type::Invalid) return;
                if (t == Type::Null) { error(e.loc, "'is' cannot test the null literal"); return; }
                if (t.kind == Type::Class) {  // class test: isinst
                    const int li = classOfType(t), ti = classOfType(e.newType);
                    if (ti < 0) { if (e.newType != Type::Invalid) error(e.loc, "a class value can only be tested against a class or interface type"); return; }
                    if (!cinfo_[static_cast<size_t>(li)].isa.count(ti) && !cinfo_[static_cast<size_t>(ti)].isa.count(li))
                        error(e.loc, "'" + className(li) + "' and '" + className(ti) + "' are unrelated, so this test is always false");
                    return;
                }
                coerce(e.lhs, Type::Obj);
                return;
            }
        }
    }

    void checkArrayLit(Expr& e, const Type* expectedElem) {
        Type elem = Type::Invalid;
        bool bad = false, sawNull = false;
        for (auto& a : e.args) {
            Type t = checkValue(a, "an array element", expectedElem);
            if (t == Type::Invalid) { bad = true; continue; }
            if (t == Type::Null) { sawNull = true; continue; }
            if (expectedElem) continue;
            if (elem == Type::Invalid || (elem == Type::Int && t == Type::Double)) {
                elem = t;
            } else if (t != elem && !(elem == Type::Double && t == Type::Int)) {
                error(a->loc, "array element has type " + typeName(t) + " but earlier elements have type " + typeName(elem));
                bad = true;
            }
        }
        if (bad) { e.type = Type::Invalid; return; }
        if (expectedElem) {
            elem = *expectedElem;
        } else {
            if (elem == Type::Invalid) {
                error(e.loc, "cannot infer the element type of an array literal that only contains null");
                e.type = Type::Invalid;
                return;
            }
            if (sawNull && !elem.isNullable() && !elem.isRefLike()) elem = Type::nullableOf(elem);
        }
        for (auto& a : e.args) {
            if (coerce(a, elem)) continue;
            if (expectedElem) {
                // The literal does not fit the expected type. Report its natural type and let the
                // caller phrase the mismatch ("cannot initialize ... with a value of type ...").
                for (auto& x : e.args)
                    if (x->type != Type::Invalid && x->type != Type::Null) { e.type = Type::arrayOf(x->type); return; }
                e.type = Type::Invalid;
                return;
            }
            error(a->loc, "array element has type " + typeName(a->type) + " but the array holds " + typeName(elem) + conversionHint(a->type, elem));
            bad = true;
        }
        e.type = bad ? Type(Type::Invalid) : Type::arrayOf(elem);
    }

    void checkIndex(Expr& e) {
        Type col = checkValue(e.lhs, "an operand");
        Type idx = checkValue(e.rhs, "an index");
        if (col == Type::Invalid) { e.type = Type::Invalid; return; }
        if (col.isArray()) {
            if (idx != Type::Invalid && !coerce(e.rhs, Type::Int))
                error(e.rhs->loc, "array index must be int but got " + typeName(idx));
            e.type = *col.elem;
        } else if (col.isMap()) {
            if (idx != Type::Invalid && !coerce(e.rhs, *col.key))
                error(e.rhs->loc, "map key must be " + typeName(*col.key) + " but got " + typeName(idx));
            e.type = *col.elem;
        } else if (col == Type::Dyn) {
            if (idx != Type::Invalid) coerce(e.rhs, Type::Obj);
            e.type = Type::Dyn;
        } else {
            error(e.loc, "cannot index a value of type " + typeName(col));
            e.type = Type::Invalid;
        }
    }

    // ---- class members ----

    // True when `e` names a class (and no variable hides it): `Point` in Point.origin() or Point.count.
    bool isClassRef(const Expr& e, int& ci) {
        if (e.kind != Expr::Kind::Var || lookup(e.text)) return false;
        auto it = classTable().find(e.text);
        if (it == classTable().end()) return false;
        ci = it->second;
        return true;
    }

    int classOfType(const Type& t) const {
        if (t.kind != Type::Class) return -1;
        auto it = uniqueClass_.find(t.name);
        return it == uniqueClass_.end() ? -1 : it->second;
    }

    const std::string& className(int ci) const { return prog_.classes[static_cast<size_t>(ci)].name; }

    // Inside a class, a bare name that is not a local refers to a member of the class (this.x or
    // Class.x). Rewrites the Var node into that Member; returns false if there is no such member.
    bool rewriteImplicitMember(Expr& e) {
        if (ctx_->cls < 0) return false;
        const ClassInfo& info = cinfo_[static_cast<size_t>(ctx_->cls)];
        const std::string& n = e.text;
        bool isInst = info.inst.count(n) || info.props.count(n);
        bool isStat = info.stat.count(n);
        if (!isInst && !isStat) return false;
        if (isInst && ctx_->isStatic) {
            error(e.loc, "'" + n + "' is an instance member and cannot be used in a static method");
            reportedImplicit_ = true;
            return false;
        }
        e.kind = Expr::Kind::Member;
        e.lhs = makeVar(isStat ? className(ctx_->cls) : "this", e.loc);
        e.depth = 2;
        return true;
    }

    // private: only inside the declaring class; protected: also inside its subclasses.
    bool canAccess(bool isPrivate, int memberClass, bool isProtected = false) const {
        if (isPrivate) return ctx_->cls == memberClass;
        if (isProtected) return ctx_->cls >= 0 && cinfo_[static_cast<size_t>(ctx_->cls)].isa.count(memberClass) > 0;
        return true;
    }

    void checkMember(Expr& e) {
        int ci;
        if (isClassRef(*e.lhs, ci)) {  // static field: Class.field
            const ClassInfo& info = cinfo_[static_cast<size_t>(ci)];
            auto it = info.stat.find(e.text);
            if (it == info.stat.end()) {
                error(e.loc, "class '" + className(ci) + "' has no static field '" + e.text + "'");
                e.type = Type::Invalid;
                return;
            }
            const FieldDecl& f = prog_.classes[static_cast<size_t>(ci)].fields[static_cast<size_t>(it->second)];
            if (!canAccess(f.isPrivate, ci)) error(e.loc, "'" + e.text + "' is private to class '" + className(ci) + "'");
            e.isStatic = true;
            e.classIndex = ci;
            e.type = f.type;
            return;
        }
        Type recv = checkValue(e.lhs, "an operand");
        if (recv == Type::Invalid) { e.type = Type::Invalid; return; }
        if (recv.kind == Type::Class) {
            ci = classOfType(recv);
            const ClassInfo& info = cinfo_[static_cast<size_t>(ci)];
            const ClassDecl& decl = prog_.classes[static_cast<size_t>(ci)];
            auto f = info.inst.find(e.text);
            if (f != info.inst.end()) {
                const FieldDecl& fd = decl.fields[static_cast<size_t>(info.instDecl[static_cast<size_t>(f->second)])];
                const int owner = fd.owner >= 0 ? fd.owner : ci;
                if (!canAccess(fd.isPrivate, owner, fd.isProtected))
                    error(e.loc, "'" + e.text + "' is " + (fd.isPrivate ? "private" : "protected") + " to class '" + className(owner) + "'");
                e.classIndex = ci;
                e.type = fd.type;
                return;
            }
            auto p = info.props.find(e.text);
            if (p != info.props.end()) {
                const PropertyDecl& pd = decl.properties[static_cast<size_t>(p->second)];
                const int powner = pd.owner >= 0 ? pd.owner : ci;
                if (!canAccess(pd.isPrivate, powner, pd.isProtected))
                    error(e.loc, "property '" + e.text + "' is " + (pd.isPrivate ? "private" : "protected") + " to class '" + className(powner) + "'");
                const bool wantSet = writeTarget_;
                if (wantSet && !pd.hasSet) { error(e.loc, "property '" + e.text + "' has no setter"); e.type = Type::Invalid; return; }
                if (!wantSet && !pd.hasGet) { error(e.loc, "property '" + e.text + "' has no getter"); e.type = Type::Invalid; return; }
                int mi = info.methods.at(std::string(wantSet ? "set_" : "get_") + e.text);
                e.classIndex = prog_.methods[static_cast<size_t>(mi)].classIndex;
                e.methodIndex = mi;
                e.type = pd.type;
                adaptReceiver(e.lhs, mi);
                if (!wantSet) {  // a read is a call of the getter
                    e.kind = Expr::Kind::MethodCall;
                    e.text = prog_.methods[static_cast<size_t>(mi)].name;
                }
                return;
            }
            if (info.stat.count(e.text)) error(e.loc, "'" + e.text + "' is a static field; use " + className(ci) + "." + e.text);
            else error(e.loc, "class '" + className(ci) + "' has no field or property '" + e.text + "'");
            e.type = Type::Invalid;
            return;
        }
        if (recv.isNullable() && recv.elem->kind == Type::Class) {
            error(e.loc, typeName(recv) + " may be null; unwrap it with '!' (or test for null) before using '." + e.text + "'");
            e.type = Type::Invalid;
            return;
        }
        if ((recv.isArray() || recv.isMap()) && e.text == "length") {
            e.type = Type::Int;
            return;
        }
        if (recv == Type::Dyn) {  // validated at run time
            e.type = Type::Dyn;
            return;
        }
        error(e.loc, typeName(recv) + " has no property '" + e.text + "'");
        e.type = Type::Invalid;
    }

    // Checks the arguments of a call to method `mi` (skipping the implicit `this`) and sets the result type.
    // An instance method is declared for its own class; a receiver of a subclass type is upcast to it.
    void adaptReceiver(ExprPtr& lhs, int mi) {
        if (!lhs || lhs->type == Type::Invalid) return;
        const Type& want = prog_.methods[static_cast<size_t>(mi)].params.empty() ? lhs->type : prog_.methods[static_cast<size_t>(mi)].params[0].type;
        if (lhs->type.kind == Type::Class && want.kind == Type::Class && !(lhs->type == want)) coerce(lhs, want);
    }

    // `nonVirtual`: call exactly `mi` (base.m(...)); otherwise a call through class type `recvClass` goes
    // through that class's dispatcher when a subclass can override the method.
    void finishClassCall(Expr& e, int mi, bool instanceCall, bool nonVirtual = false, int recvClass = -1) {
        const MethodDecl& m = prog_.methods[static_cast<size_t>(mi)];
        const int ci = m.classIndex;
        if (!canAccess(m.isPrivate, ci, m.isProtected)) error(e.loc, "method '" + shown(e.text) + "' is " + (m.isPrivate ? "private" : "protected") + " to class '" + className(ci) + "'");
        if (m.isAbstract && nonVirtual) error(e.loc, "cannot call abstract method '" + e.text + "' of '" + className(ci) + "' with base");
        int target = mi;
        if (instanceCall && !nonVirtual && recvClass >= 0) {
            auto d = dispatch_.find({recvClass, m.name});
            if (d != dispatch_.end()) target = d->second;
        }
        e.methodIndex = target;
        e.classIndex = prog_.methods[static_cast<size_t>(target)].classIndex;
        e.isStatic = !instanceCall;
        if (instanceCall) adaptReceiver(e.lhs, target);
        e.type = m.ret;
        const size_t skip = instanceCall ? 1 : 0;
        const size_t want = m.params.size() - skip;
        packVariadic(e, m.params, skip);
        if (e.args.size() != want) {
            error(e.loc, "method '" + shown(e.text) + "' expects " + std::to_string(want) + " argument(s) but got " + std::to_string(e.args.size()));
            return;
        }
        for (size_t i = 0; i < e.args.size(); ++i) checkArg(e, i, m.params[i + skip].type);
    }

    void checkNewObj(Expr& e) {
        auto it = classTable().find(e.text);
        for (auto& a : e.args) checkValue(a, "an argument");
        if (e.text == "$invalid") { e.type = Type::Invalid; return; }  // generic expansion already reported it
        if (it == classTable().end()) {
            error(e.loc, "unknown class '" + e.text + "'");
            e.type = Type::Invalid;
            return;
        }
        const int ci = it->second;
        if (prog_.classes[static_cast<size_t>(ci)].isInterface) {
            error(e.loc, "cannot create an instance of interface '" + className(ci) + "'");
            e.type = Type::Invalid;
            return;
        }
        const MethodDecl& ctor = prog_.methods[static_cast<size_t>(cinfo_[static_cast<size_t>(ci)].ctor)];
        if (!canAccess(ctor.isPrivate, ci, ctor.isProtected)) error(e.loc, "the constructor of '" + className(ci) + "' is " + (ctor.isPrivate ? "private" : "protected"));
        e.classIndex = ci;
        e.methodIndex = cinfo_[static_cast<size_t>(ci)].ctor;
        e.type = cinfo_[static_cast<size_t>(ci)].type;
        packVariadic(e, ctor.params, 0);
        if (e.args.size() != ctor.params.size()) {
            error(e.loc, "the constructor of '" + className(ci) + "' expects " + std::to_string(ctor.params.size()) +
                             " argument(s) but got " + std::to_string(e.args.size()));
            return;
        }
        std::string saved = e.text;
        e.text = "new " + className(ci);
        for (size_t i = 0; i < e.args.size(); ++i) checkArg(e, i, ctor.params[i].type);
        e.text = saved;
    }

    bool expectArgCount(Expr& e, size_t want) {
        if (e.args.size() == want) return true;
        error(e.loc, "method '" + shown(e.text) + "' expects " + std::to_string(want) + " argument(s) but got " +
                         std::to_string(e.args.size()));
        return false;
    }

    // Variadic callee (`T... xs`): the arguments from the variadic position on are packed into one array
    // literal, unless exactly one argument is given and it already has the array type. `skip` is the
    // number of leading parameters that are not in e.args (the receiver of an instance method).
    void packVariadic(Expr& e, const std::vector<Param>& ps, size_t skip) {
        if (ps.empty() || !ps.back().variadic) return;
        const size_t fixed = ps.size() - skip - 1;
        if (e.args.size() < fixed) return;  // the caller reports the count mismatch
        const Type arr = ps.back().type;
        if (e.args.size() == fixed + 1 && e.args[fixed]->type == arr) return;
        auto lit = std::make_unique<Expr>();
        lit->kind = Expr::Kind::ArrayLit;
        lit->loc = e.args.size() > fixed ? e.args[fixed]->loc : e.loc;
        lit->type = arr;
        lit->depth = 1;
        bool bad = false;
        for (size_t i = fixed; i < e.args.size(); ++i) {
            ExprPtr& a = e.args[i];
            lit->depth = std::max(lit->depth, a->depth + 1);
            if (a->type == Type::Invalid) { bad = true; continue; }
            if (!coerce(a, *arr.elem)) {
                error(a->loc, "argument " + std::to_string(i + 1) + " of '" + shown(e.text) + "' expects " + typeName(*arr.elem) +
                                  " (or an " + typeName(arr) + ") but got " + typeName(a->type) + conversionHint(a->type, *arr.elem));
                bad = true;
            }
        }
        for (size_t i = fixed; i < e.args.size(); ++i) lit->args.push_back(std::move(e.args[i]));
        e.args.resize(fixed);
        if (bad) lit->type = Type::Invalid;
        e.args.push_back(std::move(lit));
    }

    // `recv.name(args)` where no built-in or class method matches: use an extension method
    // `method name(this T self, ...)` visible from this file, if one accepts the receiver.
    bool tryExtension(Expr& e) {
        auto ovl = topOvl_[static_cast<size_t>(curModule_)].find(e.text);
        if (ovl != topOvl_[static_cast<size_t>(curModule_)].end()) {  // overloaded extension methods: the receiver is the first argument
            std::vector<int> exts;
            for (int mi : ovl->second) if (prog_.methods[static_cast<size_t>(mi)].isExtension) exts.push_back(mi);
            if (exts.empty()) return false;
            std::vector<Type> ts{e.lhs->type};
            for (const auto& a : e.args) ts.push_back(a->type);
            const int mi = pickOverload(e, exts, ts, true);
            if (mi < 0) return false;
            e.text = prog_.methods[static_cast<size_t>(mi)].name;
        }
        auto it = table().find(e.text);
        if (it == table().end() || !it->second.isMethod) return false;
        const MethodDecl& m = prog_.methods[static_cast<size_t>(it->second.index)];
        if (!m.isExtension) return false;
        e.kind = Expr::Kind::Call;
        e.args.insert(e.args.begin(), std::move(e.lhs));
        e.lhs.reset();
        finishPlainCall(e, it->second.index);
        return true;
    }

    void finishPlainCall(Expr& e, int index) {
        const MethodDecl& m = prog_.methods[static_cast<size_t>(index)];
        e.methodIndex = index;
        e.type = m.ret;
        packVariadic(e, m.params, 0);
        if (e.args.size() != m.params.size()) {
            error(e.loc, "function '" + shown(e.text) + "' expects " + std::to_string(m.params.size()) + " argument(s) but got " + std::to_string(e.args.size()));
            return;
        }
        for (size_t i = 0; i < e.args.size(); ++i) checkArg(e, i, m.params[i].type);
    }

    void checkArg(Expr& e, size_t i, const Type& want) {
        ExprPtr& a = e.args[i];
        if (a->type != Type::Invalid && !coerce(a, want))
            error(a->loc, "argument " + std::to_string(i + 1) + " of '" + shown(e.text) + "' expects " + typeName(want) +
                              " but got " + typeName(a->type) + conversionHint(a->type, want));
    }

    void checkMethodCall(Expr& e) {
        int ci;
        if (e.lhs && e.lhs->kind == Expr::Kind::Var && e.lhs->text == "base" && !lookup("base")) {  // base.m(args) / constructor chain
            const bool inInstance = ctx_->cls >= 0 && !ctx_->isStatic;
            const ClassDecl* cd = ctx_->cls >= 0 ? &prog_.classes[static_cast<size_t>(ctx_->cls)] : nullptr;
            for (auto& a : e.args) checkValue(a, "an argument");
            e.type = Type::Invalid;
            if (!inInstance || !hasBaseClass(*cd)) {
                error(e.lhs->loc, inInstance ? "'base' is only available in a class that has a base class" : "'base' is only available inside instance methods and constructors");
                return;
            }
            const int bi = cd->supers[0];
            if (!resolveClassOverload(e, bi)) return;
            const ClassInfo& binfo = cinfo_[static_cast<size_t>(bi)];
            int mi = -1;
            if (e.text == "$init") {
                mi = binfo.init;
            } else {
                auto it = binfo.methods.find(e.text);
                if (it != binfo.methods.end()) mi = it->second;
            }
            if (mi < 0) { error(e.loc, "base class '" + className(bi) + "' has no method '" + e.text + "'"); return; }
            e.lhs = makeVar("this", e.lhs->loc);
            check(*e.lhs);
            if (prog_.methods[static_cast<size_t>(mi)].isStatic) { error(e.loc, "method '" + e.text + "' is static; call it as " + className(bi) + "." + e.text + "(...)"); return; }
            if (e.text == "$init") e.text = "base";
            finishClassCall(e, mi, true, true, bi);
            return;
        }
        if (isClassRef(*e.lhs, ci)) {  // static call: Class.method(...)
            for (auto& a : e.args) checkValue(a, "an argument");
            if (!resolveClassOverload(e, ci)) return;
            const ClassInfo& info = cinfo_[static_cast<size_t>(ci)];
            auto it = info.methods.find(e.text);
            if (it == info.methods.end()) {
                error(e.loc, "class '" + className(ci) + "' has no method '" + e.text + "'");
                e.type = Type::Invalid;
            } else if (!prog_.methods[static_cast<size_t>(it->second)].isStatic) {
                error(e.loc, "method '" + e.text + "' is not static; call it on an instance of '" + className(ci) + "'");
                e.type = Type::Invalid;
            } else {
                finishClassCall(e, it->second, false);
            }
            return;
        }
        Type recv = checkValue(e.lhs, "an operand");
        for (auto& a : e.args) checkValue(a, "an argument");
        if (recv.kind == Type::Class) {
            ci = classOfType(recv);
            if (!resolveClassOverload(e, ci)) return;
            const ClassInfo& info = cinfo_[static_cast<size_t>(ci)];
            auto it = info.methods.find(e.text);
            if (it == info.methods.end()) {
                if (tryExtension(e)) return;
                error(e.loc, "class '" + className(ci) + "' has no method '" + e.text + "'");
                e.type = Type::Invalid;
            } else if (prog_.methods[static_cast<size_t>(it->second)].isStatic) {
                error(e.loc, "method '" + e.text + "' is static; call it as " + className(ci) + "." + e.text + "(...)");
                e.type = Type::Invalid;
            } else {
                finishClassCall(e, it->second, true, false, ci);
            }
            return;
        }
        if (recv.isNullable() && recv.elem->kind == Type::Class) {
            error(e.loc, typeName(recv) + " may be null; unwrap it with '!' (or test for null) before calling '" + e.text + "'");
            e.type = Type::Invalid;
            return;
        }
        if (recv == Type::Invalid) { e.type = Type::Invalid; return; }
        if (recv == Type::Dyn) {  // any method; validated at run time
            for (auto& a : e.args)
                if (a->type != Type::Invalid) coerce(a, Type::Obj);
            e.type = Type::Dyn;
            return;
        }
        const std::string& m = e.text;
        if (recv.isArray() && (m == "push" || m == "copy")) {
            size_t want = m == "push" ? 1 : 0;
            e.type = m == "push" ? Type(Type::Void) : recv;
            if (expectArgCount(e, want) && want == 1) checkArg(e, 0, *recv.elem);
            return;
        }
        if (recv.isMap()) {
            if (m == "containsKey" || m == "remove") {
                e.type = Type::Bool;
                if (expectArgCount(e, 1)) checkArg(e, 0, *recv.key);
                return;
            }
            if (m == "tryGet") {
                const Type& v = *recv.elem;
                e.type = (v.isNullable() || v.isRefLike()) ? v : Type::nullableOf(v);
                if (expectArgCount(e, 1)) checkArg(e, 0, *recv.key);
                return;
            }
            if (m == "keys" || m == "values") {
                e.type = Type::arrayOf(m == "keys" ? *recv.key : *recv.elem);
                expectArgCount(e, 0);
                return;
            }
        }
        if (tryExtension(e)) return;
        error(e.loc, typeName(recv) + " has no method '" + m + "'");
        e.type = Type::Invalid;
    }

    void checkConvert(Expr& e) {
        Type from = checkValue(e.lhs, "an operand");
        Type to = e.newType;
        e.type = to;
        if (from == Type::Invalid) { e.type = Type::Invalid; return; }
        using C = Expr::Conv;
        if (e.conv == C::Downcast) {  // value as Class
            resolveType(e.newType, e.loc);
            to = e.newType;
            e.type = to;
            if (to == Type::Invalid) return;
            const int fi = classOfType(from), ti = classOfType(to);
            if (fi < 0 || ti < 0) {
                error(e.loc, "'as' converts between class types, but " + typeName(from) + " as " + typeName(to) + " is not one");
                e.type = Type::Invalid;
            } else if (cinfo_[static_cast<size_t>(fi)].isa.count(ti)) {
                e.conv = C::Upcast;
            } else if (!cinfo_[static_cast<size_t>(ti)].isa.count(fi)) {
                error(e.loc, "cannot cast '" + className(fi) + "' to '" + className(ti) + "': they are unrelated");
                e.type = Type::Invalid;
            }
            return;
        }
        auto fail = [&]() {
            std::string hint = from.isNullable() ? " (the value may be null; use '?" "?' or '!' first)" : "";
            error(e.loc, "cannot convert " + typeName(from) + " to " + typeName(to) + hint);
            e.type = Type::Invalid;
        };
        if (from == to) { e.conv = C::Retype; return; }
        if (from.isRefLike()) { e.conv = C::Unbox; if (to == Type::String) e.conv = C::ToString; return; }
        switch (to.kind) {
            case Type::Int:
                if (from == Type::Double) e.conv = C::DoubleToInt;
                else if (from == Type::Char) e.conv = C::CharToInt;
                else if (from == Type::String) e.conv = C::StringToInt;
                else fail();
                return;
            case Type::Double:
                if (from == Type::Int) e.conv = C::IntToDouble;
                else if (from == Type::Char) {
                    e.lhs = makeConvert(std::move(e.lhs), C::CharToInt, Type::Int);
                    e.conv = C::IntToDouble;
                } else if (from == Type::String) e.conv = C::StringToDouble;
                else fail();
                return;
            case Type::String:
                if (from == Type::Int || from == Type::Bool || from == Type::Double || from == Type::Char || from == Type::Guid)
                    e.conv = C::ToString;
                else fail();
                return;
            case Type::Char:
                if (from == Type::Int) e.conv = C::IntToChar;
                else fail();
                return;
            case Type::Guid:
                if (from == Type::String) e.conv = C::StringToGuid;
                else fail();
                return;
            default: fail(); return;
        }
    }

    // The path of an imported file that declares a non-exported method `name`, or "".
    std::string notExportedFrom(const std::string& name) {
        const Module& mod = prog_.modules[static_cast<size_t>(curModule_)];
        for (int t : mod.importIndex)
            for (const MethodDecl& m : prog_.methods)
                if (t >= 0 && m.unit == t && !m.exported && writtenName(m) == name) return prog_.modules[static_cast<size_t>(t)].path;
        return "";
    }

    void checkCall(Expr& e) {
        if (e.text == "$invalid") { e.type = Type::Invalid; return; }  // generic inference already reported it
        if (ctx_->cls >= 0) {  // inside a class, a bare call may name an overloaded method of it
            const std::vector<int> ov = classOverloads(ctx_->cls, e.text);
            if (!ov.empty()) {
                preCheckArgs(e);
                std::vector<Type> ts;
                for (const auto& a : e.args) ts.push_back(a->type);
                const int mi = pickOverload(e, ov, ts, false);
                if (mi < 0) { e.type = Type::Invalid; return; }
                e.text = prog_.methods[static_cast<size_t>(mi)].name;
            }
        }
        {   // an overloaded top-level method (a class's own method of that name, resolved above, wins)
            auto top = topOvl_[static_cast<size_t>(curModule_)].find(e.text);
            if (top != topOvl_[static_cast<size_t>(curModule_)].end() && !(ctx_->cls >= 0 && cinfo_[static_cast<size_t>(ctx_->cls)].methods.count(e.text))) {
                preCheckArgs(e);
                std::vector<Type> ts;
                for (const auto& a : e.args) ts.push_back(a->type);
                const int mi = pickOverload(e, top->second, ts, false);
                if (mi < 0) { e.type = Type::Invalid; return; }
                e.text = prog_.methods[static_cast<size_t>(mi)].name;
            }
        }
        if (ctx_->cls >= 0) {  // inside a class, a bare call may name one of its methods
            const ClassInfo& info = cinfo_[static_cast<size_t>(ctx_->cls)];
            auto m = info.methods.find(e.text);
            if (m != info.methods.end()) {
                const bool isStaticMethod = prog_.methods[static_cast<size_t>(m->second)].isStatic;
                if (!isStaticMethod && ctx_->isStatic) {
                    error(e.loc, "'" + e.text + "' is an instance method and cannot be called from a static method");
                    for (auto& a : e.args) checkValue(a, "an argument");
                    e.type = Type::Invalid;
                    return;
                }
                e.kind = Expr::Kind::MethodCall;
                e.lhs = makeVar(isStaticMethod ? className(ctx_->cls) : "this", e.loc);
                checkMethodCall(e);
                return;
            }
        }
        auto it = table().find(e.text);
        const std::vector<Param>* params = nullptr;
        if (it != table().end())
            params = it->second.isMethod ? &prog_.methods[static_cast<size_t>(it->second.index)].params
                                         : &prog_.hands[static_cast<size_t>(it->second.index)].params;
        const size_t fixedParams = params ? (!params->empty() && params->back().variadic ? params->size() - 1 : params->size()) : 0;
        for (size_t i = 0; i < e.args.size(); ++i)
            checkValue(e.args[i], "an argument", params && i < fixedParams ? &(*params)[i].type : nullptr);

        if (it == table().end()) {
            std::string hidden = notExportedFrom(e.text);
            error(e.loc, hidden.empty() ? "unknown function '" + e.text + "'"
                                        : "'" + e.text + "' is not exported by '" + hidden + "' (declare it with 'export method')");
            e.type = Type::Invalid;
            return;
        }
        if (it->second.isMethod) {
            finishPlainCall(e, it->second.index);
            return;
        } else {
            e.hostIndex = it->second.index;
            e.type = prog_.hands[static_cast<size_t>(it->second.index)].ret;
        }
        if (e.args.size() != params->size()) {
            error(e.loc, "function '" + e.text + "' expects " + std::to_string(params->size()) +
                             " argument(s) but got " + std::to_string(e.args.size()));
            return;
        }
        for (size_t i = 0; i < e.args.size(); ++i) checkArg(e, i, (*params)[i].type);
    }

    void checkUnary(Expr& e) {
        Type t = checkValue(e.lhs, "an operand");
        if (t == Type::Invalid) { e.type = Type::Invalid; return; }
        if (t == Type::Dyn) { e.type = Type::Dyn; return; }
        if (e.text == "-" && isNumeric(t)) { e.type = t; return; }
        if (e.text == "!" && t == Type::Bool) { e.type = Type::Bool; return; }
        error(e.loc, "operator '" + e.text + "' cannot be applied to " + typeName(t));
        e.type = Type::Invalid;
    }

    static bool isConcatOperand(const Type& t) {
        return t == Type::Int || t == Type::String || t == Type::Bool || t == Type::Double || t == Type::Char ||
               t == Type::Guid || t == Type::Obj;
    }

    void checkCoalesce(Expr& e) {
        Type l = checkValue(e.lhs, "an operand");
        Type r = checkValue(e.rhs, "an operand", nullptr);
        if (l == Type::Invalid || r == Type::Invalid) { e.type = Type::Invalid; return; }
        if (!l.isNullable() && !l.isRefLike()) {
            error(e.loc, "operator '?" "?' needs a nullable left operand but got " + typeName(l));
            e.type = Type::Invalid;
            return;
        }
        // The result is the unwrapped type when the right side is a plain value, else the left type.
        Type inner = l.isNullable() ? *l.elem : l;
        if (coerce(e.rhs, inner)) {
            e.type = inner;
        } else if (l.isNullable() && coerce(e.rhs, l)) {
            e.type = l;
        } else {
            error(e.rhs->loc, "the right side of '?" "?' must be " + typeName(inner) + " (or " + typeName(l) + ") but is " +
                                  typeName(r));
            e.type = Type::Invalid;
        }
    }

    void checkBinary(Expr& e) {
        if (e.text == "??") { checkCoalesce(e); return; }
        Type l = checkValue(e.lhs, "an operand");
        Type r = checkValue(e.rhs, "an operand");
        if (l == Type::Invalid || r == Type::Invalid) { e.type = Type::Invalid; return; }
        const std::string& op = e.text;
        auto bad = [&]() {
            error(e.loc, "operator '" + op + "' cannot be applied to " + typeName(l) + " and " + typeName(r));
            e.type = Type::Invalid;
        };
        const bool cmp = op == "<" || op == "<=" || op == ">" || op == ">=";
        const bool eq = op == "==" || op == "!=";

        if (op == "&&" || op == "||") {
            if (coerce(e.lhs, Type::Bool) && coerce(e.rhs, Type::Bool)) e.type = Type::Bool;
            else bad();
            return;
        }

        if (eq && (l == Type::Null || r == Type::Null)) {  // x == null / x != null
            if (l == Type::Null && r == Type::Null) { error(e.loc, "cannot compare null with null"); e.type = Type::Invalid; return; }
            ExprPtr& other = l == Type::Null ? e.rhs : e.lhs;
            const Type& ot = other->type;
            if (!ot.isNullable() && !ot.isRefLike()) {
                error(e.loc, "'" + typeName(ot) + "' can never be null; declare it as '" + typeName(ot) + "?' to allow null");
                e.type = Type::Invalid;
                return;
            }
            ExprPtr keep = std::move(other);
            e.kind = Expr::Kind::IsNull;
            e.ival = op == "!=" ? 1 : 0;
            e.lhs = std::move(keep);
            e.rhs.reset();
            e.type = Type::Bool;
            return;
        }

        // Dynamic operations: resolved at run time, result is dyn.
        if ((l == Type::Dyn || r == Type::Dyn) && (cmp || eq || op == "+" || op == "-" || op == "*" || op == "/" || op == "%")) {
            if (coerce(e.lhs, Type::Dyn) && coerce(e.rhs, Type::Dyn)) e.type = Type::Dyn;
            else bad();
            return;
        }

        if (op == "+" && (l == Type::String || r == Type::String)) {
            if (isConcatOperand(l) && isConcatOperand(r)) e.type = Type::String;  // concatenation converts operands
            else bad();
            return;
        }

        if (eq && (l == Type::Obj || r == Type::Obj)) {
            if (coerce(e.lhs, Type::Obj) && coerce(e.rhs, Type::Obj)) e.type = Type::Bool;
            else bad();
            return;
        }

        if (isNumeric(l) && isNumeric(r)) {  // int op double promotes to double
            if (l != r) {
                if (!coerce(l == Type::Int ? e.lhs : e.rhs, Type::Double)) { bad(); return; }
            }
            Type common = (l == Type::Double || r == Type::Double) ? Type(Type::Double) : Type(Type::Int);
            e.type = (cmp || eq) ? Type(Type::Bool) : common;
            return;
        }

        if (cmp && l == Type::Char && r == Type::Char) { e.type = Type::Bool; return; }

        if (eq && l == r) {
            if (l.isNullable()) {
                error(e.loc, "cannot compare nullable values with '" + op + "'; compare with null or unwrap them with '!' first");
                e.type = Type::Invalid;
                return;
            }
            e.type = Type::Bool;
            return;
        }
        bad();
    }
};

} // namespace

bool analyze(Program& program, const std::string& file, std::vector<Diagnostic>& diagnostics) {
    if (program.modules.empty()) {  // a program parsed from a single file, without the module loader
        Module m;
        m.path = file;
        program.modules.push_back(std::move(m));
    }
    size_t before = diagnostics.size();
    expandGenerics(program, diagnostics);
    Sema sema(program, diagnostics);
    sema.setFirstDiag(before);
    sema.run();
    return diagnostics.size() == before;
}

} // namespace coyote
