#include "generics.hpp"

#include <functional>
#include <set>

namespace coyote {

namespace {

using Subst = std::map<std::string, Type>;

constexpr int kMaxInstances = 2000;

void substType(Type& t, const Subst* s) {
    if (!s) return;
    switch (t.kind) {
        case Type::Array: case Type::Nullable: {
            Type inner = *t.elem;
            substType(inner, s);
            t.elem = std::make_shared<const Type>(inner);
            return;
        }
        case Type::Map: {
            Type k = *t.key, v = *t.elem;
            substType(k, s);
            substType(v, s);
            t.key = std::make_shared<const Type>(k);
            t.elem = std::make_shared<const Type>(v);
            return;
        }
        case Type::Class: {
            if (t.targs.empty()) {
                auto it = s->find(t.name);
                if (it != s->end()) t = it->second;
            } else {
                for (Type& a : t.targs) substType(a, s);
            }
            return;
        }
        default: return;
    }
}

Type substituted(Type t, const Subst* s) {
    substType(t, s);
    return t;
}

ExprPtr cloneExpr(const Expr& e, const Subst* s) {
    auto n = std::make_unique<Expr>();
    n->kind = e.kind;
    n->loc = e.loc;
    n->text = e.text;
    n->ival = e.ival;
    n->dval = e.dval;
    n->conv = e.conv;
    if (e.lhs) n->lhs = cloneExpr(*e.lhs, s);
    if (e.rhs) n->rhs = cloneExpr(*e.rhs, s);
    for (const auto& a : e.args) n->args.push_back(cloneExpr(*a, s));
    n->newType = substituted(e.newType, s);
    for (const Type& t : e.typeArgs) n->typeArgs.push_back(substituted(t, s));
    n->depth = e.depth;
    return n;
}

StmtPtr cloneStmt(const Stmt& st, const Subst* s) {
    auto n = std::make_unique<Stmt>();
    n->kind = st.kind;
    n->loc = st.loc;
    if (st.expr) n->expr = cloneExpr(*st.expr, s);
    if (st.target) n->target = cloneExpr(*st.target, s);
    n->name = st.name;
    n->inferred = st.inferred;
    n->declType = substituted(st.declType, s);
    if (st.init) n->init = cloneStmt(*st.init, s);
    if (st.step) n->step = cloneStmt(*st.step, s);
    for (const auto& b : st.body) n->body.push_back(cloneStmt(*b, s));
    for (const auto& b : st.elseBody) n->elseBody.push_back(cloneStmt(*b, s));
    n->hasElse = st.hasElse;
    return n;
}

std::vector<Param> cloneParams(const std::vector<Param>& ps, const Subst* s) {
    std::vector<Param> out;
    for (const Param& p : ps) {
        Param c = p;
        substType(c.type, s);
        out.push_back(std::move(c));
    }
    return out;
}

MethodDecl cloneMethod(const MethodDecl& m, const Subst* s) {
    MethodDecl c;
    c.classIndex = m.classIndex;
    c.isStatic = m.isStatic; c.isPrivate = m.isPrivate; c.isCtor = m.isCtor; c.isExtension = m.isExtension;
    c.isStaticInit = m.isStaticInit; c.isProtected = m.isProtected; c.isOverride = m.isOverride; c.isAbstract = m.isAbstract;
    c.unit = m.unit;
    c.exported = m.exported;
    c.loc = m.loc;
    c.name = m.name;
    c.params = cloneParams(m.params, s);
    c.ret = substituted(m.ret, s);
    for (const auto& st : m.body) c.body.push_back(cloneStmt(*st, s));
    c.hasBaseCall = m.hasBaseCall;
    c.baseLoc = m.baseLoc;
    for (const auto& a : m.baseArgs) c.baseArgs.push_back(cloneExpr(*a, s));
    return c;
}

std::string mangle(const Type& t) {
    switch (t.kind) {
        case Type::Array: return "array_" + mangle(*t.elem) + "_";
        case Type::Nullable: return mangle(*t.elem) + "_opt";
        case Type::Map: return "map_" + mangle(*t.key) + "_" + mangle(*t.elem) + "_";
        case Type::Class: {
            std::string s = t.name;
            if (!t.targs.empty()) {
                s += "$";
                for (const Type& a : t.targs) s += mangle(a) + "$";
            }
            return s;
        }
        default: return typeName(t);
    }
}

class Expander {
public:
    Expander(Program& p, std::vector<Diagnostic>& out) : p_(p), out_(out) {}

