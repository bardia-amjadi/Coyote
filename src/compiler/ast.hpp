#pragma once

#include <memory>
#include <string>
#include <vector>

#include "diag.hpp"

namespace coyote {

// A Coyote type. Scalars are identified by `kind`; `Array` carries its element type; `Map` its key
// (`key`) and value (`elem`) types; `Nullable` the wrapped type (`elem`).
// Invalid is the type of an expression that already produced an error; it suppresses follow-up
// errors so one mistake yields one diagnostic. `Null` is the type of the literal `null` before it
// is coerced to a nullable/obj/dyn type. `Dyn` is `obj` plus runtime-validated operations.
struct Type {
    enum Kind { Void, Int, String, Bool, Invalid, Array, Double, Char, Guid, Map, Nullable, Obj, Dyn, Null, Class };

    Kind kind = Invalid;
    std::shared_ptr<const Type> elem;  // Array/Nullable element, Map value
    std::shared_ptr<const Type> key;   // Map key
    std::string name;                  // Class: the class name (as written; unique ML name after semantic analysis)

    Type() = default;
    Type(Kind k) : kind(k) {}  // implicit, so `Type::Int` can be used wherever a Type is expected

    static Type arrayOf(const Type& element) {
        Type t(Array);
        t.elem = std::make_shared<const Type>(element);
        return t;
    }
    static Type mapOf(const Type& k, const Type& v) {
        Type t(Map);
        t.key = std::make_shared<const Type>(k);
        t.elem = std::make_shared<const Type>(v);
        return t;
    }
    static Type nullableOf(const Type& inner) {
        Type t(Nullable);
        t.elem = std::make_shared<const Type>(inner);
        return t;
    }

    static Type classOf(const std::string& n) {
        Type t(Class);
        t.name = n;
        return t;
    }
    bool isArray() const { return kind == Array; }
    bool isMap() const { return kind == Map; }
    bool isNullable() const { return kind == Nullable; }
    bool isRefLike() const { return kind == Obj || kind == Dyn; }  // can hold null without T?

    friend bool operator==(const Type& a, const Type& b) {
        if (a.kind != b.kind) return false;
        if (a.kind == Map) return *a.key == *b.key && *a.elem == *b.elem;
        if (a.kind == Class) return a.name == b.name;
        return (a.kind != Array && a.kind != Nullable) || *a.elem == *b.elem;
    }
    friend bool operator!=(const Type& a, const Type& b) { return !(a == b); }
};

// Name used in diagnostics.
inline std::string typeName(const Type& t) {
    switch (t.kind) {
        case Type::Void: return "void";
        case Type::Int: return "int";
        case Type::String: return "string";
        case Type::Bool: return "boolean";
        case Type::Invalid: return "<error>";
        case Type::Array: return "array<" + typeName(*t.elem) + ">";
        case Type::Double: return "double";
        case Type::Char: return "char";
        case Type::Guid: return "Guid";
        case Type::Map: return "map<" + typeName(*t.key) + ", " + typeName(*t.elem) + ">";
        case Type::Nullable: return typeName(*t.elem) + "?";
        case Type::Obj: return "obj";
        case Type::Dyn: return "dyn";
        case Type::Null: return "null";
        case Type::Class: return t.name;
    }
    return "?";
}

// Name used in the emitted ML (no spaces; `dyn` is `obj` at the ML level).
inline std::string mlTypeName(const Type& t) {
    switch (t.kind) {
        case Type::Array: return "array<" + mlTypeName(*t.elem) + ">";
        case Type::Map: return "map<" + mlTypeName(*t.key) + "," + mlTypeName(*t.elem) + ">";
        case Type::Nullable: return mlTypeName(*t.elem) + "?";
        case Type::Dyn: return "obj";
        default: return typeName(t);
    }
}

struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

struct Expr {
    enum class Kind { String, Int, Bool, Double, Char, Null, Var, Call, Unary, Binary, ArrayLit, NewArray, NewMap, NewObj, Index,
                      Member, MethodCall, Convert, Unwrap, IsNull, Is };
    // Convert: how `lhs` is turned into `type` (inserted by sema, or written as int(x), double(x), ...).
    enum class Conv { IntToDouble, DoubleToInt, CharToInt, IntToChar, StringToInt, StringToDouble, StringToGuid,
                      ToString, Wrap, Box, Unbox, Retype };
    Kind kind = Kind::String;
    SourceLoc loc;               // Unary/Binary: the operator; Index: the '['; Member/MethodCall: the member name
    std::string text;            // string value, variable/callee/member name, or operator
    long long ival = 0;          // Int value, Bool (0/1), Char code point; IsNull: 1 for '!= null'
    double dval = 0;             // Double value
    Conv conv = Conv::Retype;    // Convert only
    ExprPtr lhs, rhs;            // Unary: lhs. Index: lhs[rhs]. Member/MethodCall: lhs is the receiver.
                                 // NewArray: lhs is the size.
    std::vector<ExprPtr> args;   // Call/MethodCall arguments, ArrayLit elements
    Type newType;                // NewArray/NewMap: the type being created; Is/explicit conversions: the target type
    int depth = 1;               // height of this expression tree (set by the parser; bounded)

