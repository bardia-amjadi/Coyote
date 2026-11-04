#include "parser.hpp"

#include <algorithm>
#include <set>

namespace coyote {

namespace {

// Reserved by the language design but not implemented yet. Using one is a clear error rather
// than being silently misparsed.
const std::set<std::string>& unsupportedWords() {
    static const std::set<std::string> w = {
        "hidden", "created", "here", "operator", "module"};
    return w;
}

const std::set<std::string>& keywords() {
    static const std::set<std::string> w = {"hand", "method", "var", "int", "string", "boolean",
                                            "if", "else", "while", "for", "break", "continue", "return", "true", "false", "array", "new",
                                            "double", "char", "Guid", "obj", "dyn", "map", "null", "in", "is", "import", "export", "use", "namespace",
                                            "class", "model", "interface", "property", "constructor", "public", "private", "protected", "override", "static", "this"};
    return w;
}

// Limits that keep hostile input from overflowing the native stack in the parser (nesting) or in
// the later passes that recurse over the tree (expression depth).
constexpr int kMaxNesting = 256;
constexpr int kMaxExprDepth = 1000;

class Parser {
public:
    Parser(const std::vector<Token>& toks, std::string file) : toks_(toks), file_(std::move(file)) {}

    // `errors` null: stop at (throw) the first syntax error. Otherwise errors are collected and parsing resumes at the next
    // declaration or statement, so one run reports several syntax errors.
    Program run(std::vector<Diagnostic>* errors = nullptr) {
        errs_ = errors;
        Program p;
        while (peek().kind != Tok::Eof) {
            const size_t itemStart = pos_;
            try {
                topLevelItem(p);
            } catch (const CompileError& e) {
                if (!errs_) throw;
                report(e.diag);
                recoverItem(itemStart);
                if (errs_->size() >= kMaxReportedErrors) break;
            }
        }
        return p;
    }

private:
    std::vector<Diagnostic>* errs_ = nullptr;
    static constexpr size_t kMaxReportedErrors = 50;

    // Records an error unless the previous one was on the same line (a follow-up of the same mistake).
    void report(const Diagnostic& d) {
        if (!errs_->empty() && errs_->back().loc.line == d.loc.line) return;
        errs_->push_back(d);
    }

    // After an error inside a top-level item: skip to the end of that item (its closing '}' or its line).
    void recoverItem(size_t start) {
        int depth = 0;
        for (size_t i = start; i < pos_ && i < toks_.size(); ++i) {
            if (tokIsPunct(toks_[i], "{")) ++depth;
            else if (tokIsPunct(toks_[i], "}") && depth > 0) --depth;
        }
        while (peek().kind != Tok::Eof) {
            if (isPunct("{")) {
                ++depth;
            } else if (isPunct("}")) {
                if (depth <= 1) { take(); break; }
                --depth;
            } else if (depth == 0 && pos_ > start && peek().nlBefore) {
                break;
            }
            take();
        }
        if (pos_ == start) take();
    }

    // After an error inside a block: skip the rest of the broken statement, leaving the block's own '}' alone.
    void recoverStatement(size_t start) {
        int depth = 0;
        for (size_t i = start; i < pos_ && i < toks_.size(); ++i) {
            if (tokIsPunct(toks_[i], "{")) ++depth;
            else if (tokIsPunct(toks_[i], "}") && depth > 0) --depth;
        }
        while (peek().kind != Tok::Eof) {
            if (isPunct("{")) {
                ++depth;
            } else if (isPunct("}")) {
                if (depth == 0) break;  // closes the enclosing block
                if (--depth == 0) { take(); if (isWord("else")) continue; break; }
            } else if (depth == 0 && pos_ > start && peek().nlBefore) {
                break;
            }
            take();
        }
        if (pos_ == start) take();
    }

    void topLevelItem(Program& p) {
        {
            if (isPunct("}")) fail(peek(), "unexpected '}'");
            if (isWord("namespace")) {
                const Token& kw = take();
                if (&kw != &toks_[0])
                    fail(kw, "'namespace' must be the first declaration of the file");
                p.nsLoc = kw.loc;
                for (;;) {
                    if (peek().kind != Tok::Ident || keywords().count(peek().text))
                        fail(peek(), "expected a namespace name (letters, digits and '_', parts separated by '.'), found " + describe(peek()));
                    p.ns += take().text;
                    if (!isPunct(".")) break;
                    take();
                    p.ns += ".";
                }
                endStatement();
            } else if (isWord("import")) {
                fail(peek(), "'import' was removed in Coyote 0.6: put 'namespace Name' at the top of the file and write use \"Name\" to see its exports");
            } else if (isWord("use")) {
                ImportDecl imp;
                imp.isUse = true;
                imp.loc = take().loc;
                if (peek().kind != Tok::String) fail(peek(), "expected a quoted namespace, library or package name (or a .lib path) after 'use', found " + describe(peek()));
                imp.spec = take().text;
                if (imp.spec.empty()) fail(toks_[pos_ - 1], "the use name is empty");
                endStatement();
                p.imports.push_back(std::move(imp));
            } else if (isWord("export")) {
                take();
                if (isWord("class") || isWord("model") || isWord("interface")) {
                    p.classes.push_back(classDecl());
                    p.classes.back().exported = true;
                } else if (isWord("method")) {
                    p.methods.push_back(methodDecl());
                    p.methods.back().exported = true;
                } else {
                    fail(peek(), "'export' must be followed by 'method', 'class', 'interface' or 'model', found " + describe(peek()));
                }
            } else if (isWord("class") || isWord("model") || isWord("interface")) {
                p.classes.push_back(classDecl());
            } else if (isWord("hand")) {
                p.hands.push_back(handDecl());
                endStatement();
            } else if (isWord("method")) {
                p.methods.push_back(methodDecl());
            } else {
                p.stmts.push_back(statement());
            }
        }
    }

    const std::vector<Token>& toks_;
    std::string file_;
    size_t pos_ = 0;
    int depth_ = 0;      // current syntactic nesting (blocks, parentheses, types, prefix operators)