    void run() {
        for (size_t i = 0; i < p_.classes.size(); ++i)
            if (!p_.classes[i].typeParams.empty()) classTpl_.emplace(p_.classes[i].name, static_cast<int>(i));
        for (size_t i = 0; i < p_.methods.size(); ++i)
            if (p_.methods[i].classIndex < 0 && !p_.methods[i].typeParams.empty()) methodTpl_.emplace(p_.methods[i].name, static_cast<int>(i));

        unit_ = 0;
        for (auto& s : p_.stmts) walkStmt(*s);
        size_t nc = 0, nm = 0;
        while (nc < p_.classes.size() || nm < p_.methods.size()) {  // instances created along the way are walked too
            while (nc < p_.classes.size()) walkClass(nc++);
            while (nm < p_.methods.size()) walkMethod(nm++);
        }
        removeTemplates();
    }

private:
    Program& p_;
    std::vector<Diagnostic>& out_;
    std::map<std::string, int> classTpl_, methodTpl_;
    std::set<std::string> madeClass_, madeMethod_;
    int unit_ = 0;
    int instances_ = 0;

    void error(SourceLoc loc, const std::string& msg) {
        const std::string& file = (static_cast<size_t>(unit_) < p_.modules.size()) ? p_.modules[static_cast<size_t>(unit_)].path : std::string();
        out_.push_back(Diagnostic{file, loc, msg});
    }

    void walkClass(size_t i) {
        if (!p_.classes[i].typeParams.empty()) return;  // a template: only its instances are walked
        unit_ = p_.classes[i].unit;
        for (size_t k = 0; k < p_.classes[i].fields.size(); ++k) {
            walkType(p_.classes[i].fields[k].type, p_.classes[i].fields[k].loc);
            if (p_.classes[i].fields[k].init) walkExpr(*p_.classes[i].fields[k].init);
        }
        for (size_t k = 0; k < p_.classes[i].properties.size(); ++k) walkType(p_.classes[i].properties[k].type, p_.classes[i].properties[k].loc);
        for (size_t k = 0; k < p_.classes[i].superTypes.size(); ++k) {
            Type t = p_.classes[i].superTypes[k];
            walkType(t, p_.classes[i].superLocs[k]);
            if (t.kind == Type::Class) p_.classes[i].superNames[k] = t.name;
            p_.classes[i].superTypes[k] = t;
        }
    }

    void walkMethod(size_t i) {
        {
            const MethodDecl& m = p_.methods[i];
            if (!m.typeParams.empty()) return;
            if (m.classIndex >= 0 && !p_.classes[static_cast<size_t>(m.classIndex)].typeParams.empty()) return;
        }
        unit_ = p_.methods[i].unit;
        for (size_t k = 0; k < p_.methods[i].params.size(); ++k) walkType(p_.methods[i].params[k].type, p_.methods[i].params[k].loc);
        walkType(p_.methods[i].ret, p_.methods[i].loc);
        for (size_t k = 0; k < p_.methods[i].body.size(); ++k) walkStmt(*p_.methods[i].body[k]);
        for (size_t k = 0; k < p_.methods[i].baseArgs.size(); ++k) walkExpr(*p_.methods[i].baseArgs[k]);
    }

