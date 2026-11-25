#include "parser.hpp"

#include <algorithm>
#include <list>
#include <set>

namespace coyote {

namespace {

// Reserved by the language design but not implemented yet. Using one is a clear error rather
// than being silently misparsed.
const std::set<std::string>& unsupportedWords() {
    static const std::set<std::string> w = {
        "created", "operator", "module"};
    return w;
}

const std::set<std::string>& keywords() {
    static const std::set<std::string> w = {"hand", "mouth", "method", "var", "int", "long", "byte", "string", "boolean",
                                            "if", "else", "while", "for", "break", "continue", "return", "true", "false", "array", "new",
                                            "double", "long", "byte", "char", "Guid", "obj", "dyn", "map", "null", "in", "is", "import", "export", "use", "namespace",
                                            "enum", "switch", "case", "default", "class", "model", "interface", "property", "field", "constructor", "public", "private", "protected", "override", "static", "this", "try", "catch", "finally", "throw",
                                            "hidden", "shared", "here", "locked", "readonly"};
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
                failIfRenamed(peek());
            } else if (isWord("public") || isWord("shared") || isWord("hidden") || isWord("class") || isWord("model") || isWord("interface") || isWord("method") || isWord("enum")) {
                bool isPublic = false, isShared = false, sawVis = false;
                const Token first = peek();
                while (isWord("public") || isWord("shared") || isWord("hidden")) {
                    if (isWord("shared")) {
                        if (isShared) fail(peek(), "duplicate 'shared'");
                        isShared = true;
                    } else {
                        if (sawVis) fail(peek(), "a declaration can have only one of 'public' and 'hidden'");
                        sawVis = true;
                        isPublic = peek().text == "public";
                    }
                    take();
                }
                failIfRenamed(peek());
                if (isWord("enum")) {
                    if (isShared) fail(first, "'shared' cannot be applied to an enum");
                    p.classes.push_back(enumDecl());
                    p.classes.back().exported = isPublic;
                } else if (isWord("class") || isWord("model") || isWord("interface")) {
                    if (isShared && !isWord("class")) fail(peek(), "only a class can be 'shared'");
                    p.classes.push_back(classDecl(isShared));
                    p.classes.back().exported = isPublic;
                } else if (isWord("method")) {
                    if (isShared) fail(first, "'shared' applies to classes and members; a top-level method is already shared");
                    p.methods.push_back(methodDecl());
                    p.methods.back().exported = isPublic;
                } else {
                    fail(peek(), "'" + first.text + "' must be followed by 'method', 'class', 'model' or 'interface', found " + describe(peek()));
                }
            } else if (isWord("hand")) {
                p.hands.push_back(handDecl());
                endStatement();
            } else if (isWord("mouth")) {
                p.mouths.push_back(mouthDecl());
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
    const Token& peekAt(size_t k) const { return toks_[std::min(pos_ + k, toks_.size() - 1)]; }
    const Token& peekNext() const { return toks_[std::min(pos_ + 1, toks_.size() - 1)]; }
    const Token& take() {
        const Token& t = toks_[pos_];
        if (t.kind != Tok::Eof) ++pos_;
        return t;
    }
    static bool tokIsPunct(const Token& t, const char* c) { return t.kind == Tok::Punct && t.text == c; }
    bool isPunct(const char* c) const { return tokIsPunct(peek(), c); }
    bool isWord(const char* w) const { return peek().kind == Tok::Ident && peek().text == w; }

    // Words renamed in 0.6.3: the old spelling is still reserved, but only produces this message.
    static const char* renamedTo(const std::string& w) {
        if (w == "private") return "hidden";
        if (w == "static") return "shared";
        if (w == "this") return "here";
        if (w == "export") return "public";
        return nullptr;
    }
    void failIfRenamed(const Token& t) const {
        if (t.kind != Tok::Ident) return;
        if (const char* to = renamedTo(t.text))
            fail(t, "'" + t.text + "' was renamed to '" + to + "' in Coyote 0.6.3");
    }

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
        size_t p = pos_;  // skip a qualified name: Ns.Sub.Point
        while (p + 2 < toks_.size() && tokIsPunct(toks_[p + 1], ".") && toks_[p + 2].kind == Tok::Ident && !keywords().count(toks_[p + 2].text)) p += 2;
        const Token& n = p + 1 < toks_.size() ? toks_[p + 1] : peekNext();
        auto isName = [&](const Token& x) { return x.kind == Tok::Ident && !keywords().count(x.text); };
        if (tokIsPunct(n, "<")) {  // Box<int> b / Pair<string, int>? p: scan the balanced <...> and look at what follows
            size_t k = p + 1;
            int depth = 0;
            for (; k < toks_.size() && k < p + 80; ++k) {
                const Token& x = toks_[k];
                if (x.kind == Tok::Punct && x.text == "<") ++depth;
                else if (x.kind == Tok::Punct && x.text == ">") { if (--depth == 0) break; }
                else if (!(x.kind == Tok::Ident || (x.kind == Tok::Punct && (x.text == "," || x.text == "?" || x.text == ".")))) return false;
            }
            if (k + 1 >= toks_.size() || depth != 0) return false;
            if (isName(toks_[k + 1])) return true;
            return tokIsPunct(toks_[k + 1], "?") && k + 2 < toks_.size() && isName(toks_[k + 2]);
        }
        if (isName(n)) return true;
        return tokIsPunct(n, "?") && p + 2 < toks_.size() && isName(toks_[p + 2]);
    }

    // A class name, possibly qualified with namespaces: Ns.Sub.Point
    std::string qualifiedName() {
        std::string n = take().text;
        while (isPunct(".") && peekNext().kind == Tok::Ident && !keywords().count(peekNext().text)) {
            take();
            n += "." + take().text;
        }
        return n;
    }