    // RAII nesting guard. Throws when the limit is exceeded (parsing then stops, so the counter
    // does not need to be restored on that path).
    struct Depth {
        Parser& p;
        Depth(Parser& parser, const Token& at) : p(parser) {
            if (++p.depth_ > kMaxNesting)
                p.fail(at, "code is nested too deeply (limit " + std::to_string(kMaxNesting) + " levels)");
        }
        ~Depth() { --p.depth_; }
    };

    const Token& peek() const { return toks_[pos_]; }
    const Token& peekNext() const { return toks_[std::min(pos_ + 1, toks_.size() - 1)]; }
    const Token& take() {
        const Token& t = toks_[pos_];
        if (t.kind != Tok::Eof) ++pos_;
        return t;
    }
    static bool tokIsPunct(const Token& t, const char* c) { return t.kind == Tok::Punct && t.text == c; }
    bool isPunct(const char* c) const { return tokIsPunct(peek(), c); }
    bool isWord(const char* w) const { return peek().kind == Tok::Ident && peek().text == w; }

    [[noreturn]] void fail(const Token& at, const std::string& msg) const {
        throw CompileError{Diagnostic{file_, at.loc, msg}};
    }

    static std::string describe(const Token& t) {
        switch (t.kind) {
            case Tok::Ident: return "identifier '" + t.text + "'";
            case Tok::Int: return "integer '" + t.text + "'";
            case Tok::Double: return "double '" + t.text + "'";
            case Tok::Char: return "a character literal";
            case Tok::String: return "a string literal";
            case Tok::Punct: return "'" + t.text + "'";
            case Tok::Eof: return "end of file";
        }
        return "?";
    }

    void expectPunct(const char* c, const std::string& context) {
        if (!isPunct(c)) fail(peek(), std::string("expected '") + c + "' " + context + ", found " + describe(peek()));
        take();
    }

    // True at an identifier that starts a declaration with a user-class type: "Point p", "Point? p".
    bool startsClassDeclaration() const {
        const Token& t = peek();
        if (t.kind != Tok::Ident || keywords().count(t.text) || unsupportedWords().count(t.text)) return false;
        const Token& n = peekNext();
        auto isName = [&](const Token& x) { return x.kind == Tok::Ident && !keywords().count(x.text); };
        if (tokIsPunct(n, "<")) {  // Box<int> b / Pair<string, int>? p: scan the balanced <...> and look at what follows
            size_t k = pos_ + 1;
            int depth = 0;
            for (; k < toks_.size() && k < pos_ + 80; ++k) {
                const Token& x = toks_[k];
                if (x.kind == Tok::Punct && x.text == "<") ++depth;
                else if (x.kind == Tok::Punct && x.text == ">") { if (--depth == 0) break; }
                else if (!(x.kind == Tok::Ident || (x.kind == Tok::Punct && (x.text == "," || x.text == "?")))) return false;
            }
            if (k + 1 >= toks_.size() || depth != 0) return false;
            if (isName(toks_[k + 1])) return true;
            return tokIsPunct(toks_[k + 1], "?") && k + 2 < toks_.size() && isName(toks_[k + 2]);
        }
        if (isName(n)) return true;
        return tokIsPunct(n, "?") && pos_ + 2 < toks_.size() && isName(toks_[pos_ + 2]);
    }

    static bool isTypeWord(const std::string& w) {
        return w == "var" || w == "int" || w == "string" || w == "boolean" || w == "array" || w == "double" ||
               w == "char" || w == "Guid" || w == "obj" || w == "dyn" || w == "map";
    }

    // A user-chosen name: an identifier that is neither a keyword nor a reserved word.
    std::string expectName(const std::string& what) {
        const Token& t = peek();
        if (t.kind != Tok::Ident) fail(t, "expected " + what + ", found " + describe(t));
        if (keywords().count(t.text)) fail(t, "'" + t.text + "' is a keyword and cannot be used as " + what);
        if (unsupportedWords().count(t.text)) fail(t, "'" + t.text + "' is reserved and cannot be used as " + what);
        return take().text;
    }

    void endStatement() {
        if (isPunct(";")) { take(); return; }  // optional terminator
        if (peek().kind == Tok::Eof || peek().nlBefore || isPunct("}")) return;
        fail(peek(), "expected end of statement (newline or ';'), found " + describe(peek()));
    }

    // ---- declarations ----

    Type typeName() {
        Depth guard(*this, peek());
        const Token& t = peek();
        if (t.kind != Tok::Ident) fail(t, "expected a type, found " + describe(t));
        Type base = baseType();
        if (isPunct("?")) {
            const Token& q = take();
            if (base.kind == Type::Obj || base.kind == Type::Dyn)
                fail(q, "'" + coyote::typeName(base) + "' can already hold null; remove the '?'");
            if (isPunct("?") || isPunct("??")) fail(peek(), "a type can only be made nullable once");
            return Type::nullableOf(base);
        }
        if (isPunct("??")) fail(peek(), "a type can only be made nullable once");
        return base;
    }

    Type baseType() {
        const Token& t = peek();
        if (t.text == "string") { take(); return Type::String; }
        if (t.text == "int") { take(); return Type::Int; }
        if (t.text == "boolean") { take(); return Type::Bool; }
        if (t.text == "double") { take(); return Type::Double; }
        if (t.text == "char") { take(); return Type::Char; }
        if (t.text == "Guid") { take(); return Type::Guid; }
        if (t.text == "obj") { take(); return Type::Obj; }
        if (t.text == "dyn") { take(); return Type::Dyn; }
        if (t.text == "array") {
            take();
            expectPunct("<", "after 'array' (e.g. array<int>)");
            Type element = typeName();
            expectPunct(">", "to close the array type");
            return Type::arrayOf(element);
        }
        if (t.text == "map") {
            take();
            expectPunct("<", "after 'map' (e.g. map<string, int>)");
            const Token& kt = peek();
            Type key = typeName();
            if (key != Type::Int && key != Type::String && key != Type::Char && key != Type::Bool && key != Type::Guid)
                fail(kt, "map keys must be int, string, char, boolean or Guid, not " + coyote::typeName(key));
            expectPunct(",", "between the key and value types of a map");
            Type value = typeName();
            expectPunct(">", "to close the map type");
            return Type::mapOf(key, value);
        }
        if (unsupportedWords().count(t.text)) fail(t, "type '" + t.text + "' is not supported yet");
        if (keywords().count(t.text)) fail(t, "expected a type, found keyword '" + t.text + "'");
        Type ct = Type::classOf(take().text);  // a class name; semantic analysis checks that it exists
        if (isPunct("<")) ct.targs = typeArgList();
        return ct;
    }

