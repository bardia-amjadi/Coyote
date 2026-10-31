// cdt: Coyote developer tools. Source analysis for people and editors: tokens, AST dump, machine-
// readable diagnostics, a formatter and a toolchain health check. See docs/cdt.md.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "ast.hpp"
#include "format.hpp"
#include "lexer.hpp"
#include "modules.hpp"
#include "mlgen.hpp"
#include "parser.hpp"
#include "process.hpp"
#include "sema.hpp"

#ifndef COYOTE_VERSION
#define COYOTE_VERSION "0.0.0"
#endif

namespace {

using namespace coyote;
namespace fs = std::filesystem;

constexpr int kExitProblem = 1;  // diagnostics found / file would change / tool missing
constexpr int kExitUsage = 2;
constexpr std::streamoff kMaxSourceBytes = 4 * 1024 * 1024;

void usage(std::ostream& os) {
    os << "cdt " COYOTE_VERSION " - Coyote developer tools\n"
       << "usage:\n"
       << "  cdt check [--json] <file.coy>   report diagnostics (--json: one JSON array on stdout, for editors)\n"
       << "  cdt tokens <file.coy>           print the token stream\n"
       << "  cdt ast <file.coy>              print the syntax tree (before semantic analysis)\n"
       << "  cdt fmt [--check|--write] <file.coy>...   format source (default: print the result)\n"
       << "  cdt verify-ml <file.ml>        verify ML with the PVM, without running it\n"
       << "  cdt doctor                     check that the toolchain is installed and consistent\n"
       << "exit codes: 0 ok; 1 problems found (diagnostics, files that need formatting, missing tools); 2 usage or I/O error\n";
}

bool readFile(const std::string& path, std::string& out, int& rc) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { std::cerr << "cdt: error: cannot read '" << path << "'\n"; rc = kExitUsage; return false; }
    in.seekg(0, std::ios::end);
    if (in.tellg() > kMaxSourceBytes) {
        std::cerr << "cdt: error: '" << path << "' is larger than the 4 MiB source file limit\n";
        rc = kExitUsage;
        return false;
    }
    in.seekg(0, std::ios::beg);
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

std::string jsonStr(const std::string& s) {
    std::string r = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\t': r += "\\t"; break;
            case '\r': r += "\\r"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); r += b; }
                else r += static_cast<char>(c);
        }
    }
    return r + "\"";
}

// ---- tokens ----

const char* tokKind(Tok k) {
    switch (k) {
        case Tok::Ident: return "ident";
        case Tok::Int: return "int";
        case Tok::Double: return "double";
        case Tok::Char: return "char";
        case Tok::String: return "string";
        case Tok::Punct: return "punct";
        case Tok::Eof: return "eof";
    }
    return "?";
}

// ---- AST dump ----

std::string expr(const Expr& e);

std::string exprs(const std::vector<ExprPtr>& v) {
    std::string r;
    for (const auto& x : v) r += " " + expr(*x);
    return r;
}

std::string expr(const Expr& e) {
    switch (e.kind) {
        case Expr::Kind::String: return "(string " + jsonStr(e.text) + ")";
        case Expr::Kind::Int: return "(int " + std::to_string(e.ival) + ")";
        case Expr::Kind::Bool: return std::string("(bool ") + (e.ival ? "true" : "false") + ")";
        case Expr::Kind::Double: return "(double " + std::to_string(e.dval) + ")";
        case Expr::Kind::Char: return "(char " + std::to_string(e.ival) + ")";
        case Expr::Kind::Null: return "(null)";
        case Expr::Kind::NewMap: return "(new " + typeName(e.newType) + ")";
        case Expr::Kind::Convert: return "(convert " + typeName(e.newType) + " " + expr(*e.lhs) + ")";
        case Expr::Kind::Unwrap: return "(unwrap " + expr(*e.lhs) + ")";
        case Expr::Kind::IsNull: return "(is-null " + expr(*e.lhs) + ")";
        case Expr::Kind::Is: return "(is " + expr(*e.lhs) + " " + typeName(e.newType) + ")";
        case Expr::Kind::Var: return "(var " + e.text + ")";
        case Expr::Kind::Call: return "(call " + e.text + exprs(e.args) + ")";
        case Expr::Kind::Unary: return "(unary " + e.text + " " + expr(*e.lhs) + ")";
        case Expr::Kind::Binary: return "(binary " + e.text + " " + expr(*e.lhs) + " " + expr(*e.rhs) + ")";
        case Expr::Kind::ArrayLit: return "(array" + exprs(e.args) + ")";
        case Expr::Kind::NewArray: return "(new " + typeName(e.newType) + " " + expr(*e.lhs) + ")";
        case Expr::Kind::Index: return "(index " + expr(*e.lhs) + " " + expr(*e.rhs) + ")";
        case Expr::Kind::Member: return "(member " + expr(*e.lhs) + " " + e.text + ")";
        case Expr::Kind::NewObj: return "(new-object " + e.text + exprs(e.args) + ")";
        case Expr::Kind::MethodCall: return "(method-call " + expr(*e.lhs) + " " + e.text + exprs(e.args) + ")";
    }
    return "?";
}