    static bool isTypeWord(const std::string& w) {
        return w == "var" || w == "int" || w == "long" || w == "byte" || w == "string" || w == "boolean" || w == "array" || w == "double" ||
               w == "char" || w == "Guid" || w == "obj" || w == "dyn" || w == "map";
    }

    // True at `fn(...) : T name =`, a declaration of a function-typed variable (a call of a method named fn is not).
    bool fnDeclarationAhead() {
        if (!(peek().kind == Tok::Ident && peek().text == "fn" && tokIsPunct(peekNext(), "("))) return false;
        const size_t save = pos_;
        const int saveDepth = depth_;
        bool r = false;
        try {
            typeName();
            r = peek().kind == Tok::Ident && !keywords().count(peek().text);
        } catch (const CompileError&) {
            r = false;
        }
        pos_ = save;
        depth_ = saveDepth;
        return r;
    }

    // At '(': is this `(params) => ...` or `(params) : type => ...`?
    bool lambdaAhead() {
        const size_t save = pos_;
        const int saveDepth = depth_;
        bool r = false;
        try {
            take();
            int d = 1;
            while (d > 0 && peek().kind != Tok::Eof) {
                if (isPunct("(")) ++d;
                else if (isPunct(")")) --d;
                take();
            }
            if (isPunct("=>")) {
                r = true;
            } else if (isPunct(":")) {
                take();
                typeName();
                r = isPunct("=>");
            }
        } catch (const CompileError&) {
            r = false;
        }
        pos_ = save;
        depth_ = saveDepth;
        return r;
    }

    ExprPtr lambdaExpr() {
        Depth guard(*this, peek());
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::Lambda;
        e->loc = peek().loc;
        take();  // '('
        if (!isPunct(")")) {
            for (;;) {
                Param prm;
                prm.loc = peek().loc;
                if (peek().kind == Tok::Ident && !keywords().count(peek().text) && (tokIsPunct(peekNext(), ",") || tokIsPunct(peekNext(), ")"))) {
                    prm.type = Type::Invalid;  // the type comes from where the lambda is used
                    prm.name = take().text;
                } else {
                    prm.type = typeName();
                    prm.loc = peek().loc;
                    prm.name = expectName("a parameter name");
                }
                e->lparams.push_back(std::move(prm));
                if (isPunct(",")) { take(); continue; }
                break;
            }
        }
        expectPunct(")", "to close the lambda parameters");
        if (isPunct(":")) {
            take();
            e->hasLret = true;
            e->lret = typeName();
        }
        expectPunct("=>", "after the lambda parameters");
        if (isPunct("{")) {
            e->lbody = block("to start the lambda body");
        } else {
            auto r = make(Stmt::Kind::Return, peek().loc);
            r->expr = expression();
            e->lbody.push_back(std::move(r));
            e->exprBody = true;
        }
        measure(*e);
        return e;
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
        if (peek().kind == Tok::Eof || peek().nlBefore || isPunct("}") || isWord("else")) return;
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
        if (t.text == "long") { take(); return Type::Long; }
        if (t.text == "byte") { take(); return Type::Byte; }
        if (t.text == "boolean") { take(); return Type::Bool; }
        if (t.text == "double") { take(); return Type::Double; }
        if (t.text == "char") { take(); return Type::Char; }
        if (t.text == "Guid") { take(); return Type::Guid; }
        if (t.text == "obj") { take(); return Type::Obj; }
        if (t.text == "dyn") { take(); return Type::Dyn; }
        if (t.text == "fn" && tokIsPunct(peekNext(), "(")) {  // fn(int, string) : boolean   (no ': type' means void)
            take();
            take();
            std::vector<Type> params;
            if (!isPunct(")")) {
                for (;;) {
                    params.push_back(typeName());
                    if (isPunct(",")) { take(); continue; }
                    break;
                }
            }
            expectPunct(")", "to close the function type's parameter list");
            Type ret = Type::Void;
            if (isPunct(":")) { take(); ret = typeName(); }
            return Type::fnOf(params, ret);
        }
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
            if (key != Type::Int && key != Type::Long && key != Type::Byte && key != Type::String && key != Type::Char && key != Type::Bool && key != Type::Guid)
                fail(kt, "map keys must be int, string, char, boolean or Guid, not " + coyote::typeName(key));
            expectPunct(",", "between the key and value types of a map");
            Type value = typeName();
            expectPunct(">", "to close the map type");
            return Type::mapOf(key, value);
        }
        if (unsupportedWords().count(t.text)) fail(t, "type '" + t.text + "' is not supported yet");
        if (keywords().count(t.text)) fail(t, "expected a type, found keyword '" + t.text + "'");
        Type ct = Type::classOf(qualifiedName());  // a class name (maybe Ns.Sub.Name); semantic analysis checks that it exists
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
                failIfRenamed(peek());
                if (isWord("here")) {
                    if (!allowThis || !params.empty())
                        fail(peek(), "'here' can only mark the first parameter of a top-level method (an extension method)");
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
            fail(toks_[pos_ - 1], "the 'here' parameter of an extension method cannot be variadic");
        return params;
    }