    // '<' type {',' type} '>'   (generic arguments)
    std::vector<Type> typeArgList() {
        std::vector<Type> args;
        const Token open = take();  // '<'
        Depth guard(*this, open);
        for (;;) {
            args.push_back(typeName());
            if (isPunct(",")) { take(); continue; }
            break;
        }
        expectPunct(">", "to close the generic arguments");
        return args;
    }

    // '<' name {',' name} '>'   (declaring type parameters)
    std::vector<std::string> typeParamList() {
        std::vector<std::string> names;
        take();  // '<'
        for (;;) {
            const SourceLoc at = peek().loc;
            std::string n = expectName("a type parameter name");
            for (const std::string& o : names)
                if (o == n) throw CompileError{Diagnostic{file_, at, "duplicate type parameter '" + n + "'"}};
            names.push_back(std::move(n));
            if (isPunct(",")) { take(); continue; }
            break;
        }
        expectPunct(">", "to close the type parameters");
        return names;
    }

    // allowThis: a leading `this T name` makes an extension method (reported through *isExtension).
    std::vector<Param> paramList(bool allowThis = false, bool* isExtension = nullptr, bool allowVariadic = true) {
        std::vector<Param> params;
        expectPunct("(", "to start the parameter list");
        if (!isPunct(")")) {
            for (;;) {
                Param prm;
                prm.loc = peek().loc;
                if (isWord("this")) {
                    if (!allowThis || !params.empty())
                        fail(peek(), "'this' can only mark the first parameter of a top-level method (an extension method)");
                    take();
                    if (isExtension) *isExtension = true;
                }
                prm.type = typeName();
                if (isPunct(".")) {
                    const Token& dot = peek();
                    if (!allowVariadic) fail(dot, "variadic parameters ('...') are not supported here");
                    for (int k = 0; k < 3; ++k) {
                        if (!isPunct(".")) fail(peek(), "expected '...' after the parameter type, found " + describe(peek()));
                        take();
                    }
                    prm.variadic = true;
                    prm.type = Type::arrayOf(prm.type);
                }
                prm.name = expectName("a parameter name");
                bool variadic = prm.variadic;
                params.push_back(std::move(prm));
                if (variadic && isPunct(",")) fail(peek(), "a variadic parameter must be the last parameter");
                if (isPunct(",")) { take(); continue; }
                break;
            }
        }
        expectPunct(")", "to close the parameter list");
        if (isExtension && *isExtension && params.size() == 1 && params[0].variadic)
            fail(toks_[pos_ - 1], "the 'this' parameter of an extension method cannot be variadic");
        return params;
    }

    HandDecl handDecl() {
        HandDecl h;
        h.loc = take().loc;  // 'hand'
        if (!isPunct(":")) fail(peek(), "expected ':' after 'hand' (e.g. hand:con println(string text)), found " + describe(peek()));
        take();
        h.module = expectName("a host module name after 'hand:'");
        h.name = expectName("a host function name");
        h.params = paramList(false, nullptr, false);
        if (isPunct(":")) {
            take();
            h.ret = typeName();
        }
        return h;
    }

    MethodDecl methodDecl(bool topLevel = true, bool signatureOnly = false) {
        MethodDecl m;
        take();  // 'method'
        m.loc = peek().loc;
        m.name = expectName("a method name");
        if (isPunct("<")) {
            if (!topLevel) fail(peek(), "generic methods are only supported at the top level; make the class generic instead");
            m.typeParams = typeParamList();
        }
        m.params = paramList(topLevel, &m.isExtension);
        if (isPunct(":")) {
            take();
            m.ret = typeName();
        }
        if (signatureOnly) {  // interface method: no body
            m.isAbstract = true;
            endStatement();
            return m;
        }
        m.body = block("to start the body of method '" + m.name + "'");
        return m;
    }

    // ---- classes ----

    // class Name { members }  |  model Name { fields }
    // members: [public|private] [static] <type> <name> [= expr]        field
    //          [public|private] constructor(params) { ... }
    //          [public|private] [static] method name(params) [: type] { ... }
    //          [public|private] property <type> Name [{ get { ... } set { ... } }]
    ClassDecl classDecl() {
        ClassDecl c;
        c.isModel = isWord("model");
        c.isInterface = isWord("interface");
        take();  // 'class' / 'model' / 'interface'
        c.loc = peek().loc;
        c.name = expectName("a class name");
        if (isPunct("<")) c.typeParams = typeParamList();
        if (isPunct(":")) {  // class D : Base, Interface1, Interface2
            if (c.isModel) fail(peek(), "a model cannot have a base class or interfaces");
            take();
            for (;;) {
                c.superLocs.push_back(peek().loc);
                if (peek().kind != Tok::Ident || keywords().count(peek().text)) fail(peek(), "expected a base class or interface name, found " + describe(peek()));
                Type st = typeName();
                c.superNames.push_back(st.name);
                c.superTypes.push_back(std::move(st));
                if (!isPunct(",")) break;
                take();
            }
        }
        if (!isPunct("{")) fail(peek(), "expected '{' to start the body of '" + c.name + "', found " + describe(peek()));
        const Token open = take();
        Depth guard(*this, open);
        while (!isPunct("}")) {
            if (peek().kind == Tok::Eof)
                fail(peek(), "expected '}' to close the class opened at line " + std::to_string(open.loc.line));
            classMember(c);
        }
        take();  // '}'
        return c;
    }

