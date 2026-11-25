#include <cstring>
#include <cctype>
#include "mlgen.hpp"

#include <cstdio>
#include <functional>
#include <set>
#include <sstream>

namespace coyote {

namespace {

std::string quote(const std::string& s) {
    std::string r = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '\\': r += "\\\\"; break;
            case '"': r += "\\\""; break;
            case '\n': r += "\\n"; break;
            case '\t': r += "\\t"; break;
            case '\r': r += "\\r"; break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\x%02X", c);
                    r += buf;
                } else {
                    r += static_cast<char>(c);  // printable ASCII and UTF-8 bytes pass through
                }
        }
    }
    return r + "\"";
}

std::string sigText(const std::vector<Param>& params, Type ret) {
    std::string s = "(";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) s += ", ";
        s += mlTypeName(params[i].type);
    }
    return s + ") -> " + mlTypeName(ret);
}

class Gen {
public:
    Gen(const Program& p, bool removeUnused) : prog_(p), dce_(removeUnused) {}

    std::string run(const std::string& sourcePath) {
        markLive();
        int version = kMlVersion;
        {
            std::function<bool(const Stmt&)> hasTry = [&](const Stmt& s) {
                if (s.kind == Stmt::Kind::Try || s.kind == Stmt::Kind::Throw) return true;
                for (const auto& b : s.finallyBody) if (hasTry(*b)) return true;
                if (s.init && hasTry(*s.init)) return true;
                for (const auto& b : s.body) if (hasTry(*b)) return true;
                for (const auto& b : s.elseBody) if (hasTry(*b)) return true;
                return false;
            };
            bool any = false;
            for (const auto& s : prog_.stmts) any = any || hasTry(*s);
            for (const MethodDecl& m : prog_.methods) for (const auto& s : m.body) any = any || hasTry(*s);
            if (any) version = 8;  // try/throw need ML 8
        }
        for (const ClassDecl& c : prog_.classes)
            if (!c.supers.empty()) version = std::max(version, 7);  // class hierarchies need ML 7
        os_ << "ml " << version << "\n";
        os_ << "source " << quote(sourcePath) << "\n";
        std::set<std::string> hosts;  // several files may declare the same host function
        for (const HandDecl& h : prog_.hands)
            if (hosts.insert(h.module + "." + h.name).second)
                os_ << "host " << h.module << "." << h.name << " " << sigText(h.params, h.ret) << "\n";
        for (const ClassDecl& c : prog_.classes) {
            os_ << "class " << classMlName(c);
            if (!c.supers.empty()) {
                os_ << " extends";
                for (int sidx : c.supers) os_ << " " << classMlName(prog_.classes[static_cast<size_t>(sidx)]);
            }
            os_ << "\n";
            for (const FieldDecl& f : c.fields)
                if (!f.isStatic) os_ << "  field " << f.name << " " << mlTypeName(f.type) << "\n";
            os_ << "end\n";
            for (const FieldDecl& f : c.fields)
                if (f.isStatic) os_ << "global " << classMlName(c) << "." << f.name << " " << mlTypeName(f.type) << "\n";
        }
        for (size_t mi = 0; mi < prog_.methods.size(); ++mi) {
            const MethodDecl& m = prog_.methods[mi];
            if (m.isAbstract || !live_[mi]) continue;  // interface signature (no code) / unreachable method
            if (m.isDispatcher) { dispatcher(m); continue; }
            function(fnName(m), sigText(m.params, m.ret), m.ret, m.localTypes, m.body, m.unit, &m);
        }
        for (const auto& g : prog_.globals) os_ << "global $G." << g.first << " " << mlTypeName(g.second) << "\n";
        function("$main", "() -> void", Type::Void, prog_.mainLocals, prog_.stmts, 0, nullptr);
        os_ << "entry $main\n";
        std::string out = os_.str();
        if (!usedMouths_.empty()) {  // mouth declarations go right after the 'source' line; they need ML 10
            std::string decls;
            for (const auto& [name, rw] : usedMouths_) decls += "mouth " + name + (rw.first ? " get" : "") + (rw.second ? " set" : "") + "\n";
            const size_t srcEnd = out.find('\n', out.find('\n') + 1);
            out.insert(srcEnd + 1, decls);
            out.replace(0, out.find('\n'), "ml 10");
            version = 10;
        } else if (out.find("\n  ocopy\n") != std::string::npos) {  // (copy) of an object or map needs ML 9
            out.replace(0, out.find('\n'), "ml 9");
            version = 9;
        }
        {   // long, byte and their operations (and string indexing / comparison) need ML 11
            static const std::set<std::string> v11 = {"str.at", "str.len", "lt.str", "le.str", "gt.str", "ge.str", "push.lng", "i2l", "l2i", "l2d", "d2l",
                "str.lng", "parse.lng", "i2b", "b2i", "bnot.int", "bnot.lng"};
            bool need = false;
            std::istringstream in(out);
            std::string line;
            std::getline(in, line);
            std::getline(in, line);  // 'ml' and 'source' lines
            while (!need && std::getline(in, line)) {
                const size_t b0 = line.find_first_not_of(' ');
                if (b0 == std::string::npos || line.compare(b0, 8, "push.str") == 0) continue;
                const size_t e0 = line.find_first_of(" \t", b0);
                const std::string op = line.substr(b0, e0 == std::string::npos ? std::string::npos : e0 - b0);
                if (v11.count(op)) { need = true; break; }
                const size_t dot = op.rfind('.');
                if (dot != std::string::npos) {
                    const std::string head = op.substr(0, dot), tail = op.substr(dot + 1);
                    if (tail == "lng" || (tail == "int" && (head == "and" || head == "or" || head == "xor" || head == "shl" || head == "shr"))) { need = true; break; }
                }
                for (const char* w : {"long", "byte"}) {
                    for (size_t at = line.find(w); at != std::string::npos; at = line.find(w, at + 1)) {
                        const auto isId = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.'; };
                        if ((at == 0 || !isId(line[at - 1])) && (at + std::strlen(w) >= line.size() || !isId(line[at + std::strlen(w)]))) need = true;
                    }
                }
            }
            if (need) {
                out.replace(0, out.find('\n'), "ml 11");
                version = 11;
            }
        }
        if (out.find("\n  throwx\n") != std::string::npos) {  // exception objects (`throw obj`, `finally`) need ML 12
            out.replace(0, out.find('\n'), "ml 12");
            version = 12;
        }
        if (version < 6) {  // function values need ML 6
            bool fn = false;
            std::istringstream in(out);
            for (std::string line; std::getline(in, line) && !fn;) {
                const size_t b = line.find_first_not_of(' ');
                if (b == std::string::npos || line.compare(b, 8, "push.str") == 0) continue;
                fn = line.compare(b, 10, "mkclosure ") == 0 || line.compare(b, 7, "callfn ") == 0 || line.find("fn(") != std::string::npos;
            }
            if (fn) out.replace(0, out.find('\n'), "ml 6");
        }
        return out;
    }

private:
    const Program& prog_;
    bool dce_;
    std::vector<char> live_;       // per method: reachable from the top-level code
    std::ostringstream os_;
    std::map<std::string, std::pair<bool, bool>> usedMouths_;  // "module.name" -> (read, written): declared in the ML header (version 10)
    int curLine_ = 0;
    int labels_ = 0;