void stmts(std::ostream& os, const std::vector<StmtPtr>& v, int ind);

void stmt(std::ostream& os, const Stmt& s, int ind) {
    std::string pad(static_cast<size_t>(ind) * 2, ' ');
    os << pad;
    switch (s.kind) {
        case Stmt::Kind::ExprStmt: os << "expr " << expr(*s.expr) << "\n"; return;
        case Stmt::Kind::VarDecl:
            os << "var " << s.name << " : " << (s.inferred ? "inferred" : typeName(s.declType)) << " = " << expr(*s.expr) << "\n";
            return;
        case Stmt::Kind::Assign: os << "assign " << expr(*s.target) << " = " << expr(*s.expr) << "\n"; return;
        case Stmt::Kind::If:
            os << "if " << expr(*s.expr) << "\n";
            stmts(os, s.body, ind + 1);
            if (s.hasElse) {
                os << pad << "else\n";
                stmts(os, s.elseBody, ind + 1);
            }
            return;
        case Stmt::Kind::While: os << "while " << expr(*s.expr) << "\n"; stmts(os, s.body, ind + 1); return;
        case Stmt::Kind::For:
            os << "for\n";
            if (s.init) { os << pad << " init\n"; stmt(os, *s.init, ind + 2); }
            if (s.expr) os << pad << " cond " << expr(*s.expr) << "\n";
            if (s.step) { os << pad << " step\n"; stmt(os, *s.step, ind + 2); }
            os << pad << " body\n";
            stmts(os, s.body, ind + 2);
            return;
        case Stmt::Kind::ForEach:
            os << "foreach " << s.name << " : " << (s.inferred ? "inferred" : typeName(s.declType)) << " in " << expr(*s.expr) << "\n";
            stmts(os, s.body, ind + 1);
            return;
        case Stmt::Kind::Break: os << "break\n"; return;
        case Stmt::Kind::Continue: os << "continue\n"; return;
        case Stmt::Kind::Return: os << "return" << (s.expr ? " " + expr(*s.expr) : "") << "\n"; return;
        case Stmt::Kind::Block: os << "block\n"; stmts(os, s.body, ind + 1); return;
    }
}

void stmts(std::ostream& os, const std::vector<StmtPtr>& v, int ind) {
    for (const auto& s : v) stmt(os, *s, ind);
}

// ---- commands ----

int cmdTokens(const std::string& path) {
    std::string src;
    int rc = 0;
    if (!readFile(path, src, rc)) return rc;
    try {
        for (const Token& t : lex(src, path)) {
            std::cout << t.loc.line << ":" << t.loc.col << " " << tokKind(t.kind);
            if (t.kind == Tok::Int || t.kind == Tok::Char) std::cout << " " << t.ival;
            else if (t.kind == Tok::String) std::cout << " " << jsonStr(t.text);
            else if (t.kind != Tok::Eof) std::cout << " " << t.text;
            std::cout << (t.nlBefore ? "  (new line)" : "") << "\n";
        }
    } catch (const CompileError& e) {
        std::cerr << formatDiagnostic(e.diag) << "\n";
        return kExitProblem;
    }
    return 0;
}