    ExprPtr varExpr(const std::string& name, SourceLoc loc) {
        auto v = std::make_unique<Expr>();
        v->kind = Expr::Kind::Var;
        v->loc = loc;
        v->text = name;
        return v;
    }

    ExprPtr thisField(const std::string& field, SourceLoc loc) {
        auto m = std::make_unique<Expr>();
        m->kind = Expr::Kind::Member;
        m->loc = loc;
        m->text = field;
        m->lhs = varExpr("this", loc);
        m->depth = 2;
        return m;
    }

    void classMember(ClassDecl& c) {
        bool isPrivate = false, isProtected = false, isStatic = false, isOverride = false, sawAccess = false;
        for (;;) {
            if (isWord("public") || isWord("private") || isWord("protected")) {
                if (sawAccess) fail(peek(), "a member can have only one of 'public', 'private' and 'protected'");
                sawAccess = true;
                isPrivate = peek().text == "private";
                isProtected = peek().text == "protected";
                take();
            } else if (isWord("override")) {
                if (isOverride) fail(peek(), "duplicate 'override'");
                isOverride = true;
                take();
            } else if (isWord("static")) {
                if (isStatic) fail(peek(), "duplicate 'static'");
                isStatic = true;
                take();
            } else {
                break;
            }
        }
        const Token& t = peek();
        if (isWord("constructor")) {
            if (isStatic) fail(t, "a constructor cannot be static");
            for (const MethodDecl& m : c.methods)
                if (m.isCtor) fail(t, "class '" + c.name + "' already has a constructor (overloading is not supported)");
            MethodDecl m;
            m.isCtor = true;
            m.isPrivate = isPrivate;
            m.isProtected = isProtected;
            m.loc = take().loc;
            m.name = "$new";
            m.params = paramList();
            if (isPunct(":")) {  // constructor(...) : base(args)
                take();
                if (!isWord("base")) fail(peek(), "expected 'base(...)' after ':' in a constructor, found " + describe(peek()));
                m.hasBaseCall = true;
                m.baseLoc = take().loc;
                if (!isPunct("(")) fail(peek(), "expected '(' after 'base', found " + describe(peek()));
                take();
                while (!isPunct(")")) {
                    m.baseArgs.push_back(expression());
                    if (isPunct(",")) take(); else break;
                }
                if (!isPunct(")")) fail(peek(), "expected ')' to close the base(...) arguments, found " + describe(peek()));
                take();
            }
            m.body = block("to start the constructor body");
            c.methods.push_back(std::move(m));
            return;
        }
        if (isWord("method")) {
            MethodDecl m = methodDecl(false, c.isInterface);
            m.isStatic = isStatic;
            m.isPrivate = isPrivate;
            m.isProtected = isProtected;
            m.isOverride = isOverride;
            if (c.isInterface && (isStatic || isPrivate || isProtected || isOverride))
                fail(t, "interface methods cannot be static, private, protected or override");
            c.methods.push_back(std::move(m));
            return;
        }
        if (c.isInterface) fail(t, "an interface can only contain method signatures (method name(params): type)");
        if (isOverride) fail(t, "'override' applies to methods only");
        if (isWord("property")) {
            if (isStatic) fail(t, "static properties are not supported; use a static field");
            propertyDecl(c, isPrivate, isProtected);
            return;
        }
        if (t.kind != Tok::Ident) fail(t, "expected a member of class '" + c.name + "', found " + describe(t));
        if (unsupportedWords().count(t.text)) fail(t, "'" + t.text + "' is not supported yet");
        if (isWord("var")) fail(t, "fields need an explicit type (e.g. int count); 'var' is not allowed in a class body");
        FieldDecl f;
        f.loc = t.loc;
        f.type = typeName();
        f.loc = peek().loc;
        f.name = expectName("a field name");
        f.isPrivate = isPrivate;
        f.isProtected = isProtected;
        f.isStatic = isStatic;
        if (isPunct("=")) {
            take();
            f.init = expression();
        }
        endStatement();
        for (const FieldDecl& other : c.fields)
            if (other.name == f.name) fail(toks_[pos_ - 1], "duplicate field '" + f.name + "' in class '" + c.name + "'");
        c.fields.push_back(std::move(f));
    }

    void propertyDecl(ClassDecl& c, bool isPrivate, bool isProtected) {
        take();  // 'property'
        PropertyDecl p;
        p.type = typeName();
        p.loc = peek().loc;
        p.name = expectName("a property name");
        p.isPrivate = isPrivate;
        p.isProtected = isProtected;
        for (const PropertyDecl& other : c.properties)
            if (other.name == p.name) fail(toks_[pos_ - 1], "duplicate property '" + p.name + "' in class '" + c.name + "'");
        for (const FieldDecl& other : c.fields)
            if (other.name == p.name) fail(toks_[pos_ - 1], "'" + p.name + "' is already a field of class '" + c.name + "'");

        auto accessor = [&](bool getter, std::vector<StmtPtr> body) {
            MethodDecl m;
            m.loc = p.loc;
            m.isPrivate = isPrivate;
            m.isProtected = isProtected;
            m.name = (getter ? "get_" : "set_") + p.name;
            if (getter) {
                m.ret = p.type;
            } else {
                Param v;
                v.type = p.type;
                v.name = "value";
                v.loc = p.loc;
                m.params.push_back(std::move(v));
            }
            m.body = std::move(body);
            c.methods.push_back(std::move(m));
        };

        if (!isPunct("{")) {  // auto-property: a hidden backing field plus trivial accessors
            endStatement();
            FieldDecl backing;
            backing.loc = p.loc;
            backing.name = "$" + p.name;
            backing.type = p.type;
            backing.isPrivate = true;
            c.fields.push_back(std::move(backing));
            std::vector<StmtPtr> g;
            auto r = make(Stmt::Kind::Return, p.loc);
            r->expr = thisField("$" + p.name, p.loc);
            g.push_back(std::move(r));
            std::vector<StmtPtr> st;
            auto a = make(Stmt::Kind::Assign, p.loc);
            a->target = thisField("$" + p.name, p.loc);
            a->expr = varExpr("value", p.loc);
            st.push_back(std::move(a));
            accessor(true, std::move(g));
            accessor(false, std::move(st));
            p.hasGet = p.hasSet = true;
            c.properties.push_back(std::move(p));
            return;
        }
        const Token open = take();  // '{'
        while (!isPunct("}")) {
            if (peek().kind == Tok::Eof) fail(peek(), "expected '}' to close the property opened at line " + std::to_string(open.loc.line));
            if (isWord("get") || isWord("set")) {
                bool getter = peek().text == "get";
                if ((getter && p.hasGet) || (!getter && p.hasSet)) fail(peek(), std::string("duplicate '") + peek().text + "' accessor");
                take();
                accessor(getter, block(getter ? "to start the 'get' body" : "to start the 'set' body"));
                (getter ? p.hasGet : p.hasSet) = true;
            } else {
                fail(peek(), "expected 'get' or 'set' in the property body, found " + describe(peek()));
            }
        }
        take();  // '}'
        if (!p.hasGet && !p.hasSet) fail(toks_[pos_ - 1], "property '" + p.name + "' needs a 'get' or a 'set' accessor");
        c.properties.push_back(std::move(p));
    }