    // Dead-code removal: marks the methods reachable from the top-level code and the static initializers.
    void markLive() {
        live_.assign(prog_.methods.size(), dce_ ? 0 : 1);
        if (!dce_) return;
        std::vector<size_t> work;
        auto mark = [&](int mi) {
            if (mi < 0 || live_[static_cast<size_t>(mi)]) return;
            live_[static_cast<size_t>(mi)] = 1;
            work.push_back(static_cast<size_t>(mi));
        };
        std::function<void(const Expr&)> ex = [&](const Expr& e) {
            if (e.kind == Expr::Kind::Call || e.kind == Expr::Kind::MethodCall || e.kind == Expr::Kind::NewObj || e.kind == Expr::Kind::Member || e.kind == Expr::Kind::Lambda)
                mark(e.methodIndex);
            if (e.lhs) ex(*e.lhs);
            if (e.rhs) ex(*e.rhs);
            for (const auto& a : e.args) ex(*a);
        };
        std::function<void(const Stmt&)> st = [&](const Stmt& s) {
            if (s.expr) ex(*s.expr);
            if (s.target) ex(*s.target);
            if (s.msgExpr) ex(*s.msgExpr);
            if (s.traceExpr) ex(*s.traceExpr);
            if (s.init) st(*s.init);
            if (s.step) st(*s.step);
            for (const auto& b : s.body) st(*b);
            for (const auto& b : s.elseBody) st(*b);
            for (const auto& b : s.finallyBody) st(*b);
        };
        for (const auto& s : prog_.stmts) st(*s);
        for (size_t i = 0; i < prog_.methods.size(); ++i)
            if (prog_.methods[i].isStaticInit) mark(static_cast<int>(i));
        while (!work.empty()) {
            const MethodDecl& m = prog_.methods[work.back()];
            work.pop_back();
            for (const auto& s : m.body) st(*s);
            mark(m.initMethod);
            for (const auto& c : m.cases) mark(c.second);
            mark(m.defaultImpl);
        }
    }

    // ML name of a method. Methods of imported files get a per-file prefix so private methods in
    // different files may share a name.
    std::string fnName(const MethodDecl& m) const {
        if (m.classIndex >= 0) return classMlName(prog_.classes[static_cast<size_t>(m.classIndex)]) + "." + m.name;
        return m.unit == 0 ? m.name : "m" + std::to_string(m.unit) + "." + m.name;
    }

