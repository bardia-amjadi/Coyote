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
        for (const ClassDecl& c : prog_.classes)
            if (!c.supers.empty()) version = 7;  // class hierarchies need ML 7
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
        function("$main", "() -> void", Type::Void, prog_.mainLocals, prog_.stmts, 0, nullptr);
        os_ << "entry $main\n";
        return os_.str();
    }

private:
    const Program& prog_;
    bool dce_;
    std::vector<char> live_;       // per method: reachable from the top-level code
    std::ostringstream os_;
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
            if (e.kind == Expr::Kind::Call || e.kind == Expr::Kind::MethodCall || e.kind == Expr::Kind::NewObj || e.kind == Expr::Kind::Member)
                mark(e.methodIndex);
            if (e.lhs) ex(*e.lhs);
            if (e.rhs) ex(*e.rhs);
            for (const auto& a : e.args) ex(*a);
        };
        std::function<void(const Stmt&)> st = [&](const Stmt& s) {
            if (s.expr) ex(*s.expr);
            if (s.target) ex(*s.target);
            if (s.init) st(*s.init);
            if (s.step) st(*s.step);
            for (const auto& b : s.body) st(*b);
            for (const auto& b : s.elseBody) st(*b);
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
                if (s.target->kind == Expr::Kind::Var) {
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
                loops_.push_back({top, end});
                for (const auto& c : s.body) stmt(*c);
                loops_.pop_back();
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
                loops_.push_back({cont, end});
                for (const auto& c : s.body) stmt(*c);
                loops_.pop_back();
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
                os_ << "  alen\n  lt.int\n  jump.false " << end << "\n";
                os_ << "  load " << s.slotArr << "\n  load " << s.slotIdx << "\n  aload\n  store " << s.slot << "\n";
                loops_.push_back({cont, end});
                for (const auto& c : s.body) stmt(*c);
                loops_.pop_back();
                os_ << "label " << cont << "\n";
                os_ << "  load " << s.slotIdx << "\n  push.int 1\n  add.int\n  store " << s.slotIdx << "\n";
                os_ << "  jump " << top << "\n";
                os_ << "label " << end << "\n";
                return;
            }
            case Stmt::Kind::Break:
                os_ << "  jump " << loops_.back().second << "\n";
                return;
            case Stmt::Kind::Continue:
                os_ << "  jump " << loops_.back().first << "\n";
                return;
            case Stmt::Kind::Block:
                for (const auto& c : s.body) stmt(*c);
                return;
            case Stmt::Kind::Return:
                if (s.expr) expr(*s.expr);
                os_ << "  ret\n";
                return;
        }
    }

    // Converts the value on top of the stack to a string (for concatenation).
    void toString(Type t) {
        if (t == Type::Int) os_ << "  str.int\n";
        else if (t == Type::Bool) os_ << "  str.bool\n";
        else if (t == Type::Double) os_ << "  str.dbl\n";
        else if (t == Type::Char) os_ << "  str.chr\n";
        else if (t == Type::Guid) os_ << "  str.guid\n";
        else if (t.isRefLike()) os_ << "  tostr.obj\n";
    }

    void expr(const Expr& e) {
        switch (e.kind) {
            case Expr::Kind::String: os_ << "  push.str " << quote(e.text) << "\n"; return;
            case Expr::Kind::Int: os_ << "  push.int " << e.ival << "\n"; return;
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
            case Expr::Kind::Var: os_ << "  load " << e.slot << "\n"; return;
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
                if (e.lhs->type == Type::Dyn) os_ << (e.text == "-" ? "  dyn.neg\n" : "  dyn.not\n");
                else if (e.text == "-") os_ << (e.lhs->type == Type::Double ? "  neg.dbl\n" : "  neg.int\n");
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
                os_ << (e.lhs->type.isMap() ? "  mapget\n" : e.lhs->type == Type::Dyn ? "  dyn.index\n" : "  aload\n");
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
                else os_ << (e.lhs->type.isMap() ? "  maplen\n" : "  alen\n");  // the only property is .length
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
        expr(*e.lhs);
        setLine(e.loc.line);
        switch (e.conv) {
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
        const char* suffix = operand == Type::Int ? "int" : operand == Type::Double ? "dbl" : operand == Type::Char ? "chr"
                           : operand == Type::String ? "str" : operand == Type::Bool ? "bool" : operand == Type::Guid ? "guid"
                           : operand == Type::Obj ? "obj" : "ref";
        const char* n = op == "+" ? "add" : op == "-" ? "sub" : op == "*" ? "mul" : op == "/" ? "div" : op == "%" ? "rem"
                      : op == "==" ? "eq" : op == "!=" ? "ne" : op == "<" ? "lt" : op == "<=" ? "le" : op == ">" ? "gt" : "ge";
        os_ << "  " << n << "." << suffix << "\n";
    }
};

} // namespace

std::string generateMl(const Program& program, const std::string& sourcePath, bool removeUnused) {
    return Gen(program, removeUnused).run(sourcePath);
}

} // namespace coyote