    // ---- statements ----

    std::vector<StmtPtr> block(const std::string& context) {
        Depth guard(*this, peek());
        if (!isPunct("{")) fail(peek(), "expected '{' " + context + ", found " + describe(peek()));
        const Token open = take();
        std::vector<StmtPtr> body;
        while (!isPunct("}")) {
            if (peek().kind == Tok::Eof)
                fail(peek(), "expected '}' to close the block opened at line " + std::to_string(open.loc.line));
            const size_t stmtStart = pos_;
            try {
                body.push_back(statement());
            } catch (const CompileError& e) {
                if (!errs_) throw;
                report(e.diag);
                if (errs_->size() >= kMaxReportedErrors) throw;
                recoverStatement(stmtStart);
            }
        }
        take();  // '}'
        return body;
    }

    StmtPtr make(Stmt::Kind k, SourceLoc loc) {
        auto s = std::make_unique<Stmt>();
        s->kind = k;
        s->loc = loc;
        return s;
    }

    StmtPtr statement() {
        const Token& t = peek();
        if (isPunct("{")) {
            auto s = make(Stmt::Kind::Block, t.loc);
            s->body = block("to start a block");
            return s;
        }
        if (t.kind != Tok::Ident) fail(t, "expected a statement, found " + describe(t));
        const std::string& w = t.text;

        if (w == "import" || w == "export" || w == "use") fail(t, "'" + w + "' is only allowed at the top level of a file");
        if (w == "hand") fail(t, "hand declarations are only allowed at the top level of a file");
        if (w == "method") fail(t, "methods can only be declared at the top level of a file");
        if (w == "else") fail(t, "'else' without a matching 'if'");
        if (w == "if") return ifStatement();
        if (w == "while") return whileStatement();
        if (w == "for") return forStatement();
        if (w == "break" || w == "continue") {
            auto s = make(w == "break" ? Stmt::Kind::Break : Stmt::Kind::Continue, t.loc);
            take();
            endStatement();
            return s;
        }
        if (w == "return") return returnStatement();
        if ((isTypeWord(w) && !tokIsPunct(peekNext(), "(")) || startsClassDeclaration()) return varDecl();
        if (unsupportedWords().count(w)) fail(t, "'" + w + "' is not supported yet");
        if (w == "true" || w == "false") fail(t, "expected a statement, found '" + w + "'");

        return exprStatement(true);
    }

    // Copies an expression that can be evaluated twice without changing the program: variables, literals, fields,
    // elements and arithmetic of those. Returns null for anything else (calls, `new`, ...).
    ExprPtr cloneSimple(const Expr& e) {
        auto c = std::make_unique<Expr>();
        c->kind = e.kind;
        c->loc = e.loc;
        c->text = e.text;
        c->ival = e.ival;
        c->dval = e.dval;
        c->depth = e.depth;
        switch (e.kind) {
            case Expr::Kind::Var: case Expr::Kind::Int: case Expr::Kind::Char: case Expr::Kind::Bool: case Expr::Kind::String:
                return c;
            case Expr::Kind::Member:
                if (!e.lhs || !(c->lhs = cloneSimple(*e.lhs))) return nullptr;
                return c;
            case Expr::Kind::Index: case Expr::Kind::Binary:
                if (!e.lhs || !e.rhs || !(c->lhs = cloneSimple(*e.lhs)) || !(c->rhs = cloneSimple(*e.rhs))) return nullptr;
                return c;
            default:
                return nullptr;
        }
    }