    void function(const std::string& name, const std::string& sig, Type ret,
                  const std::vector<Type>& locals, const std::vector<StmtPtr>& body, int unit, const MethodDecl* method) {
        curLine_ = 0;
        labels_ = 0;
        os_ << "func " << name << " " << sig << "\n";
        if (unit != 0 && static_cast<size_t>(unit) < prog_.modules.size())
            os_ << "  .file " << quote(prog_.modules[static_cast<size_t>(unit)].path) << "\n";
        for (Type t : locals) os_ << "  local " << mlTypeName(t) << "\n";
        if (method)
            for (size_t i = 0; i < method->params.size(); ++i) os_ << "  .var " << i << " " << quote(method->params[i].name) << "\n";
        {
            const std::vector<std::string>& names = method ? method->localNames : prog_.mainLocalNames;
            const size_t base = method ? method->params.size() : 0;
            for (size_t i = 0; i < names.size(); ++i) os_ << "  .var " << base + i << " " << quote(names[i]) << "\n";
        }
        if (!method) {  // top-level code: static initializers run first
            for (const MethodDecl& m : prog_.methods)
                if (m.isStaticInit) os_ << "  call " << fnName(m) << " 0\n";
        }
        if (method && method->isCtor) {  // allocate the object; the epilogue returns it
            os_ << "  newobj " << mlTypeName(method->ret) << "\n  store " << method->thisSlot << "\n";
            if (method->initMethod >= 0) {  // hierarchy class: the constructor body lives in `$init`
                os_ << "  load " << method->thisSlot << "\n";
                for (size_t i = 0; i < method->params.size(); ++i) os_ << "  load " << i << "\n";
                os_ << "  call " << fnName(prog_.methods[static_cast<size_t>(method->initMethod)]) << " " << method->params.size() + 1 << "\n";
            }
        }
        for (const auto& s : body) stmt(*s);
        if (method && method->isCtor) {
            os_ << "  load " << method->thisSlot << "\n  ret\nend\n";
            return;
        }
        // Fall-through return. Semantic analysis guarantees non-void methods return on every
        // path, so for them this tail is unreachable; it keeps the ML structurally complete.
        defaultReturn(ret);
        os_ << "end\n";
    }

    // The ML of one dispatcher: tests the receiver's class (most derived first) and calls the implementation.
    void dispatcher(const MethodDecl& d) {
        const std::string self = mlTypeName(d.params[0].type);
        os_ << "func " << fnName(d) << " " << sigText(d.params, d.ret) << "\n";
        for (size_t i = 0; i < d.params.size(); ++i) os_ << "  .var " << i << " " << quote(i == 0 ? "this" : d.params[i].name) << "\n";
        auto callImpl = [&](int impl, const std::string& from) {
            const MethodDecl& y = prog_.methods[static_cast<size_t>(impl)];
            const std::string owner = classMlName(prog_.classes[static_cast<size_t>(y.classIndex)]);
            if (owner != from) os_ << "  upcast " << owner << "\n";
            for (size_t i = 1; i < d.params.size(); ++i) os_ << "  load " << i << "\n";
            os_ << "  call " << fnName(y) << " " << d.params.size() << "\n  ret\n";
        };
        for (const auto& [cls, impl] : d.cases) {
            const std::string next = newLabel();
            const std::string cname = classMlName(prog_.classes[static_cast<size_t>(cls)]);
            os_ << "  load 0\n  isinst " << cname << "\n  jump.false " << next << "\n  load 0\n  downcast " << cname << "\n";
            callImpl(impl, cname);
            os_ << "label " << next << "\n";
        }
        if (d.defaultImpl >= 0) {
            os_ << "  load 0\n";
            callImpl(d.defaultImpl, self);
        } else {
            defaultReturn(d.ret);
        }
        os_ << "end\n";
    }

    void defaultReturn(Type ret) {
        switch (ret.kind) {
            case Type::Int: os_ << "  push.int 0\n"; break;
            case Type::Long: os_ << "  push.lng 0\n"; break;
            case Type::Byte: os_ << "  push.int 0\n  i2b\n"; break;
            case Type::String: os_ << "  push.str \"\"\n"; break;
            case Type::Bool: os_ << "  push.bool false\n"; break;
            case Type::Double: os_ << "  push.dbl 0\n"; break;
            case Type::Char: os_ << "  push.chr 0\n"; break;
            case Type::Guid:
                os_ << "  push.str \"00000000-0000-0000-0000-000000000000\"\n  parse.guid\n";
                break;
            case Type::Nullable: case Type::Obj: case Type::Dyn: os_ << "  push.null " << mlTypeName(ret) << "\n"; break;
            default: break;
        }
        os_ << "  ret\n";
    }