    void walkStmt(Stmt& s) {
        walkType(s.declType, s.loc);
        if (s.expr) walkExpr(*s.expr);
        if (s.target) walkExpr(*s.target);
        if (s.init) walkStmt(*s.init);
        if (s.step) walkStmt(*s.step);
        for (auto& b : s.body) walkStmt(*b);
        for (auto& b : s.elseBody) walkStmt(*b);
    }

    void walkExpr(Expr& e) {
        const bool hadTypeArgs = e.kind == Expr::Kind::NewObj && e.newType.kind == Type::Class && !e.newType.targs.empty();
        walkType(e.newType, e.loc);
        if (hadTypeArgs) {
            e.text = e.newType.kind == Type::Class ? e.newType.name : "$invalid";  // new Box<int>(...); an error was already reported
        } else if (e.kind == Expr::Kind::NewObj && classTpl_.count(e.text)) {
            error(e.loc, "generic class '" + e.text + "' needs type arguments, e.g. new " + e.text + "<...>(...)");
            e.text = "$invalid";
        }
        if (e.kind == Expr::Kind::Call && !e.typeArgs.empty()) instantiateMethod(e);
        if (e.lhs) walkExpr(*e.lhs);
        if (e.rhs) walkExpr(*e.rhs);
        for (auto& a : e.args) walkExpr(*a);
    }

    void walkType(Type& t, SourceLoc loc) {
        switch (t.kind) {
            case Type::Array: case Type::Nullable: {
                Type inner = *t.elem;
                walkType(inner, loc);
                t.elem = std::make_shared<const Type>(inner);
                return;
            }
            case Type::Map: {
                Type k = *t.key, v = *t.elem;
                walkType(k, loc);
                walkType(v, loc);
                t.key = std::make_shared<const Type>(k);
                t.elem = std::make_shared<const Type>(v);
                return;
            }
            case Type::Class:
                if (!t.targs.empty()) instantiateClass(t, loc);
                else if (classTpl_.count(t.name)) {
                    error(loc, "generic class '" + t.name + "' needs type arguments (e.g. " + t.name + "<" + p_.classes[static_cast<size_t>(classTpl_[t.name])].typeParams[0] + ">)");
                    t = Type::Invalid;
                }
                return;
            default: return;
        }
    }

    void instantiateClass(Type& t, SourceLoc loc) {
        for (Type& a : t.targs) walkType(a, loc);
        auto it = classTpl_.find(t.name);
        if (it == classTpl_.end()) {
            error(loc, "'" + t.name + "' is not a generic class, so it takes no type arguments");
            t = Type::Invalid;
            return;
        }
        const size_t tpl = static_cast<size_t>(it->second);
        const size_t want = p_.classes[tpl].typeParams.size();
        if (want != t.targs.size()) {
            error(loc, "generic class '" + t.name + "' takes " + std::to_string(want) + " type argument(s) but got " + std::to_string(t.targs.size()));
            t = Type::Invalid;
            return;
        }
        for (const Type& a : t.targs)
            if (a == Type::Invalid || a == Type::Void) { t = Type::Invalid; return; }
        const std::string mangled = mangle(t);
        if (!madeClass_.count(mangled)) {
            if (++instances_ > kMaxInstances) {
                error(loc, "too many generic instantiations (is a generic class instantiating itself with a growing type?)");
                t = Type::Invalid;
                return;
            }
            madeClass_.insert(mangled);
            genericDisplayNames()[mangled] = typeName(t);
            Subst s;
            for (size_t i = 0; i < want; ++i) s.emplace(p_.classes[tpl].typeParams[i], t.targs[i]);
            ClassDecl c;
            {
                const ClassDecl& o = p_.classes[tpl];
                c.unit = o.unit; c.exported = o.exported; c.isModel = o.isModel; c.isInterface = o.isInterface;
                c.loc = o.loc;
                c.name = mangled;
                c.superNames = o.superNames;
                c.superLocs = o.superLocs;
                for (const Type& st : o.superTypes) c.superTypes.push_back(substituted(st, &s));
                for (const FieldDecl& f : o.fields) {
                    FieldDecl nf;
                    nf.loc = f.loc; nf.name = f.name; nf.type = substituted(f.type, &s);
                    if (f.init) nf.init = cloneExpr(*f.init, &s);
                    nf.isPrivate = f.isPrivate; nf.isProtected = f.isProtected; nf.isStatic = f.isStatic;
                    c.fields.push_back(std::move(nf));
                }
                for (const PropertyDecl& pr : o.properties) {
                    PropertyDecl np = pr;
                    np.type = substituted(pr.type, &s);
                    c.properties.push_back(std::move(np));
                }
            }
            p_.classes.push_back(std::move(c));
            const int idx = static_cast<int>(p_.classes.size()) - 1;
            const size_t existing = p_.methods.size();
            for (size_t i = 0; i < existing; ++i) {
                if (p_.methods[i].classIndex != static_cast<int>(tpl)) continue;
                MethodDecl m = cloneMethod(p_.methods[i], &s);
                m.classIndex = idx;
                p_.methods.push_back(std::move(m));
            }
        }
        t = Type::classOf(mangled);
    }