    // A statement that starts with an expression: a call, a method call, or an assignment (plain or
    // compound). `terminate` is false inside a 'for' header, which has its own separators.
    StmtPtr exprStatement(bool terminate) {
        const Token& t = peek();
        ExprPtr lhs = postfix(primary());
        if (peek().kind == Tok::Punct && (peek().text == "+=" || peek().text == "-=" || peek().text == "*=" ||
                                          peek().text == "/=" || peek().text == "%=" || peek().text == "++" || peek().text == "--")) {
            const bool step = peek().text == "++" || peek().text == "--";
            const Token op = take();
            ExprPtr copy = cloneSimple(*lhs);
            if (!copy)
                fail(op, "'" + op.text + "' needs a target without side effects (a variable, a field or an element with a simple index); write the full assignment");
            auto bin = std::make_unique<Expr>();
            bin->kind = Expr::Kind::Binary;
            bin->loc = op.loc;
            bin->text = op.text.substr(0, 1);
            bin->lhs = std::move(copy);
            if (step) {
                auto one = std::make_unique<Expr>();
                one->kind = Expr::Kind::Int;
                one->loc = op.loc;
                one->ival = 1;
                bin->rhs = std::move(one);
            } else {
                bin->rhs = expression();
            }
            measure(*bin);
            auto s = make(Stmt::Kind::Assign, t.loc);
            s->target = std::move(lhs);
            s->expr = std::move(bin);
            if (terminate) endStatement();
            return s;
        }
        if (isPunct("=")) {
            if (lhs->kind != Expr::Kind::Var && lhs->kind != Expr::Kind::Index && lhs->kind != Expr::Kind::Member)
                fail(peek(), "invalid assignment target: only variables, elements and fields can be assigned");
            auto s = make(Stmt::Kind::Assign, t.loc);
            take();  // '='
            s->target = std::move(lhs);
            s->expr = expression();
            if (terminate) endStatement();
            return s;
        }
        if (lhs->kind == Expr::Kind::Call || lhs->kind == Expr::Kind::MethodCall) {
            auto s = make(Stmt::Kind::ExprStmt, t.loc);
            s->expr = std::move(lhs);
            if (terminate) endStatement();
            return s;
        }
        fail(t, "expression is not a statement: only calls and assignments can be used as statements");
    }

    StmtPtr varDecl(bool terminate = true) {
        auto s = make(Stmt::Kind::VarDecl, peek().loc);
        if (isWord("var")) {
            take();
            s->inferred = true;
        } else {
            s->declType = typeName();
        }
        s->name = expectName("a variable name");
        if (!isPunct("=")) {
            fail(peek(), "variable '" + s->name + "' must be initialized (expected '=', found " + describe(peek()) + ")");
        }
        take();
        s->expr = expression();
        if (terminate) endStatement();
        return s;
    }

    StmtPtr condition(StmtPtr s, const char* kw) {
        take();  // keyword
        expectPunct("(", std::string("after '") + kw + "'");
        s->expr = expression();
        expectPunct(")", "after the condition");
        return s;
    }

    StmtPtr ifStatement() {
        Depth guard(*this, peek());
        auto s = condition(make(Stmt::Kind::If, peek().loc), "if");
        s->body = block("to start the 'if' body");
        if (isWord("else")) {
            take();
            s->hasElse = true;
            if (isWord("if")) {
                s->elseBody.push_back(ifStatement());
            } else {
                s->elseBody = block("to start the 'else' body");
            }
        }
        return s;
    }

    StmtPtr whileStatement() {
        auto s = condition(make(Stmt::Kind::While, peek().loc), "while");
        s->body = block("to start the 'while' body");
        return s;
    }

    // for (init; condition; step) { ... } -- each part is optional; a missing condition means true.
    StmtPtr forStatement() {
        Depth guard(*this, peek());
        auto s = make(Stmt::Kind::For, take().loc);
        expectPunct("(", "after 'for'");
        if (peek().kind == Tok::Ident && (isTypeWord(peek().text) || startsClassDeclaration()) && forEachAhead()) {
            s->kind = Stmt::Kind::ForEach;
            if (isWord("var")) {
                take();
                s->inferred = true;
            } else {
                s->declType = typeName();
            }
            s->name = expectName("a loop variable name");
            take();  // 'in'
            s->expr = expression();
            expectPunct(")", "after the 'for' collection");
            s->body = block("to start the 'for' body");
            return s;
        }
        if (!isPunct(";")) {
            if ((peek().kind == Tok::Ident && isTypeWord(peek().text) && !tokIsPunct(peekNext(), "(")) || startsClassDeclaration())
                s->init = varDecl(false);
            else
                s->init = exprStatement(false);
        }
        expectPunct(";", "after the 'for' initializer");
        if (!isPunct(";")) s->expr = expression();
        expectPunct(";", "after the 'for' condition");
        if (!isPunct(")")) s->step = exprStatement(false);
        expectPunct(")", "after the 'for' header");
        s->body = block("to start the 'for' body");
        return s;
    }

    // True when the tokens after 'for (' read "<type> <name> in" (a for-each loop). Looks ahead
    // without consuming anything.
    bool forEachAhead() {
        size_t save = pos_;
        int saveDepth = depth_;
        bool result = false;
        try {
            if (isWord("var")) {
                take();
            } else {
                typeName();
            }
            if (peek().kind == Tok::Ident && !keywords().count(peek().text)) {
                take();
                result = isWord("in");
            }
        } catch (const CompileError&) {
            result = false;
        }
        pos_ = save;
        depth_ = saveDepth;
        return result;
    }

    StmtPtr returnStatement() {
        auto s = make(Stmt::Kind::Return, take().loc);
        if (!(peek().kind == Tok::Eof || peek().nlBefore || isPunct(";") || isPunct("}"))) s->expr = expression();
        endStatement();
        return s;
    }

    // ---- expressions ----

    ExprPtr callExpr() {
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::Call;
        e->loc = peek().loc;
        e->text = take().text;  // callee name (already checked by the caller)
        e->args = argList();
        measure(*e);
        return e;
    }

    // '(' [expression {',' expression}] ')'
    std::vector<ExprPtr> argList() {
        std::vector<ExprPtr> args;
        expectPunct("(", "to start the argument list");
        if (!isPunct(")")) {
            for (;;) {
                args.push_back(expression());
                if (isPunct(",")) { take(); continue; }
                break;
            }
        }
        expectPunct(")", "to close the argument list");
        return args;
    }