int cmdAst(const std::string& path) {
    std::string src;
    int rc = 0;
    if (!readFile(path, src, rc)) return rc;
    try {
        Program p = parse(lex(src, path), path);
        for (const HandDecl& h : p.hands) {
            std::cout << "hand:" << h.module << " " << h.name << "(";
            for (size_t i = 0; i < h.params.size(); ++i) std::cout << (i ? ", " : "") << typeName(h.params[i].type) << " " << h.params[i].name;
            std::cout << ")\n";
        }
        for (const ClassDecl& c : p.classes) {
            std::cout << (c.isModel ? "model " : "class ") << c.name << "\n";
            for (const FieldDecl& f : c.fields) std::cout << "  field " << f.name << " : " << typeName(f.type) << "\n";
            for (const PropertyDecl& pr : c.properties) std::cout << "  property " << pr.name << " : " << typeName(pr.type) << "\n";
            for (const MethodDecl& m : c.methods) {
                std::cout << "  " << (m.isCtor ? "constructor" : "method " + m.name) << "(";
                for (size_t i = 0; i < m.params.size(); ++i) std::cout << (i ? ", " : "") << typeName(m.params[i].type) << " " << m.params[i].name;
                std::cout << ")\n";
                stmts(std::cout, m.body, 2);
            }
        }
        for (const MethodDecl& m : p.methods) {
            std::cout << "method " << m.name << "(";
            for (size_t i = 0; i < m.params.size(); ++i) std::cout << (i ? ", " : "") << typeName(m.params[i].type) << " " << m.params[i].name;
            std::cout << ") : " << typeName(m.ret) << "\n";
            stmts(std::cout, m.body, 1);
        }
        stmts(std::cout, p.stmts, 0);
    } catch (const CompileError& e) {
        std::cerr << formatDiagnostic(e.diag) << "\n";
        return kExitProblem;
    }
    return 0;
}

int cmdCheck(const std::string& path, bool json) {
    std::string src;
    int rc = 0;
    if (!readFile(path, src, rc)) return rc;
    std::vector<Diagnostic> diags;
    Program p;
    std::string readError;
    if (loadProgram(path, p, diags, readError)) analyze(p, path, diags);
    if (json) {
        std::cout << "[";
        for (size_t i = 0; i < diags.size(); ++i)
            std::cout << (i ? ",\n " : "\n ") << "{\"file\": " << jsonStr(diags[i].file) << ", \"line\": " << diags[i].loc.line
                      << ", \"column\": " << diags[i].loc.col << ", \"severity\": \"error\", \"message\": " << jsonStr(diags[i].message) << "}";
        std::cout << (diags.empty() ? "]\n" : "\n]\n");
    } else {
        for (const Diagnostic& d : diags) std::cerr << formatDiagnostic(d) << "\n";
        if (!diags.empty()) std::cerr << diags.size() << " error(s)\n";
    }
    return diags.empty() ? 0 : kExitProblem;
}

int cmdFmt(const std::vector<std::string>& args) {
    bool check = false, write = false;
    std::vector<std::string> files;
    for (const std::string& a : args) {
        if (a == "--check") check = true;
        else if (a == "--write") write = true;
        else if (!a.empty() && a[0] == '-') { std::cerr << "cdt: error: unknown option '" << a << "'\n"; return kExitUsage; }
        else files.push_back(a);
    }
    if (check && write) { std::cerr << "cdt: error: --check and --write cannot be combined\n"; return kExitUsage; }
    if (files.empty()) { std::cerr << "cdt: error: no files given\n"; return kExitUsage; }
    if (files.size() > 1 && !check && !write) { std::cerr << "cdt: error: printing needs exactly one file (use --check or --write for several)\n"; return kExitUsage; }

    int result = 0;
    for (const std::string& f : files) {
        std::string src;
        int rc = 0;
        if (!readFile(f, src, rc)) return rc;
        std::string out;
        try {
            out = formatSource(src, f);
        } catch (const CompileError& e) {
            std::cerr << formatDiagnostic(e.diag) << "\n";
            result = kExitProblem;
            continue;
        }
        bool changed = out != src;
        if (check) {
            if (changed) { std::cout << f << ": needs formatting\n"; result = kExitProblem; }
        } else if (write) {
            if (changed) {
                std::ofstream o(f, std::ios::binary);
                if (!o || !(o << out)) { std::cerr << "cdt: error: cannot write '" << f << "'\n"; return kExitUsage; }
                std::cout << "formatted " << f << "\n";
            }
        } else {
            std::cout << out;
        }
    }
    return result;
}