    // Active `try` ops of the function being generated, innermost last: fin != null marks the protection of a `finally` block, which
    // is copied inline wherever control leaves it (normal end, return, break, continue; the exceptional path has its own copy).
    struct TryScope { const Stmt* fin; };
    std::vector<TryScope> scopes_;
    std::vector<size_t> loopTry_;  // scopes_.size() when each enclosing loop was entered
    void emitFinally(const Stmt& t) { for (const auto& c : t.finallyBody) stmt(*c); }
    void leaveTries(size_t downTo) {
        const std::vector<TryScope> saved = scopes_;
        for (size_t i = saved.size(); i > downTo; --i) {
            scopes_.resize(i - 1);  // a finally body runs outside its own protection
            os_ << "  endtry\n";
            if (saved[i - 1].fin) emitFinally(*saved[i - 1].fin);
        }
        scopes_ = saved;
    }
    bool hasFinallyScope() const { for (const TryScope& s : scopes_) if (s.fin) return true; return false; }
    std::vector<std::pair<std::string, std::string>> loops_;  // (continue label, break label) of enclosing loops

    std::string newLabel() { return "L" + std::to_string(labels_++); }

    void setLine(int line) {
        if (line != curLine_) {
            os_ << ".line " << line << "\n";
            curLine_ = line;
        }
    }