    // Index, property and method-call suffixes. They must start on the same line as the
    // expression they apply to, so a '[' or '.' at the start of a line never continues the
    // previous statement.
    ExprPtr postfix(ExprPtr e) {
        for (;;) {
            const Token& t = peek();
            if (t.nlBefore) return e;
            if (tokIsPunct(t, "!")) {
                auto n = std::make_unique<Expr>();
                n->kind = Expr::Kind::Unwrap;
                n->loc = t.loc;
                take();
                n->lhs = std::move(e);
                measure(*n);
                e = std::move(n);
            } else if (tokIsPunct(t, "[")) {
                auto n = std::make_unique<Expr>();
                n->kind = Expr::Kind::Index;
                n->loc = t.loc;
                take();
                n->lhs = std::move(e);
                n->rhs = expression();
                expectPunct("]", "to close the index");
                measure(*n);
                e = std::move(n);
            } else if (tokIsPunct(t, ".")) {
                take();
                const Token& name = peek();
                if (name.kind != Tok::Ident) fail(name, "expected a member name after '.', found " + describe(name));
                auto n = std::make_unique<Expr>();
                n->loc = name.loc;
                n->text = take().text;
                n->lhs = std::move(e);
                if (isPunct("(")) {
                    n->kind = Expr::Kind::MethodCall;
                    n->args = argList();
                } else {
                    n->kind = Expr::Kind::Member;
                }
                measure(*n);
                e = std::move(n);
            } else {
                return e;
            }
        }
    }

    // Computes the height of a freshly built node from its children and rejects the expression as
    // soon as it exceeds the limit. This must happen during construction: left-associative chains
    // (a + b + c + ...) and index/member chains are built by loops, not recursion, so only an
    // explicit check keeps the tree shallow enough for the recursive passes that follow (semantic
    // analysis, code generation, and destroying the tree itself) to be safe on any input.
    void measure(Expr& e) {
        int d = 0;
        if (e.lhs) d = std::max(d, e.lhs->depth);
        if (e.rhs) d = std::max(d, e.rhs->depth);
        for (const auto& a : e.args) d = std::max(d, a->depth);
        e.depth = d + 1;
        if (e.depth > kMaxExprDepth)
            throw CompileError{Diagnostic{file_, e.loc, "expression is too complex (more than " +
                                                            std::to_string(kMaxExprDepth) + " levels deep)"}};
    }

    ExprPtr expression() {
        Depth guard(*this, peek());
        ExprPtr lhs = binary(0);
        if (isPunct("??")) {  // right-associative, lower precedence than '||'
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Kind::Binary;
            e->loc = peek().loc;
            e->text = "??";
            take();
            e->lhs = std::move(lhs);
            e->rhs = expression();
            measure(*e);
            return e;
        }
        return lhs;
    }

    ExprPtr binary(size_t level) {
        static const std::vector<std::vector<std::string>> levels = {
            {"||"}, {"&&"}, {"==", "!="}, {"<", "<=", ">", ">="}, {"+", "-"}, {"*", "/", "%"}};
        if (level == levels.size()) return unary();
        ExprPtr lhs = binary(level + 1);
        for (;;) {
            const Token& t = peek();
            if (level == 3 && isWord("is")) {
                auto e = std::make_unique<Expr>();
                e->kind = Expr::Kind::Is;
                e->loc = t.loc;
                take();
                e->lhs = std::move(lhs);
                e->newType = typeName();
                measure(*e);
                lhs = std::move(e);
                continue;
            }
            if (level == 3 && isWord("as")) {  // downcast: animal as Dog
                auto e = std::make_unique<Expr>();
                e->kind = Expr::Kind::Convert;
                e->conv = Expr::Conv::Downcast;
                e->loc = t.loc;
                take();
                e->lhs = std::move(lhs);
                e->newType = typeName();
                measure(*e);
                lhs = std::move(e);
                continue;
            }
            if (t.kind != Tok::Punct ||
                std::find(levels[level].begin(), levels[level].end(), t.text) == levels[level].end())
                return lhs;
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Kind::Binary;
            e->loc = t.loc;
            e->text = t.text;
            take();
            e->lhs = std::move(lhs);
            e->rhs = binary(level + 1);
            measure(*e);
            lhs = std::move(e);
        }
    }

    ExprPtr unary() {
        Depth guard(*this, peek());
        const Token& t = peek();
        if (t.kind == Tok::Punct && (t.text == "-" || t.text == "!")) {
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Kind::Unary;
            e->loc = t.loc;
            e->text = t.text;
            take();
            e->lhs = unary();
            measure(*e);
            return e;
        }
        return postfix(primary());
    }

    // $"Hello {name}, {a + b}!" becomes "Hello " + (name) + ", " + (a + b) + "!". `{{` and `}}` are literal braces.
    // The expressions cannot contain double quotes (the string would end there).
    ExprPtr interpolated(const Token& t) {
        const std::string& s = t.text;
        ExprPtr acc;
        auto strExpr = [&](const std::string& v) {
            auto x = std::make_unique<Expr>();
            x->kind = Expr::Kind::String;
            x->loc = t.loc;
            x->text = v;
            return x;
        };
        auto add = [&](ExprPtr part) {
            if (!acc) {
                if (part->kind == Expr::Kind::String) { acc = std::move(part); return; }
                acc = strExpr("");
            }
            auto bin = std::make_unique<Expr>();
            bin->kind = Expr::Kind::Binary;
            bin->loc = t.loc;
            bin->text = "+";
            bin->lhs = std::move(acc);
            bin->rhs = std::move(part);
            measure(*bin);
            acc = std::move(bin);
        };
        std::string lit;
        auto flush = [&]() { if (!lit.empty()) { add(strExpr(lit)); lit.clear(); } };
        for (size_t i = 0; i < s.size();) {
            if (s[i] == '{') {
                if (i + 1 < s.size() && s[i + 1] == '{') { lit += '{'; i += 2; continue; }
                size_t j = i + 1;
                int d = 1;
                while (j < s.size()) {
                    if (s[j] == '{') ++d;
                    else if (s[j] == '}' && --d == 0) break;
                    ++j;
                }
                if (d != 0) fail(t, "unterminated '{' in an interpolated string (write '{{' for a literal brace)");
                flush();
                add(interpolatedExpr(s.substr(i + 1, j - i - 1), t));
                i = j + 1;
            } else if (s[i] == '}') {
                if (i + 1 < s.size() && s[i + 1] == '}') { lit += '}'; i += 2; continue; }
                fail(t, "a single '}' in an interpolated string: write '}}' for a literal brace");
            } else {
                lit += s[i++];
            }
        }
        flush();
        return acc ? std::move(acc) : strExpr("");
    }