int cmdVerifyMl(const std::string& path, const std::string& selfDir) {
    coyote::pkg::ProcessResult r = coyote::pkg::runProcess({"pvm", "--verify-only", path}, selfDir);
    if (!r.started) { std::cerr << "cdt: error: " << r.error << "\n"; return kExitProblem; }
    if (r.exitCode == 0) std::cout << path << ": verified\n";
    return r.exitCode == 0 ? 0 : (r.exitCode == 1 ? kExitProblem : kExitUsage);
}

int cmdDoctor(const std::string& selfDir) {
    int missing = 0;
    std::cout << "cdt " COYOTE_VERSION ", ML format version " << kMlVersion << "\n";
    std::cout << "tools directory: " << (selfDir.empty() ? "(unknown)" : selfDir) << "\n";
    struct Tool { const char* name; bool required; };
    for (Tool t : {Tool{"coyote", true}, Tool{"pvm", true}, Tool{"cpx", true}, Tool{"cpm", true}, Tool{"coyote-llvm", false}}) {
        std::string out;
        coyote::pkg::ProcessResult r = coyote::pkg::runProcessCapture({t.name, "--version"}, selfDir, out);
        if (r.started && r.exitCode == 0) {
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
            std::cout << "  ok       " << t.name << "  (" << out << ")\n";
        } else {
            std::cout << (t.required ? "  MISSING  " : "  absent   ") << t.name << (t.required ? "" : "  (optional native backend)") << "\n";
            if (t.required) ++missing;
        }
    }
    {
        std::string out;
        coyote::pkg::ProcessResult r = coyote::pkg::runProcessCapture({"git", "--version"}, "", out);
        std::cout << (r.started && r.exitCode == 0 ? "  ok       git  (needed for git dependencies)\n" : "  absent   git  (needed only for git dependencies)\n");
    }
    std::cout << (missing ? "problems found\n" : "all required tools found\n");
    return missing ? kExitProblem : 0;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) { usage(std::cerr); return kExitUsage; }
    const std::string cmd = args[0];
    if (cmd == "--help" || cmd == "-h") { usage(std::cout); return 0; }
    if (cmd == "--version") { std::cout << "cdt " COYOTE_VERSION "\n"; return 0; }
    std::vector<std::string> rest(args.begin() + 1, args.end());
    const std::string selfDir = coyote::pkg::executableDir(argv[0]);

    if (cmd == "fmt") return cmdFmt(rest);
    if (cmd == "doctor") {
        if (!rest.empty()) { std::cerr << "cdt: error: 'doctor' takes no arguments\n"; return kExitUsage; }
        return cmdDoctor(selfDir);
    }
    bool json = false;
    std::vector<std::string> files;
    for (const std::string& a : rest) {
        if (a == "--json" && cmd == "check") json = true;
        else if (!a.empty() && a[0] == '-') { std::cerr << "cdt: error: unknown option '" << a << "'\n"; return kExitUsage; }
        else files.push_back(a);
    }
    if (cmd != "check" && cmd != "tokens" && cmd != "ast" && cmd != "verify-ml") {
        std::cerr << "cdt: error: unknown command '" << cmd << "'\n";
        usage(std::cerr);
        return kExitUsage;
    }
    if (files.size() != 1) { std::cerr << "cdt: error: '" << cmd << "' needs exactly one file\n"; return kExitUsage; }
    if (cmd == "check") return cmdCheck(files[0], json);
    if (cmd == "tokens") return cmdTokens(files[0]);
    if (cmd == "ast") return cmdAst(files[0]);
    return cmdVerifyMl(files[0], selfDir);
}