    // Filled in by semantic analysis.
    Type type = Type::Void;
    int hostIndex = -1;          // Call to a hand declaration
    int methodIndex = -1;        // Call to a method
    int slot = -1;               // Var: local slot
    int classIndex = -1;         // Member/MethodCall on a class: the class (Program::classes); NewObj: the class
    bool isStatic = false;       // Member: a static field; MethodCall: a static method (no receiver value)
};

struct Stmt {
    enum class Kind { ExprStmt, VarDecl, Assign, If, While, For, ForEach, Break, Continue, Return, Block };
    Kind kind = Kind::ExprStmt;
    SourceLoc loc;
    ExprPtr expr;                // ExprStmt call, initializer, assigned value, condition (For: may be null), return value
    ExprPtr target;              // Assign: a Var or Index expression
    std::string name;            // VarDecl
    bool inferred = false;       // VarDecl: declared with 'var'
    Type declType = Type::Invalid;
    StmtPtr init, step;          // For: optional initializer (declaration or assignment) and step (assignment or call)
    std::vector<StmtPtr> body;   // If-then, While, For, Block
    std::vector<StmtPtr> elseBody;
    bool hasElse = false;

    int slot = -1;               // VarDecl/ForEach: local slot of the variable (filled by sema)
    int slotArr = -1, slotIdx = -1;  // ForEach: hidden locals holding the array and the index
};

struct Param {
    Type type = Type::String;   // a variadic parameter has type array<T>
    std::string name;
    SourceLoc loc;
    bool variadic = false;      // declared as `T... name`; callers may pass any number of T (or one array<T>)
};

// hand:<module> <name>(<params>) [: <type>]  -- declares a host intrinsic.
struct HandDecl {
    int unit = 0;                // index into Program::modules (the file it was declared in)
    SourceLoc loc;
    std::string module;
    std::string name;
    std::vector<Param> params;
    Type ret = Type::Void;       // host functions may return int, double, boolean, char or string
};

// method <name>(<params>) [: <type>] { ... }
struct MethodDecl {
    int classIndex = -1;         // index into Program::classes when declared inside a class
    bool isStatic = false;
    bool isPrivate = false;
    bool isCtor = false;         // the constructor (named "$new"); builds `this`, runs field initializers, returns it
    bool isExtension = false;    // `method name(this T self, ...)`: callable as self.name(...)
    bool isStaticInit = false;   // synthesized: runs a class's static field initializers
    int thisSlot = -1;           // constructors: the local slot that holds the new object (set by sema)
    int unit = 0;                // index into Program::modules (the file it was declared in)
    bool exported = false;       // declared with 'export'
    SourceLoc loc;               // the name
    std::string name;
    std::vector<Param> params;
    Type ret = Type::Void;
    std::vector<StmtPtr> body;
    std::vector<Type> localTypes;  // non-parameter locals, in slot order (filled by sema)
    std::vector<std::string> localNames;  // their names (debug information)
};

struct FieldDecl {
    SourceLoc loc;
    std::string name;
    Type type;
    ExprPtr init;                // optional initializer (moved into the constructor / static initializer by sema)
    bool isPrivate = false;
    bool isStatic = false;
};

struct PropertyDecl {
    SourceLoc loc;
    std::string name;
    Type type;
    bool isPrivate = false;
    bool hasGet = false, hasSet = false;  // the accessors are methods "get_<name>" / "set_<name>" of the class
};

// class <name> { members }   or   model <name> { fields }
struct ClassDecl {
    int unit = 0;                // index into Program::modules
    bool exported = false;
    bool isModel = false;        // a constructor taking every field in order is generated when none is declared
    SourceLoc loc;
    std::string name;
    std::vector<FieldDecl> fields;
    std::vector<PropertyDecl> properties;
    std::vector<MethodDecl> methods;  // parser output; the loader moves them into Program::methods
};

// The ML name of a class: its own name in the entry file, prefixed with the file index elsewhere.
inline std::string classMlName(const ClassDecl& c) { return c.unit == 0 ? c.name : "m" + std::to_string(c.unit) + "." + c.name; }

// import "<path or package>"
struct ImportDecl {
    SourceLoc loc;
    std::string spec;
    bool isUse = false;  // `use "Name"` / `use "./file.lib"` (a library) instead of `import`
};

// One source file of the program. Module 0 is the entry file.
struct Module {
    std::string path;                 // as shown in diagnostics and in the ML '.file' directive
    std::vector<ImportDecl> imports;
    std::vector<int> importIndex;     // resolved module of each import (filled by the loader)
    std::vector<int> libSiblings;     // other files of the same .lib (they see each other's exports)
    std::string libName;              // non-empty for a file that came out of a .lib
    std::vector<std::string> libRequires;  // libraries that .lib asks to load with it (made visible to this file)
};

struct Program {
    std::vector<ImportDecl> imports;  // parser output for a single file; the loader moves them into `modules`
    std::vector<Module> modules;      // filled by the loader (a bare parse leaves it empty)
    std::vector<ClassDecl> classes;
    std::vector<HandDecl> hands;
    std::vector<MethodDecl> methods;
    std::vector<StmtPtr> stmts;      // top-level statements, run in order as the entry function
    std::vector<std::string> mainLocalNames;
    std::vector<Type> mainLocals;    // locals of the top-level code (filled by sema)
};

} // namespace coyote