    void stmt(const Stmt& s) {
        // Every statement starts a source line, so the debugger can stop on it. (Instructions that can
        // fail still set their own, more precise line.)
        if (s.kind != Stmt::Kind::Block && s.loc.line > 0) setLine(s.loc.line);
        switch (s.kind) {
            case Stmt::Kind::ExprStmt:
                expr(*s.expr);
                if (s.expr->type != Type::Void) os_ << "  pop\n";
                return;
            case Stmt::Kind::VarDecl:
                expr(*s.expr);
                os_ << "  store " << s.slot << "\n";
                return;
            case Stmt::Kind::Assign:
                if (s.target->kind == Expr::Kind::Var && s.target->mouthIndex >= 0) {
                    const MouthDecl& md = prog_.mouths[static_cast<size_t>(s.target->mouthIndex)];
                    usedMouths_[md.module + "." + md.name].second = true;
                    expr(*s.expr);
                    os_ << "  setmouth " << md.module << "." << md.name << "\n";
 } else if (s.target->kind == Expr::Kind::Var && s.target->isGlobal) {
                    expr(*s.expr);
                    os_ << "  setglobal $G." << s.target->text << "\n";
                } else if (s.target->kind == Expr::Kind::Var) {
                    expr(*s.expr);
                    os_ << "  store " << s.target->slot << "\n";
                } else if (s.target->kind == Expr::Kind::Member) {  // class field (sema turned property writes into calls)
                    const Expr& t = *s.target;
                    const std::string fld = classMlName(prog_.classes[static_cast<size_t>(t.classIndex)]) + "." + t.text;
                    if (t.isStatic) {
                        expr(*s.expr);
                        os_ << "  setglobal " << fld << "\n";
                    } else {
                        expr(*t.lhs);
                        expr(*s.expr);
                        setLine(t.loc.line);
                        os_ << "  setfield " << fld << "\n";
                    }
                } else {  // element: collection, index/key, value
                    expr(*s.target->lhs);
                    expr(*s.target->rhs);
                    expr(*s.expr);
                    setLine(s.target->loc.line);
                    const Type& ct = s.target->lhs->type;
                    os_ << (ct.isMap() ? "  mapset\n" : ct == Type::Dyn ? "  dyn.setindex\n" : "  astore\n");
                }
                return;
            case Stmt::Kind::If: {
                std::string elseL = newLabel();
                expr(*s.expr);
                os_ << "  jump.false " << elseL << "\n";
                for (const auto& c : s.body) stmt(*c);
                if (s.hasElse) {
                    std::string endL = newLabel();
                    os_ << "  jump " << endL << "\n";
                    os_ << "label " << elseL << "\n";
                    for (const auto& c : s.elseBody) stmt(*c);
                    os_ << "label " << endL << "\n";
                } else {
                    os_ << "label " << elseL << "\n";
                }
                return;
            }
            case Stmt::Kind::While: {
                std::string top = newLabel(), end = newLabel();
                os_ << "label " << top << "\n";
                expr(*s.expr);
                os_ << "  jump.false " << end << "\n";
                loops_.push_back({top, end}); loopTry_.push_back(scopes_.size());
                for (const auto& c : s.body) stmt(*c);
                loops_.pop_back(); loopTry_.pop_back();
                os_ << "  jump " << top << "\n";
                os_ << "label " << end << "\n";
                return;
            }
            case Stmt::Kind::For: {
                std::string top = newLabel(), cont = newLabel(), end = newLabel();
                if (s.init) stmt(*s.init);
                os_ << "label " << top << "\n";
                if (s.expr) {
                    expr(*s.expr);
                    os_ << "  jump.false " << end << "\n";
                }
                loops_.push_back({cont, end}); loopTry_.push_back(scopes_.size());
                for (const auto& c : s.body) stmt(*c);
                loops_.pop_back(); loopTry_.pop_back();
                os_ << "label " << cont << "\n";  // 'continue' lands here, then the step runs
                if (s.step) stmt(*s.step);
                os_ << "  jump " << top << "\n";
                os_ << "label " << end << "\n";
                return;
            }
            case Stmt::Kind::ForEach: {
                std::string top = newLabel(), cont = newLabel(), end = newLabel();
                expr(*s.expr);
                os_ << "  store " << s.slotArr << "\n";
                os_ << "  push.int 0\n  store " << s.slotIdx << "\n";
                os_ << "label " << top << "\n";
                os_ << "  load " << s.slotIdx << "\n  load " << s.slotArr << "\n";
                setLine(s.loc.line);
                const bool overString = s.expr->type == Type::String;
                os_ << (overString ? "  str.len\n" : "  alen\n") << "  lt.int\n  jump.false " << end << "\n";
                os_ << "  load " << s.slotArr << "\n  load " << s.slotIdx << (overString ? "\n  str.at\n  store " : "\n  aload\n  store ") << s.slot << "\n";
                loops_.push_back({cont, end}); loopTry_.push_back(scopes_.size());
                for (const auto& c : s.body) stmt(*c);
                loops_.pop_back(); loopTry_.pop_back();
                os_ << "label " << cont << "\n";
                os_ << "  load " << s.slotIdx << "\n  push.int 1\n  add.int\n  store " << s.slotIdx << "\n";
                os_ << "  jump " << top << "\n";
                os_ << "label " << end << "\n";
                return;
            }
            case Stmt::Kind::Try: {
                std::string finHandler, finEnd;
                if (s.hasFinally) {
                    finHandler = newLabel();
                    finEnd = newLabel();
                    os_ << "  try " << finHandler << "\n";
                    scopes_.push_back({&s});
                }
                if (s.hasElse) {
                    std::string handler = newLabel(), end = newLabel();
                    os_ << "  try " << handler << "\n";
                    scopes_.push_back({nullptr});
                    for (const auto& c : s.body) stmt(*c);
                    scopes_.pop_back();
                    os_ << "  endtry\n  jump " << end << "\n";
                    os_ << "label " << handler << "\n";
                    os_ << "  store " << s.slot << "\n";
                    for (const auto& c : s.elseBody) stmt(*c);
                    os_ << "label " << end << "\n";
                } else {
                    for (const auto& c : s.body) stmt(*c);
                }
                if (s.hasFinally) {
                    scopes_.pop_back();
                    os_ << "  endtry\n";
                    emitFinally(s);  // the normal way out
                    os_ << "  jump " << finEnd << "\nlabel " << finHandler << "\n";
                    // an exception got out of the try/catch blocks: keep it, run the finally block, raise it again
                    usedMouths_["sys.exceptionObject"].first = true;
                    usedMouths_["sys.stackTrace"].first = true;
                    os_ << "  store " << s.slotMsg << "\n  getmouth sys.exceptionObject\n  store " << s.slotPay << "\n"
                        << "  getmouth sys.stackTrace\n  unbox string\n  store " << s.slotTrace << "\n";
                    emitFinally(s);
                    os_ << "  load " << s.slotPay << "\n  load " << s.slotMsg << "\n  load " << s.slotTrace << "\n  throwx\n";
                    os_ << "label " << finEnd << "\n";
                }
                return;
            }
            case Stmt::Kind::Throw:
                // sema turned the thrown Exception object into: temp declaration (init), object, message, stack trace
                stmt(*s.init);
                expr(*s.expr);
                expr(*s.msgExpr);
                expr(*s.traceExpr);
                os_ << "  throwx\n";
                return;
            case Stmt::Kind::Break:
                leaveTries(static_cast<size_t>(loopTry_.back()));
                os_ << "  jump " << loops_.back().second << "\n";
                return;
            case Stmt::Kind::Continue:
                leaveTries(static_cast<size_t>(loopTry_.back()));
                os_ << "  jump " << loops_.back().first << "\n";
                return;
            case Stmt::Kind::Block:
                for (const auto& c : s.body) stmt(*c);
                return;
            case Stmt::Kind::Return:
                if (hasFinallyScope()) {  // the value waits in a hidden local while the finally blocks run
                    if (s.expr) { expr(*s.expr); os_ << "  store " << s.slot << "\n"; }
                    leaveTries(0);
                    if (s.expr) os_ << "  load " << s.slot << "\n";
                    os_ << "  ret\n";
                    return;
                }
                if (s.expr) expr(*s.expr);
                os_ << "  ret\n";
                return;
        }
    }