    // mouth:<module> <name>            read-only host variable
    // mouth:<module> <name> {get;}     the same, spelled out;  {get; set;} also allows assignment (the host decides whether it can)
    MouthDecl mouthDecl() {
        MouthDecl d;
        d.loc = take().loc;  // 'mouth'
        if (!isPunct(":")) fail(peek(), "expected ':' after 'mouth' (e.g. mouth:sys exception {get;}), found " + describe(peek()));
        take();
        d.module = expectName("a host module name after 'mouth:'");
        d.name = expectName("a mouth name");
        if (isPunct("{")) {
            take();
            d.canGet = d.canSet = false;
            while (!isPunct("}")) {
                const Token t = take();
                if (t.text == "get" && t.kind == Tok::Ident) d.canGet = true;
                else if (t.text == "set" && t.kind == Tok::Ident) d.canSet = true;
                else fail(t, "a mouth has only 'get;' and 'set;' accessors, found " + describe(t));
                if (isPunct(";")) take();
            }
            take();  // '}'
            if (!d.canGet && !d.canSet) fail(toks_[pos_ - 1], "a mouth needs at least one accessor: {get;} or {get; set;}");
        }
        return d;
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
    ClassDecl classDecl(bool shared = false) {
        ClassDecl c;
        c.isShared = shared;
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
        bool isReadonly = false, isLocked = false;
        for (;;) {
            failIfRenamed(peek());
            if (isWord("public") || isWord("hidden") || isWord("protected")) {
                if (sawAccess) fail(peek(), "a member can have only one of 'public', 'hidden' and 'protected'");
                sawAccess = true;
                isPrivate = peek().text == "hidden";
                isProtected = peek().text == "protected";
                take();
            } else if (isWord("override")) {
                if (isOverride) fail(peek(), "duplicate 'override'");
                isOverride = true;
                take();
            } else if (isWord("shared")) {
                if (isStatic) fail(peek(), "duplicate 'shared'");
                isStatic = true;
                take();
            } else if (isWord("readonly")) {
                if (isReadonly) fail(peek(), "duplicate 'readonly'");
                isReadonly = true;
                take();
            } else if (isWord("locked")) {
                if (isLocked) fail(peek(), "duplicate 'locked'");
                isLocked = true;
                take();
            } else {
                break;
            }
        }
        if (c.isShared) isStatic = true;  // `shared class`: every member is shared
        const Token& t = peek();
        if ((isReadonly || isLocked) && !isWord("field"))
            fail(t, std::string("'") + (isLocked ? "locked" : "readonly") + "' applies to fields only (write: " + (isLocked ? "locked" : "readonly") + " field ...)");
        if (isReadonly && isLocked) fail(t, "'locked' already means the field can never change; use either 'locked' or 'readonly'");
        if (isWord("constructor")) {
            if (c.isShared) fail(t, "a shared class has no constructor: it cannot be instantiated");
            if (isStatic) fail(t, "a constructor cannot be shared");
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
                fail(t, "interface methods cannot be shared, hidden, protected or override");
            c.methods.push_back(std::move(m));
            return;
        }
        if (c.isInterface) fail(t, "an interface can only contain method signatures (method name(params): type)");
        if (isOverride) fail(t, "'override' applies to methods only");
        if (isWord("property")) {
            if (isStatic) fail(t, "shared properties are not supported; use a shared field");
            propertyDecl(c, isPrivate, isProtected);
            return;
        }
        if (t.kind != Tok::Ident) fail(t, "expected a member of class '" + c.name + "', found " + describe(t));
        if (!isWord("field")) {
            if (unsupportedWords().count(t.text)) fail(t, "'" + t.text + "' is not supported yet");
            if (isWord("var")) fail(t, "fields are declared with 'field' and a type: field int count");
            fail(t, "a class member starts with 'field', 'property', 'method' or 'constructor' (since 0.6.3 fields are written: field " + (isTypeWord(t.text) || !keywords().count(t.text) ? t.text : std::string("int")) + " name)");
        }
        take();  // 'field'
        FieldDecl f;
        f.loc = t.loc;
        // `field Name = "x"`: the type comes from the initializer (literals and `new` only)
        const bool typeless = peek().kind == Tok::Ident && !keywords().count(peek().text) && tokIsPunct(peekNext(), "=");
        if (!typeless) f.type = typeName();
        f.loc = peek().loc;
        f.name = expectName("a field name");
        f.isPrivate = isPrivate;
        f.isProtected = isProtected;
        f.isStatic = isStatic;
        f.isReadonly = isReadonly;
        f.isLocked = isLocked;
        if (isPunct("=")) {
            take();
            f.init = expression();
        }
        if (typeless) {
            Type inferred = Type::Invalid;
            if (f.init) inferred = literalType(*f.init);
            if (inferred == Type::Invalid) fail(toks_[pos_ - 1], "cannot infer the type of field '" + f.name + "'; write it: field <type> " + f.name + " = ...");
            f.type = inferred;
        }
        if (isLocked && !f.init) fail(toks_[pos_ - 1], "a locked field needs its value: locked field <type> " + f.name + " = <constant>");
        endStatement();
        for (const FieldDecl& other : c.fields)
            if (other.name == f.name) fail(toks_[pos_ - 1], "duplicate field '" + f.name + "' in class '" + c.name + "'");
        c.fields.push_back(std::move(f));
    }

    // The type of a literal (or `new X(...)`) initializer, for `field Name = "x"`; Invalid when it is anything else.
    static Type literalType(const Expr& e) {
        switch (e.kind) {
            case Expr::Kind::Int: return Type::Int;
            case Expr::Kind::String: return Type::String;
            case Expr::Kind::Bool: return Type::Bool;
            case Expr::Kind::Double: return Type::Double;
            case Expr::Kind::Char: return Type::Char;
            case Expr::Kind::Unary: return (e.text == "-" && e.lhs) ? (literalType(*e.lhs) == Type::Int || literalType(*e.lhs) == Type::Double ? literalType(*e.lhs) : Type(Type::Invalid)) : Type(Type::Invalid);
            case Expr::Kind::NewObj: return e.newType.kind == Type::Class ? e.newType : Type::classOf(e.text);
            case Expr::Kind::Binary: {  // constant arithmetic / concatenation of literals
                if (!e.lhs || !e.rhs) return Type::Invalid;
                const Type l = literalType(*e.lhs), r = literalType(*e.rhs);
                if (l == Type::Invalid || r == Type::Invalid) return Type::Invalid;
                const std::string& o = e.text;
                if (o == "==" || o == "!=" || o == "<" || o == ">" || o == "<=" || o == ">=" || o == "&&" || o == "||") return Type::Bool;
                if (o == "+" && (l == Type::String || r == Type::String)) return Type::String;
                if (l == Type::Double || r == Type::Double) return Type::Double;
                return l;
            }
            default: return Type::Invalid;
        }
    }

    void propertyDecl(ClassDecl& c, bool isPrivate, bool isProtected) {
        take();  // 'property'
        PropertyDecl p;
        // `property Name = "x"` / `property Name { get; set; } = "x"`: the type comes from the initializer (literals and `new` only)
        const bool typeless = peek().kind == Tok::Ident && !keywords().count(peek().text) && (tokIsPunct(peekNext(), "=") || tokIsPunct(peekNext(), "{"));
        if (!typeless) p.type = typeName();
        p.loc = peek().loc;
        p.name = expectName("a property name");
        // `{ get; set; }` (or `{ get; }`) is the auto-property form; an optional `= value` initializes its backing field
        bool autoForm = !isPunct("{");
        bool wantSet = true;
        if (isPunct("{") && peekAt(1).kind == Tok::Ident && (peekAt(1).text == "get" || peekAt(1).text == "set") && tokIsPunct(peekAt(2), ";")) {
            take();  // '{'
            bool sawGet = false, sawSet = false;
            while (!isPunct("}")) {
                if (!(isWord("get") || isWord("set"))) fail(peek(), "expected 'get;' or 'set;' in the auto-property body, found " + describe(peek()));
                const bool g = peek().text == "get";
                if ((g && sawGet) || (!g && sawSet)) fail(peek(), std::string("duplicate '") + peek().text + "' accessor");
                (g ? sawGet : sawSet) = true;
                take();
                if (!isPunct(";")) fail(peek(), "expected ';' after the accessor, found " + describe(peek()));
                take();
            }
            take();  // '}'
            if (!sawGet) fail(toks_[pos_ - 1], "property '" + p.name + "' needs 'get;'");
            wantSet = sawSet;
            autoForm = true;
        }
        ExprPtr propInit;
        if (autoForm && isPunct("=")) {
            take();
            propInit = expression();
        }
        if (typeless) {
            Type inferred = propInit ? literalType(*propInit) : Type(Type::Invalid);
            if (inferred == Type::Invalid) fail(toks_[pos_ - 1], "cannot infer the type of property '" + p.name + "'; write it: property <type> " + p.name + " = ...");
            p.type = inferred;
        }
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

        if (autoForm) {  // auto-property: a hidden backing field plus trivial accessors
            endStatement();
            FieldDecl backing;
            backing.loc = p.loc;
            backing.name = "$" + p.name;
            backing.type = p.type;
            backing.isPrivate = true;
            backing.init = std::move(propInit);
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
            if (wantSet) accessor(false, std::move(st));
            p.hasGet = true;
            p.hasSet = wantSet;
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

    // A body is either a braced block or a single statement (0.6.1): "if (x) y = 1 else y = 2".
    std::vector<StmtPtr> bodyOrStatement(const std::string& context) {
        if (isPunct("{")) return block(context);
        std::vector<StmtPtr> one;
        one.push_back(statement());
        return one;
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

        failIfRenamed(t);
        if (w == "import" || w == "use") fail(t, "'" + w + "' is only allowed at the top level of a file");
        if (w == "hand") fail(t, "hand declarations are only allowed at the top level of a file");
        if (w == "mouth") fail(t, "mouth declarations are only allowed at the top level of a file");
        if (w == "method") fail(t, "methods can only be declared at the top level of a file");
        if (w == "else") fail(t, "'else' without a matching 'if'");
        if (w == "if") return ifStatement();
        if (w == "switch") return switchStatement();
        if (w == "try") return tryStatement();
        if (w == "throw") {
            auto s = make(Stmt::Kind::Throw, take().loc);
            s->expr = expression();
            endStatement();
            return s;
        }
        if (w == "catch") fail(t, "'catch' without a matching 'try'");
        if (w == "while") return whileStatement();
        if (w == "for") return forStatement();
        if (w == "in" && tokIsPunct(peekNext(), "(")) return foreachStatement();
        if (w == "break" || w == "continue") {
            auto s = make(w == "break" ? Stmt::Kind::Break : Stmt::Kind::Continue, t.loc);
            take();
            endStatement();
            return s;
        }
        if (w == "return") return returnStatement();
        if ((isTypeWord(w) && !tokIsPunct(peekNext(), "(")) || startsClassDeclaration() || fnDeclarationAhead()) return varDecl();
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
        const bool shiftRightAssign = peek().kind == Tok::Punct && peek().text == ">" && pos_ + 1 < toks_.size() && toks_[pos_ + 1].text == ">=" &&
                                      toks_[pos_ + 1].loc.line == peek().loc.line && toks_[pos_ + 1].loc.col == peek().loc.col + 1;
        if (shiftRightAssign || (peek().kind == Tok::Punct && (peek().text == "+=" || peek().text == "-=" || peek().text == "*=" ||
                                          peek().text == "/=" || peek().text == "%=" || peek().text == "&=" || peek().text == "|=" ||
                                          peek().text == "^=" || peek().text == "<<=" || peek().text == "++" || peek().text == "--"))) {
            const bool step = peek().text == "++" || peek().text == "--";
            Token op = take();
            if (shiftRightAssign) { take(); op.text = ">>="; }
            ExprPtr copy = cloneSimple(*lhs);
            if (!copy)
                fail(op, "'" + op.text + "' needs a target without side effects (a variable, a field or an element with a simple index); write the full assignment");
            auto bin = std::make_unique<Expr>();
            bin->kind = Expr::Kind::Binary;
            bin->loc = op.loc;
            bin->text = step ? op.text.substr(0, 1) : op.text.substr(0, op.text.size() - 1);
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

    // ---- enum (0.7) ----
    // enum Color { Red, Green = 5, Blue }  is desugared to a class: one shared, read-only instance per member (compared with ==),
    // with Name, Value, ToString(), CompareTo(other), Parse(text), TryParse(text), FromValue(n) and Values().
    std::list<std::vector<Token>> generated_;
    ClassDecl enumDecl() {
        const Token kw = take();  // 'enum'
        const SourceLoc at = kw.loc;
        const std::string name = expectName("an enum name");
        if (!isPunct("{")) fail(peek(), "expected '{' to start the enum '" + name + "', found " + describe(peek()));
        const Token open = take();
        std::vector<std::pair<std::string, long long>> members;
        std::set<std::string> seen;
        static const std::set<std::string> reserved = {"Name", "Value", "ToString", "CompareTo", "Parse", "TryParse", "FromValue", "Values", "all"};
        long long next = 0;
        while (!isPunct("}")) {
            if (peek().kind == Tok::Eof) fail(peek(), "expected '}' to close the enum opened at line " + std::to_string(open.loc.line));
            const Token mt = peek();
            const std::string m = expectName("an enum member name");
            if (reserved.count(m)) fail(mt, "'" + m + "' is used by every enum (Name, Value, ToString, CompareTo, Parse, TryParse, FromValue, Values); choose another member name");
            if (!seen.insert(m).second) fail(mt, "the enum '" + name + "' already has a member '" + m + "'");
            long long v = next;
            if (isPunct("=")) {
                take();
                bool neg = false;
                if (isPunct("-")) { take(); neg = true; }
                if (peek().kind != Tok::Int || peek().isLong) fail(peek(), "an enum value is an int literal, found " + describe(peek()));
                v = take().ival;
                if (neg) v = -v;
            }
            members.push_back({m, v});
            next = v + 1;
            if (isPunct(",")) take();
            else if (!isPunct("}") && !peek().nlBefore) fail(peek(), "expected ',' or '}' after the enum member '" + m + "', found " + describe(peek()));
        }
        take();  // '}'
        if (members.empty()) fail(kw, "the enum '" + name + "' has no members");
        std::string src = "class " + name + " {\n  public readonly field string Name\n  public readonly field int Value\n"
                          "  hidden constructor(string n, int v) {\n    Name = n\n    Value = v\n  }\n";
        std::string all;
        for (const auto& [m, v] : members) {
            src += "  public shared readonly field " + name + " " + m + " = new " + name + "(\"" + m + "\", " + std::to_string(v) + ")\n";
            all += (all.empty() ? "" : ", ") + name + "." + m;
        }
        src += "  hidden shared field array<" + name + "> all = [" + all + "]\n";
        src += "  public method ToString(): string {\n    return Name\n  }\n";
        src += "  public method CompareTo(" + name + " other): int {\n    if (Value < other.Value) {\n      return -1\n    }\n    if (Value > other.Value) {\n      return 1\n    }\n    return 0\n  }\n";
        src += "  public shared method TryParse(string text): " + name + "? {\n    in (var c of all) {\n      if (c.Name == text) {\n        return c\n      }\n    }\n    return null\n  }\n";
        src += "  public shared method Parse(string text): " + name + " {\n    in (var c of all) {\n      if (c.Name == text) {\n        return c\n      }\n    }\n    throw \"'\" + text + \"' is not a member of the enum " + name + "\"\n  }\n";
        src += "  public shared method FromValue(int n): " + name + " {\n    in (var c of all) {\n      if (c.Value == n) {\n        return c\n      }\n    }\n    throw \"" + name + " has no member with the value \" + n\n  }\n";
        src += "  public shared method Values(): array<" + name + "> {\n    return (copy)all\n  }\n}\n";
        std::vector<Token> toks;
        try {
            toks = lex(src, file_);
        } catch (const CompileError& e) {
            fail(kw, "internal error while expanding the enum '" + name + "': " + e.diag.message);
        }
        for (Token& k : toks) k.loc = at;  // diagnostics point at the enum
        generated_.push_back(std::move(toks));
        Parser sub(generated_.back(), file_);
        sub.depth_ = depth_;
        Program q = sub.run(nullptr);
        if (q.classes.size() != 1) fail(kw, "internal error while expanding the enum '" + name + "'");
        ClassDecl c = std::move(q.classes[0]);
        c.isEnum = true;
        for (const auto& mv : members) c.enumNames.push_back(mv.first);
        c.loc = at;
        return c;
    }

    // switch (subject) { case A, B: ... default: ... }  -- desugared to { var $sw = subject; if ($sw == A || $sw == B) {...} else {...} }.
    // There is no fall-through; 'break' still refers to the enclosing loop.
    int switchCounter_ = 0;
    StmtPtr switchStatement() {
        Depth guard(*this, peek());
        const Token kw = take();  // 'switch'
        expectPunct("(", "after 'switch'");
        ExprPtr subject = expression();
        expectPunct(")", "after the switch subject");
        if (!isPunct("{")) fail(peek(), "expected '{' to start the switch body, found " + describe(peek()));
        const Token open = take();
        const std::string tmp = "$sw" + std::to_string(switchCounter_++);
        auto block = make(Stmt::Kind::Block, kw.loc);
        auto decl = make(Stmt::Kind::VarDecl, kw.loc);
        decl->name = tmp;
        decl->inferred = true;
        decl->expr = std::move(subject);
        block->body.push_back(std::move(decl));
        struct Arm { std::vector<ExprPtr> pats; std::vector<StmtPtr> body; bool isDefault = false; SourceLoc loc; };
        std::vector<Arm> arms;
        bool sawDefault = false;
        while (!isPunct("}")) {
            if (peek().kind == Tok::Eof) fail(peek(), "expected '}' to close the switch opened at line " + std::to_string(open.loc.line));
            Arm arm;
            arm.loc = peek().loc;
            if (isWord("default")) {
                if (sawDefault) fail(peek(), "a switch can have only one 'default'");
                sawDefault = true;
                take();
                arm.isDefault = true;
            } else if (isWord("case")) {
                take();
                for (;;) {
                    arm.pats.push_back(expression());
                    if (!isPunct(",")) break;
                    take();
                }
            } else {
                fail(peek(), "a switch body holds 'case value:' and 'default:' sections, found " + describe(peek()));
            }
            expectPunct(":", "after the case value");
            while (!isPunct("}") && !isWord("case") && !isWord("default")) {
                if (peek().kind == Tok::Eof) fail(peek(), "expected '}' to close the switch opened at line " + std::to_string(open.loc.line));
                arm.body.push_back(statement());
            }
            arms.push_back(std::move(arm));
        }
        take();  // '}'
        // build the if-chain from the last arm backwards
        std::vector<StmtPtr> defaultBody;
        bool hasDefault = false;
        for (auto& a : arms) if (a.isDefault) { defaultBody = std::move(a.body); hasDefault = true; }
        StmtPtr chain;
        for (size_t k = arms.size(); k-- > 0;) {
            Arm& a = arms[k];
            if (a.isDefault) continue;
            auto ifs = make(Stmt::Kind::If, a.loc);
            ExprPtr cond;
            for (auto& pat : a.pats) {
                auto var = std::make_unique<Expr>();
                var->kind = Expr::Kind::Var;
                var->loc = pat->loc;
                var->text = tmp;
                var->depth = 1;
                auto cmp = std::make_unique<Expr>();
                cmp->kind = Expr::Kind::Binary;
                cmp->loc = pat->loc;
                cmp->text = "==";
                cmp->lhs = std::move(var);
                cmp->rhs = std::move(pat);
                measure(*cmp);
                if (!cond) {
                    cond = std::move(cmp);
                } else {
                    auto orx = std::make_unique<Expr>();
                    orx->kind = Expr::Kind::Binary;
                    orx->loc = cmp->loc;
                    orx->text = "||";
                    orx->lhs = std::move(cond);
                    orx->rhs = std::move(cmp);
                    measure(*orx);
                    cond = std::move(orx);
                }
            }
            ifs->expr = std::move(cond);
            ifs->body = std::move(a.body);
            if (chain) {
                ifs->hasElse = true;
                ifs->elseBody.push_back(std::move(chain));
            } else if (hasDefault) {
                ifs->hasElse = true;
                ifs->elseBody = std::move(defaultBody);
            }
            chain = std::move(ifs);
        }
        if (chain) block->body.push_back(std::move(chain));
        else for (auto& d : defaultBody) block->body.push_back(std::move(d));
        return block;
    }

    // switch (subject) { A, B => result, C => result, _ => result }
    ExprPtr switchExpression() {
        Depth guard(*this, peek());
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::Switch;
        e->loc = take().loc;  // 'switch'
        expectPunct("(", "after 'switch'");
        e->lhs = expression();
        expectPunct(")", "after the switch subject");
        if (!isPunct("{")) fail(peek(), "expected '{' to start the switch body, found " + describe(peek()));
        const Token open = take();
        while (!isPunct("}")) {
            if (peek().kind == Tok::Eof) fail(peek(), "expected '}' to close the switch opened at line " + std::to_string(open.loc.line));
            int count = 0;
            if (isWord("_")) {
                take();
            } else {
                for (;;) {
                    e->args.push_back(expression());
                    ++count;
                    if (!isPunct(",")) break;
                    take();
                }
            }
            expectPunct("=>", "after the switch pattern");
            e->args.push_back(expression());
            e->armSizes.push_back(count);
            if (isPunct(",")) take();
            else if (!isPunct("}") && !peek().nlBefore) fail(peek(), "expected ',' or '}' after the switch arm, found " + describe(peek()));
        }
        take();  // '}'
        if (e->armSizes.empty()) fail(open, "a switch expression needs at least one arm");
        measure(*e);
        return e;
    }

    StmtPtr ifStatement() {
        Depth guard(*this, peek());
        auto s = condition(make(Stmt::Kind::If, peek().loc), "if");
        s->body = bodyOrStatement("to start the 'if' body");
        if (isWord("else")) {
            take();
            s->hasElse = true;
            if (isWord("if")) {
                s->elseBody.push_back(ifStatement());
            } else {
                s->elseBody = bodyOrStatement("to start the 'else' body");
            }
        }
        return s;
    }

    // try { ... } catch (e: IoError) { ... } catch (e) { ... } finally { ... }
    // The typed catches are lowered here: the VM's single catch gets `var $ex = Exception.Caught()` followed by an if-chain of `is` tests;
    // `catch (e)` (or `catch (e: Exception)`) catches everything and must come last; without it an unmatched exception is rethrown.
    int catchCounter_ = 0;
    ExprPtr nameExpr(const std::string& n, SourceLoc loc) {
        auto v = std::make_unique<Expr>();
        v->kind = Expr::Kind::Var;
        v->loc = loc;
        v->text = n;
        v->depth = 1;
        return v;
    }
    StmtPtr tryStatement() {
        Depth guard(*this, peek());
        auto s = make(Stmt::Kind::Try, take().loc);
        s->body = block("to start the 'try' body");
        struct Clause { std::string var; bool typed = false; Type type = Type::Invalid; std::vector<StmtPtr> body; SourceLoc loc; };
        std::vector<Clause> clauses;
        while (isWord("catch")) {
            Clause c;
            c.loc = take().loc;
            if (isPunct("(")) {
                take();
                if (isWord("string") || isWord("var")) fail(peek(), "since 0.8 a catch variable is an Exception object: write catch (e) and use e.Message (or catch (e: SomeError))");
                c.var = expectName("a name for the caught exception");
                if (isPunct(":")) {
                    take();
                    c.typed = true;
                    c.type = typeName();
                    if (c.type.kind != Type::Class) fail(peek(), "a catch type must be a class derived from Exception");
                }
                expectPunct(")", "after the catch variable");
            }
            c.body = block("to start the 'catch' body");
            if (!clauses.empty() && (!clauses.back().typed || clauses.back().type.name == "Exception"))
                fail(peek(), "this catch can never run: an earlier 'catch' already catches every exception");
            clauses.push_back(std::move(c));
        }
        if (isWord("finally")) {
            take();
            s->hasFinally = true;
            s->finallyBody = block("to start the 'finally' body");
        }
        if (clauses.empty() && !s->hasFinally) fail(peek(), "'try' needs a 'catch' or 'finally' block (expected 'catch', found " + describe(peek()) + ")");
        if (clauses.empty()) return s;
        const SourceLoc at = s->loc;
        const std::string ex = "$ex" + std::to_string(catchCounter_++);
        s->hasElse = true;
        {   // var $ex = Exception.Caught()
            auto call = std::make_unique<Expr>();
            call->kind = Expr::Kind::MethodCall;
            call->loc = at;
            call->text = "Caught";
            call->lhs = nameExpr("Exception", at);
            measure(*call);
            auto decl = make(Stmt::Kind::VarDecl, at);
            decl->name = ex;
            decl->inferred = true;
            decl->expr = std::move(call);
            s->elseBody.push_back(std::move(decl));
        }
        StmtPtr chain;
        bool hasAll = false;
        for (size_t k = clauses.size(); k-- > 0;) {
            Clause& c = clauses[k];
            const bool all = !c.typed || c.type.name == "Exception";
            std::vector<StmtPtr> body;
            if (!c.var.empty()) {
                auto decl = make(Stmt::Kind::VarDecl, c.loc);
                decl->name = c.var;
                decl->inferred = true;
                if (all) {
                    decl->expr = nameExpr(ex, c.loc);
                } else {
                    auto as = std::make_unique<Expr>();
                    as->kind = Expr::Kind::Convert;
                    as->conv = Expr::Conv::Downcast;
                    as->loc = c.loc;
                    as->lhs = nameExpr(ex, c.loc);
                    as->newType = c.type;
                    measure(*as);
                    decl->expr = std::move(as);
                }
                body.push_back(std::move(decl));
            }
            for (auto& b : c.body) body.push_back(std::move(b));
            if (all) {  // the catch-all: it is the last clause, everything before it is tested first
                hasAll = true;
                auto blk = make(Stmt::Kind::Block, c.loc);
                blk->body = std::move(body);
                chain = std::move(blk);
                continue;
            }
            auto test = std::make_unique<Expr>();
            test->kind = Expr::Kind::Is;
            test->loc = c.loc;
            test->lhs = nameExpr(ex, c.loc);
            test->newType = c.type;
            measure(*test);
            auto ifs = make(Stmt::Kind::If, c.loc);
            ifs->expr = std::move(test);
            ifs->body = std::move(body);
            if (chain) {
                ifs->hasElse = true;
                ifs->elseBody.push_back(std::move(chain));
            } else {  // nothing catches it: rethrow
                auto thr = make(Stmt::Kind::Throw, c.loc);
                thr->expr = nameExpr(ex, c.loc);
                ifs->hasElse = true;
                ifs->elseBody.push_back(std::move(thr));
            }
            chain = std::move(ifs);
        }
        (void)hasAll;
        s->elseBody.push_back(std::move(chain));
        return s;
    }

    StmtPtr whileStatement() {
        auto s = condition(make(Stmt::Kind::While, peek().loc), "while");
        s->body = bodyOrStatement("to start the 'while' body");
        return s;
    }

    // for (init; condition; step) { ... } -- each part is optional; a missing condition means true.
    // in (var x of items) { ... }   (since 0.6.3; the old spelling for (var x in items) is an error)
    StmtPtr foreachStatement() {
        Depth guard(*this, peek());
        auto s = make(Stmt::Kind::ForEach, take().loc);
        expectPunct("(", "after 'in'");
        if (isWord("var")) {
            take();
            s->inferred = true;
        } else {
            s->declType = typeName();
        }
        s->name = expectName("a loop variable name");
        if (!isWord("of")) fail(peek(), "expected 'of' after the loop variable (in (var x of items) { ... }), found " + describe(peek()));
        take();  // 'of'
        s->expr = expression();
        expectPunct(")", "after the collection of the 'in' loop");
        s->body = bodyOrStatement("to start the 'in' body");
        return s;
    }

    StmtPtr forStatement() {
        Depth guard(*this, peek());
        auto s = make(Stmt::Kind::For, take().loc);
        expectPunct("(", "after 'for'");
        if (peek().kind == Tok::Ident && (isTypeWord(peek().text) || startsClassDeclaration()) && forEachAhead())
            fail(peek(), "'for (x in items)' was replaced in Coyote 0.6.3: write in (var x of items) { ... }; 'for' is only for for (init; condition; step)");
        if (!isPunct(";")) {
            if ((peek().kind == Tok::Ident && isTypeWord(peek().text) && !tokIsPunct(peekNext(), "(")) || startsClassDeclaration() || fnDeclarationAhead())
                s->init = varDecl(false);
            else
                s->init = exprStatement(false);
        }
        expectPunct(";", "after the 'for' initializer");
        if (!isPunct(";")) s->expr = expression();
        expectPunct(";", "after the 'for' condition");
        if (!isPunct(")")) s->step = exprStatement(false);
        expectPunct(")", "after the 'for' header");
        s->body = bodyOrStatement("to start the 'for' body");
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
            } else if (tokIsPunct(t, "(") && e->kind != Expr::Kind::Lambda) {  // calling a function value: f(1)(2), obj.Hook()()
                auto n = std::make_unique<Expr>();
                n->kind = Expr::Kind::CallFn;
                n->loc = t.loc;
                n->lhs = std::move(e);
                n->args = argList();
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
            {"||"}, {"&&"}, {"|"}, {"^"}, {"&"}, {"==", "!="}, {"<", "<=", ">", ">="}, {"<<", ">>"}, {"+", "-"}, {"*", "/", "%"}};
        constexpr size_t kRelational = 6, kShift = 7;
        if (level == levels.size()) return unary();
        ExprPtr lhs = binary(level + 1);
        for (;;) {
            const Token& t = peek();
            if (level == kShift && t.kind == Tok::Punct && t.text == ">" && pos_ + 1 < toks_.size() && toks_[pos_ + 1].kind == Tok::Punct &&
                toks_[pos_ + 1].text == ">" && toks_[pos_ + 1].loc.line == t.loc.line && toks_[pos_ + 1].loc.col == t.loc.col + 1 && !toks_[pos_ + 1].nlBefore &&
                !(pos_ + 2 < toks_.size() && toks_[pos_ + 2].text == "=" && toks_[pos_ + 2].loc.line == t.loc.line && toks_[pos_ + 2].loc.col == t.loc.col + 2)) {
                // '>>' is two adjacent '>' tokens (the lexer keeps them apart so that List<List<int>> still parses)
                auto e = std::make_unique<Expr>();
                e->kind = Expr::Kind::Binary;
                e->loc = t.loc;
                e->text = ">>";
                take();
                take();
                e->lhs = std::move(lhs);
                e->rhs = binary(level + 1);
                measure(*e);
                lhs = std::move(e);
                continue;
            }
            if (level == kRelational && isWord("is")) {
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
            if (level == kRelational && isWord("as")) {  // downcast: animal as Dog
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
        if (t.kind == Tok::Punct && (t.text == "-" || t.text == "!" || t.text == "~")) {
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Kind::Unary;
            e->loc = t.loc;
            e->text = t.text;
            take();
            e->lhs = unary();
            measure(*e);
            return e;
        }
        if (t.kind == Tok::Punct && t.text == "(" && peekAt(1).kind == Tok::Ident && (peekAt(1).text == "copy" || peekAt(1).text == "ref") &&
            tokIsPunct(peekAt(2), ")") &&
            (peekAt(3).kind != Tok::Punct || tokIsPunct(peekAt(3), "("))) {  // (copy)expr  /  (ref)variable
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Kind::Unary;
            e->loc = t.loc;
            e->text = "(" + peekAt(1).text + ")";
            take(); take(); take();
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
                e->isLong = t.isLong;
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
                if (!keywords().count(t.text) && tokIsPunct(peekNext(), "=>")) {  // x => body: one untyped parameter
                    Depth guard(*this, peek());
                    e->kind = Expr::Kind::Lambda;
                    Param prm;
                    prm.loc = peek().loc;
                    prm.type = Type::Invalid;
                    prm.name = take().text;
                    e->lparams.push_back(std::move(prm));
                    take();  // '=>'
                    if (isPunct("{")) {
                        e->lbody = block("to start the lambda body");
                    } else {
                        auto r = make(Stmt::Kind::Return, peek().loc);
                        r->expr = expression();
                        e->lbody.push_back(std::move(r));
                        e->exprBody = true;
                    }
                    measure(*e);
                    return e;
                }
                if (t.text == "switch" && tokIsPunct(peekNext(), "(")) return switchExpression();
                if (t.text == "null") {
                    e->kind = Expr::Kind::Null;
                    take();
                    return e;
                }
                if ((t.text == "int" || t.text == "long" || t.text == "byte" || t.text == "double" || t.text == "string" || t.text == "char" || t.text == "Guid") &&
                    tokIsPunct(peekNext(), "(")) {  // conversion: int(x), double(x), string(x), char(x), Guid(x)
                    e->kind = Expr::Kind::Convert;
                    e->newType = baseType();
                    take();  // '('
                    e->lhs = expression();
                    expectPunct(")", "to close the conversion");
                    measure(*e);
                    return e;
                }
                failIfRenamed(t);
                if (t.text == "here") {  // the current object; sema calls its slot "this" internally
                    e->kind = Expr::Kind::Var;
                    take();
                    e->text = "this";
                    return e;
                }
                if (t.text == "new") {
                    take();
                    if (peek().kind == Tok::Ident && !keywords().count(peek().text) && !unsupportedWords().count(peek().text)) {
                        e->kind = Expr::Kind::NewObj;  // new Point(1, 2)
                        e->loc = peek().loc;
                        e->text = qualifiedName();
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
                if (t.text == "(" && lambdaAhead()) return lambdaExpr();
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