    ExprPtr interpolatedExpr(const std::string& text, const Token& at) {
        if (text.find_first_not_of(" \t") == std::string::npos) fail(at, "empty expression '{}' in an interpolated string");
        std::vector<Token> toks;
        try {
            toks = lex(text, file_);
        } catch (const CompileError& e) {
            fail(at, "in the interpolated expression {" + text + "}: " + e.diag.message);
        }
        for (Token& k : toks) k.loc = at.loc;  // diagnostics point at the string
        Parser sub(toks, file_);
        sub.depth_ = depth_;
        ExprPtr x = sub.expression();
        if (sub.peek().kind != Tok::Eof) fail(at, "unexpected " + describe(sub.peek()) + " in the interpolated expression {" + text + "}");
        return x;
    }

    ExprPtr primary() {
        const Token& t = peek();
        auto e = std::make_unique<Expr>();
        e->loc = t.loc;
        switch (t.kind) {
            case Tok::String:
                if (t.ival == 1) {  // $"text {expression} text"
                    const Token& st = take();
                    return interpolated(st);
                }
                e->kind = Expr::Kind::String;
                e->text = t.text;
                take();
                return e;
            case Tok::Int:
                e->kind = Expr::Kind::Int;
                e->ival = t.ival;
                take();
                return e;
            case Tok::Double:
                e->kind = Expr::Kind::Double;
                e->dval = t.dval;
                take();
                return e;
            case Tok::Char:
                e->kind = Expr::Kind::Char;
                e->ival = t.ival;
                take();
                return e;
            case Tok::Ident:
                if (t.text == "null") {
                    e->kind = Expr::Kind::Null;
                    take();
                    return e;
                }
                if ((t.text == "int" || t.text == "double" || t.text == "string" || t.text == "char" || t.text == "Guid") &&
                    tokIsPunct(peekNext(), "(")) {  // conversion: int(x), double(x), string(x), char(x), Guid(x)
                    e->kind = Expr::Kind::Convert;
                    e->newType = baseType();
                    take();  // '('
                    e->lhs = expression();
                    expectPunct(")", "to close the conversion");
                    measure(*e);
                    return e;
                }
                if (t.text == "this") {
                    e->kind = Expr::Kind::Var;
                    e->text = take().text;
                    return e;
                }
                if (t.text == "new") {
                    take();
                    if (peek().kind == Tok::Ident && !keywords().count(peek().text) && !unsupportedWords().count(peek().text)) {
                        e->kind = Expr::Kind::NewObj;  // new Point(1, 2)
                        e->loc = peek().loc;
                        e->text = take().text;
                        if (isPunct("<")) e->newType = [&] { Type ct = Type::classOf(e->text); ct.targs = typeArgList(); return ct; }();
                        e->args = argList();
                        measure(*e);
                        return e;
                    }
                    if (isWord("map")) {
                        e->kind = Expr::Kind::NewMap;
                        e->newType = typeName();
                        expectPunct("(", "after the map type (new map<K, V>())");
                        expectPunct(")", "to close new map<K, V>(); maps are created empty");
                        measure(*e);
                        return e;
                    }
                    e->kind = Expr::Kind::NewArray;
                    if (!isWord("array")) fail(peek(), "'new' can only create arrays or maps (new array<T>(size), new map<K, V>()), found " + describe(peek()));
                    e->newType = typeName();
                    expectPunct("(", "after the array type (new array<T>(size))");
                    e->lhs = expression();
                    expectPunct(")", "to close the array size");
                    measure(*e);
                    return e;
                }
                if (t.text == "true" || t.text == "false") {
                    e->kind = Expr::Kind::Bool;
                    e->ival = t.text == "true" ? 1 : 0;
                    take();
                    return e;
                }
                if (keywords().count(t.text)) fail(t, "'" + t.text + "' cannot be used in an expression");
                if (unsupportedWords().count(t.text)) fail(t, "'" + t.text + "' is not supported yet");
                if (tokIsPunct(peekNext(), "(")) return callExpr();
                if (tokIsPunct(peekNext(), "<")) {  // Id<int>(x): explicit generic arguments (otherwise `a < b`)
                    const size_t savePos = pos_;
                    const int saveDepth = depth_;
                    std::vector<Type> targs;
                    bool ok = false;
                    const Token& name = peek();
                    try {
                        take();
                        targs = typeArgList();
                        ok = isPunct("(");
                    } catch (const CompileError&) {
                    }
                    if (ok) {
                        e->kind = Expr::Kind::Call;
                        e->loc = name.loc;
                        e->text = name.text;
                        e->typeArgs = std::move(targs);
                        e->args = argList();
                        measure(*e);
                        return e;
                    }
                    pos_ = savePos;
                    depth_ = saveDepth;
                }
                e->kind = Expr::Kind::Var;
                e->text = take().text;
                return e;
            case Tok::Punct:
                if (t.text == "[") {
                    take();
                    e->kind = Expr::Kind::ArrayLit;
                    if (isPunct("]"))
                        fail(t, "cannot infer the element type of an empty array literal; use new array<T>(0)");
                    for (;;) {
                        e->args.push_back(expression());
                        if (isPunct(",")) { take(); continue; }
                        break;
                    }
                    expectPunct("]", "to close the array literal");
                    measure(*e);
                    return e;
                }
                if (t.text == "(") {
                    take();
                    ExprPtr inner = expression();
                    expectPunct(")", "to close the parenthesized expression");
                    return inner;
                }
                break;
            case Tok::Eof:
                break;
        }
        fail(t, "expected an expression, found " + describe(t));
    }
};

} // namespace

Program parse(const std::vector<Token>& tokens, const std::string& file) {
    return Parser(tokens, file).run();
}

Program parseRecovering(const std::vector<Token>& tokens, const std::string& file, std::vector<Diagnostic>& errors) {
    return Parser(tokens, file).run(&errors);
}

} // namespace coyote