    // Converts the value on top of the stack to a string (for concatenation).
    void toString(Type t) {
        if (t == Type::Int) os_ << "  str.int\n";
        else if (t == Type::Long) os_ << "  str.lng\n";
        else if (t == Type::Byte) os_ << "  b2i\n  str.int\n";
        else if (t == Type::Bool) os_ << "  str.bool\n";
        else if (t == Type::Double) os_ << "  str.dbl\n";
        else if (t == Type::Char) os_ << "  str.chr\n";
        else if (t == Type::Guid) os_ << "  str.guid\n";
        else if (t.isRefLike()) os_ << "  tostr.obj\n";
    }

    void expr(const Expr& e) {
        switch (e.kind) {
            case Expr::Kind::String: os_ << "  push.str " << quote(e.text) << "\n"; return;
            case Expr::Kind::Int: os_ << (e.isLong ? "  push.lng " : "  push.int ") << e.ival << "\n"; return;
            case Expr::Kind::Bool: os_ << "  push.bool " << (e.ival ? "true" : "false") << "\n"; return;
            case Expr::Kind::Double: {
                char buf[40];
                std::snprintf(buf, sizeof buf, "%.17g", e.dval);
                os_ << "  push.dbl " << buf << "\n";
                return;
            }
            case Expr::Kind::Char: os_ << "  push.chr " << e.ival << "\n"; return;
            case Expr::Kind::Null: os_ << "  push.null " << mlTypeName(e.type) << "\n"; return;
            case Expr::Kind::NewMap: os_ << "  newmap " << mlTypeName(e.type) << "\n"; return;
            case Expr::Kind::Unwrap:
                expr(*e.lhs);
                setLine(e.loc.line);
                os_ << "  unwrap\n";
                return;
            case Expr::Kind::IsNull:
                expr(*e.lhs);
                os_ << "  isnull\n";
                if (e.ival) os_ << "  not\n";
                return;
            case Expr::Kind::Is:
                expr(*e.lhs);
                os_ << (e.lhs->type.kind == Type::Class ? "  isinst " : "  istype ") << mlTypeName(e.newType) << "\n";
                return;
            case Expr::Kind::Convert: convert(e); return;
            case Expr::Kind::Var:
                if (e.mouthIndex >= 0) {
                    const MouthDecl& md = prog_.mouths[static_cast<size_t>(e.mouthIndex)];
                    usedMouths_[md.module + "." + md.name].first = true;
                    os_ << "  getmouth " << md.module << "." << md.name << "\n";
                    return;
                }
                if (e.isGlobal) { os_ << "  getglobal $G." << e.text << "\n"; return; }
                os_ << "  load " << e.slot << "\n";
                return;
            case Expr::Kind::Lambda:  // closure over the captured values (none for a plain method reference)
                for (const auto& a : e.args) expr(*a);
                os_ << "  mkclosure " << fnName(prog_.methods[static_cast<size_t>(e.methodIndex)]) << " " << e.args.size() << "\n";
                return;
            case Expr::Kind::Switch: {
                expr(*e.lhs);
                os_ << "  store " << e.slot << "\n";
                const std::string end = newLabel();
                std::vector<std::string> armL;
                std::vector<size_t> resIdx;
                std::string defL;
                size_t idx = 0;
                for (const int n : e.armSizes) {
                    armL.push_back(newLabel());
                    for (int j = 0; j < n; ++j) {
                        const std::string next = newLabel();
                        expr(*e.args[idx++]);
                        os_ << "  jump.false " << next << "\n  jump " << armL.back() << "\nlabel " << next << "\n";
                    }
                    if (n == 0) defL = armL.back();
                    resIdx.push_back(idx++);
                }
                if (defL.empty()) {
                    setLine(e.loc.line);
                    os_ << "  push.str \"no switch arm matched the value\"\n  throw\n";
                } else {
                    os_ << "  jump " << defL << "\n";
                }
                for (size_t k = 0; k < armL.size(); ++k) {
                    os_ << "label " << armL[k] << "\n";
                    expr(*e.args[resIdx[k]]);
                    os_ << "  jump " << end << "\n";
                }
                os_ << "label " << end << "\n";
                return;
            }
            case Expr::Kind::CallFn:
                expr(*e.lhs);
                for (const auto& a : e.args) expr(*a);
                setLine(e.loc.line);
                os_ << "  callfn " << e.args.size() << "\n";
                return;
            case Expr::Kind::Call: {
                for (const auto& a : e.args) expr(*a);
                setLine(e.loc.line);
                if (e.methodIndex >= 0) {
                    os_ << "  call " << fnName(prog_.methods[static_cast<size_t>(e.methodIndex)]) << " "
                        << e.args.size() << "\n";
                } else {
                    const HandDecl& h = prog_.hands[static_cast<size_t>(e.hostIndex)];
                    os_ << "  hostcall " << h.module << "." << h.name << " " << e.args.size() << "\n";
                }
                return;
            }
            case Expr::Kind::Unary:
                expr(*e.lhs);
                setLine(e.loc.line);
                if (e.text == "(copy)") {
                    if (e.lhs->type.kind != Type::Obj) os_ << (e.lhs->type.isArray() ? "  acopy\n" : "  ocopy\n");
                    return;
                }
                if (e.lhs->type == Type::Dyn) os_ << (e.text == "-" ? "  dyn.neg\n" : "  dyn.not\n");
                else if (e.text == "-") os_ << (e.lhs->type == Type::Double ? "  neg.dbl\n" : e.lhs->type == Type::Long ? "  neg.lng\n" : "  neg.int\n");
                else if (e.text == "~") os_ << (e.lhs->type == Type::Long ? "  bnot.lng\n" : "  bnot.int\n");
                else os_ << "  not\n";
                return;
            case Expr::Kind::Binary: binary(e); return;
            case Expr::Kind::ArrayLit:
                for (const auto& a : e.args) expr(*a);
                os_ << "  arraylit " << mlTypeName(*e.type.elem) << " " << e.args.size() << "\n";
                return;
            case Expr::Kind::NewArray:
                expr(*e.lhs);
                setLine(e.loc.line);
                os_ << "  newarray " << mlTypeName(*e.type.elem) << "\n";
                return;
            case Expr::Kind::Index:
                expr(*e.lhs);
                expr(*e.rhs);
                setLine(e.loc.line);
                os_ << (e.lhs->type.isMap() ? "  mapget\n" : e.lhs->type == Type::Dyn ? "  dyn.index\n" : e.lhs->type == Type::String ? "  str.at\n" : "  aload\n");
                return;
            case Expr::Kind::NewObj: {
                for (const auto& a : e.args) expr(*a);
                setLine(e.loc.line);
                os_ << "  call " << fnName(prog_.methods[static_cast<size_t>(e.methodIndex)]) << " " << e.args.size() << "\n";
                return;
            }
            case Expr::Kind::Member:
                if (e.classIndex >= 0) {  // class field
                    const std::string fld = classMlName(prog_.classes[static_cast<size_t>(e.classIndex)]) + "." + e.text;
                    if (e.isStatic) {
                        os_ << "  getglobal " << fld << "\n";
                    } else {
                        expr(*e.lhs);
                        setLine(e.loc.line);
                        os_ << "  getfield " << fld << "\n";
                    }
                    return;
                }
                expr(*e.lhs);
                setLine(e.loc.line);
                if (e.lhs->type == Type::Dyn) os_ << "  dyn.member " << quote(e.text) << "\n";
                else os_ << (e.lhs->type.isMap() ? "  maplen\n" : e.lhs->type == Type::String ? "  str.len\n" : "  alen\n");  // the only property is .length
                return;
            case Expr::Kind::MethodCall: {
                if (e.methodIndex >= 0) {  // a method of a class (or a property accessor)
                    if (!e.isStatic) expr(*e.lhs);
                    for (const auto& a : e.args) expr(*a);
                    setLine(e.loc.line);
                    os_ << "  call " << fnName(prog_.methods[static_cast<size_t>(e.methodIndex)]) << " "
                        << e.args.size() + (e.isStatic ? 0 : 1) << "\n";
                    return;
                }
                expr(*e.lhs);
                for (const auto& a : e.args) expr(*a);
                setLine(e.loc.line);
                const Type& rt = e.lhs->type;
                if (rt == Type::Dyn) os_ << "  dyn.invoke " << quote(e.text) << " " << e.args.size() << "\n";
                else if (rt.isMap())
                    os_ << "  " << (e.text == "containsKey" ? "maphas" : e.text == "remove" ? "mapdel" : e.text == "tryGet" ? "maptry"
                                   : e.text == "keys" ? "mapkeys" : "mapvalues") << "\n";
                else os_ << (e.text == "push" ? "  apush\n" : "  acopy\n");
                return;
            }
        }
    }