    void instantiateMethod(Expr& e) {
        for (Type& a : e.typeArgs) walkType(a, e.loc);
        auto it = methodTpl_.find(e.text);
        if (it == methodTpl_.end()) {
            error(e.loc, "'" + e.text + "' is not a generic method, so it takes no type arguments");
            e.typeArgs.clear();
            return;
        }
        const size_t tpl = static_cast<size_t>(it->second);
        const size_t want = p_.methods[tpl].typeParams.size();
        if (want != e.typeArgs.size()) {
            error(e.loc, "generic method '" + e.text + "' takes " + std::to_string(want) + " type argument(s) but got " + std::to_string(e.typeArgs.size()));
            e.typeArgs.clear();
            return;
        }
        std::string mangled = e.text + "$";
        for (const Type& a : e.typeArgs) {
            if (a == Type::Invalid || a == Type::Void) { e.typeArgs.clear(); return; }
            mangled += mangle(a) + "$";
        }
        if (!madeMethod_.count(mangled)) {
            if (++instances_ > kMaxInstances) {
                error(e.loc, "too many generic instantiations");
                e.typeArgs.clear();
                return;
            }
            madeMethod_.insert(mangled);
            Subst s;
            for (size_t i = 0; i < want; ++i) s.emplace(p_.methods[tpl].typeParams[i], e.typeArgs[i]);
            MethodDecl m = cloneMethod(p_.methods[tpl], &s);
            m.name = mangled;
            m.loc = p_.methods[tpl].loc;
            p_.methods.push_back(std::move(m));
        }
        e.text = mangled;
        e.typeArgs.clear();
    }

    void removeTemplates() {
        std::vector<int> remap(p_.classes.size(), -1);
        std::vector<ClassDecl> kept;
        for (size_t i = 0; i < p_.classes.size(); ++i) {
            if (!p_.classes[i].typeParams.empty()) continue;
            remap[i] = static_cast<int>(kept.size());
            kept.push_back(std::move(p_.classes[i]));
        }
        p_.classes = std::move(kept);
        std::vector<MethodDecl> keptMethods;
        for (MethodDecl& m : p_.methods) {
            if (!m.typeParams.empty()) continue;
            if (m.classIndex >= 0) {
                if (remap[static_cast<size_t>(m.classIndex)] < 0) continue;
                m.classIndex = remap[static_cast<size_t>(m.classIndex)];
            }
            keptMethods.push_back(std::move(m));
        }
        p_.methods = std::move(keptMethods);
    }
};

} // namespace

void expandGenerics(Program& program, std::vector<Diagnostic>& diagnostics) {
    Expander(program, diagnostics).run();
}

} // namespace coyote