    // Emits a Convert node (see Expr::Conv).
    void convert(const Expr& e) {
        using C = Expr::Conv;
        if (e.conv == C::IntToLong && e.lhs->kind == Expr::Kind::Int) {  // a literal: write it as a long directly
            os_ << "  push.lng " << e.lhs->ival << "\n";
            return;
        }
        expr(*e.lhs);
        setLine(e.loc.line);
        switch (e.conv) {
            case C::IntToLong: os_ << "  i2l\n"; return;
            case C::LongToInt: os_ << "  l2i\n"; return;
            case C::LongToDouble: os_ << "  l2d\n"; return;
            case C::DoubleToLong: os_ << "  d2l\n"; return;
            case C::IntToByte: os_ << "  i2b\n"; return;
            case C::ByteToInt: os_ << "  b2i\n"; return;
            case C::StringToLong: os_ << "  parse.lng\n"; return;
            case C::IntToDouble: os_ << "  i2d\n"; return;
            case C::DoubleToInt: os_ << "  d2i\n"; return;
            case C::CharToInt: os_ << "  c2i\n"; return;
            case C::IntToChar: os_ << "  i2c\n"; return;
            case C::StringToInt: os_ << "  parse.int\n"; return;
            case C::StringToDouble: os_ << "  parse.dbl\n"; return;
            case C::StringToGuid: os_ << "  parse.guid\n"; return;
            case C::ToString: toString(e.lhs->type); return;
            case C::Wrap: os_ << "  wrap\n"; return;
            case C::Box: os_ << "  box\n"; return;
            case C::Unbox: os_ << "  unbox " << mlTypeName(e.type) << "\n"; return;
            case C::Upcast: os_ << "  upcast " << mlTypeName(e.type) << "\n"; return;
            case C::Downcast: os_ << "  downcast " << mlTypeName(e.type) << "\n"; return;
            case C::Retype: return;
        }
    }

    void binary(const Expr& e) {
        const std::string& op = e.text;

        // Short-circuit logic: the right operand is evaluated only when needed.
        if (op == "&&" || op == "||") {
            std::string shortL = newLabel(), endL = newLabel();
            expr(*e.lhs);
            if (op == "||") os_ << "  not\n";
            os_ << "  jump.false " << shortL << "\n";
            expr(*e.rhs);
            os_ << "  jump " << endL << "\n";
            os_ << "label " << shortL << "\n";
            os_ << "  push.bool " << (op == "||" ? "true" : "false") << "\n";
            os_ << "label " << endL << "\n";
            return;
        }

        if (op == "??") {
            const bool unwrap = e.lhs->type.isNullable() && !e.type.isNullable();
            std::string notNull = newLabel(), endL = newLabel();
            expr(*e.lhs);
            os_ << "  dup\n  isnull\n  jump.false " << notNull << "\n  pop\n";
            expr(*e.rhs);
            os_ << "  jump " << endL << "\nlabel " << notNull << "\n";
            if (unwrap) os_ << "  unwrap\n";
            os_ << "label " << endL << "\n";
            return;
        }

        if (e.lhs->type == Type::Dyn) {  // both operands are dyn (sema coerced them): resolved at run time
            expr(*e.lhs);
            expr(*e.rhs);
            setLine(e.loc.line);
            const char* n = op == "+" ? "add" : op == "-" ? "sub" : op == "*" ? "mul" : op == "/" ? "div" : op == "%" ? "rem"
                          : op == "==" ? "eq" : op == "!=" ? "ne" : op == "<" ? "lt" : op == "<=" ? "le" : op == ">" ? "gt" : "ge";
            os_ << "  dyn." << n << "\n";
            return;
        }

        if (op == "+" && e.type == Type::String) {  // concatenation, with int/boolean converted
            expr(*e.lhs);
            toString(e.lhs->type);
            expr(*e.rhs);
            toString(e.rhs->type);
            setLine(e.loc.line);
            os_ << "  concat\n";
            return;
        }

        expr(*e.lhs);
        expr(*e.rhs);
        setLine(e.loc.line);
        Type operand = e.lhs->type;  // both operands have the same type for every remaining op
        if (operand == Type::Int && e.rhs->type == Type::Double) operand = Type::Double;
        const char* suffix = operand == Type::Int ? "int" : operand == Type::Long ? "lng" : operand == Type::Double ? "dbl" : operand == Type::Char ? "chr"
                           : operand == Type::String ? "str" : operand == Type::Bool ? "bool" : operand == Type::Guid ? "guid"
                           : operand == Type::Obj ? "obj" : "ref";
        const char* n = op == "&" ? "and" : op == "|" ? "or" : op == "^" ? "xor" : op == "<<" ? "shl" : op == ">>" ? "shr" :
                        op == "+" ? "add" : op == "-" ? "sub" : op == "*" ? "mul" : op == "/" ? "div" : op == "%" ? "rem"
                      : op == "==" ? "eq" : op == "!=" ? "ne" : op == "<" ? "lt" : op == "<=" ? "le" : op == ">" ? "gt" : "ge";
        os_ << "  " << n << "." << suffix << "\n";
    }
};

} // namespace

std::string generateMl(const Program& program, const std::string& sourcePath, bool removeUnused) {
    return Gen(program, removeUnused).run(sourcePath);
}

} // namespace coyote
