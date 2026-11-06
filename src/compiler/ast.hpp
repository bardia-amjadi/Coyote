#pragma once

#include <map>
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
    enum Kind { Void, Int, String, Bool, Invalid, Array, Double, Char, Guid, Map, Nullable, Obj, Dyn, Null, Class, Fn };

    Kind kind = Invalid;
    std::shared_ptr<const Type> elem;  // Array/Nullable element, Map value
    std::shared_ptr<const Type> key;   // Map key
    std::string name;                  // Class: the class name (as written; unique ML name after semantic analysis)
    std::vector<Type> targs;           // Class: generic arguments as written (`Box<int>`); empty once instantiated

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

    // Function type: `fn(int, string) : boolean`. The parameter types are kept in `targs`, the result type in `elem`.
    static Type fnOf(const std::vector<Type>& params, const Type& ret) {
        Type t(Fn);
        t.targs = params;
        t.elem = std::make_shared<const Type>(ret);
        return t;
    }
    bool isFn() const { return kind == Fn; }

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
        if (a.kind == Class) return a.name == b.name && a.targs == b.targs;
        if (a.kind == Fn) return a.targs == b.targs && *a.elem == *b.elem;
        return (a.kind != Array && a.kind != Nullable) || *a.elem == *b.elem;
    }
    friend bool operator!=(const Type& a, const Type& b) { return !(a == b); }
};

// Readable names of instantiated generic classes (mangled ML name -> `Box<int>`), for diagnostics.
inline std::map<std::string, std::string>& genericDisplayNames() {
    static std::map<std::string, std::string> names;
    return names;
}

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
        case Type::Fn: {
            std::string s = "fn(";
            for (size_t i = 0; i < t.targs.size(); ++i) s += (i ? ", " : "") + typeName(t.targs[i]);
            s += ")";
            if (*t.elem != Type::Void) s += " : " + typeName(*t.elem);
            return s;
        }
        case Type::Class: {
            if (!t.targs.empty()) {
                std::string s = t.name + "<";
                for (size_t i = 0; i < t.targs.size(); ++i) s += (i ? ", " : "") + typeName(t.targs[i]);
                return s + ">";
            }
            auto it = genericDisplayNames().find(t.name);
            return it == genericDisplayNames().end() ? t.name : it->second;
        }
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
        case Type::Fn: {
            std::string s = "fn(";
            for (size_t i = 0; i < t.targs.size(); ++i) s += (i ? "," : "") + mlTypeName(t.targs[i]);
            return s + ")->" + mlTypeName(*t.elem);
        }
        case Type::Class: return t.name;  // the mangled name, never the display name
        default: return typeName(t);
    }
}

struct Param {
    Type type = Type::String;   // a variadic parameter has type array<T>
    std::string name;
    SourceLoc loc;
    bool variadic = false;      // declared as `T... name`; callers may pass any number of T (or one array<T>)
};

struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

struct Expr {
    enum class Kind { String, Int, Bool, Double, Char, Null, Var, Call, Unary, Binary, ArrayLit, NewArray, NewMap, NewObj, Index,
                      Member, MethodCall, Convert, Unwrap, IsNull, Is, Lambda, CallFn };
    // Convert: how `lhs` is turned into `type` (inserted by sema, or written as int(x), double(x), ...).
    enum class Conv { IntToDouble, DoubleToInt, CharToInt, IntToChar, StringToInt, StringToDouble, StringToGuid,
                      ToString, Wrap, Box, Unbox, Retype, Upcast, Downcast };
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
    std::vector<Type> typeArgs;  // Call: explicit generic arguments (`Id<int>(5)`); removed by generic expansion
    int depth = 1;               // height of this expression tree (set by the parser; bounded)
    // Lambda: `(int a, b) : int => expr` or `=> { ... }`. Sema lifts the body into a method and turns the node into captures + mkclosure.
    std::vector<Param> lparams;
    std::vector<StmtPtr> lbody;  // an expression body is a single `return` statement (exprBody)
    bool exprBody = false;
    bool hasLret = false;        // a result type was written: `(int x) : int => ...`
    Type lret = Type::Void;
    bool pending = false;        // sema: a lambda whose parameter types come from the context it is passed to
    bool preChecked = false;     // sema already checked this expression (call arguments typed before an overload is chosen)

    // Filled in by semantic analysis.
    Type type = Type::Void;
    int hostIndex = -1;          // Call to a hand declaration
    int methodIndex = -1;        // Call to a method
    int slot = -1;               // Var: local slot
    int classIndex = -1;         // Member/MethodCall on a class: the class (Program::classes); NewObj: the class
    bool isStatic = false;       // Member: a static field; MethodCall: a static method (no receiver value)
};

struct Stmt {
    enum class Kind { ExprStmt, VarDecl, Assign, If, While, For, ForEach, Break, Continue, Return, Block, Try, Throw };
    Kind kind = Kind::ExprStmt;
    bool fieldInit = false;      // Assign: synthesized from a field initializer (may assign readonly/locked fields)
    SourceLoc loc;
    ExprPtr expr;                // ExprStmt call, initializer, assigned value, condition (For: may be null), return value
    ExprPtr target;              // Assign: a Var or Index expression
    std::string name;            // VarDecl
    bool inferred = false;       // VarDecl: declared with 'var'
    Type declType = Type::Invalid;
    StmtPtr init, step;          // For: optional initializer (declaration or assignment) and step (assignment or call)
    std::vector<StmtPtr> body;   // If-then, While, For, Block
    std::vector<StmtPtr> elseBody;
    bool hasElse = false;       // Try: the catch variable is named in `name` (empty: none); body = try block, elseBody = catch block

    int slot = -1;               // VarDecl/ForEach: local slot of the variable (filled by sema)
    int slotArr = -1, slotIdx = -1;  // ForEach: hidden locals holding the array and the index
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
    bool isProtected = false;    // visible in the class and its subclasses
    bool isOverride = false;     // declared with 'override' (must replace an inherited method)
    bool isAbstract = false;     // an interface method: a signature without a body
    bool isInit = false;         // synthesized `$init`: the constructor body of a class that is part of a hierarchy
    bool lifted = false;         // synthesized from a lambda; already checked by sema
    bool isDispatcher = false;   // synthesized: chooses an implementation with isinst tests (emitted by mlgen)
    int initMethod = -1;         // constructors of hierarchy classes: the `$init` method the constructor calls
    std::vector<std::pair<int, int>> cases;  // dispatcher: (class, implementing method), most derived classes first
    int defaultImpl = -1;        // dispatcher: the method used when no case matches (-1: none, interface)
    std::vector<std::string> typeParams;  // `method Id<T>(T x)`: a template (top-level methods only)
    bool hasBaseCall = false;    // constructor written as `constructor(...) : base(args)`
    SourceLoc baseLoc;
    std::vector<std::unique_ptr<Expr>> baseArgs;
    int thisSlot = -1;           // constructors: the local slot that holds the new object (set by sema)
    int unit = 0;                // index into Program::modules (the file it was declared in)
    bool exported = false;       // declared with 'export'
    SourceLoc loc;               // the name
    std::string name;            // overloaded methods get a unique name `Name$$<parameter types>` (see Sema::mangleOverloads)
    std::string srcName;         // the name as written, when `name` was changed because of overloading
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
    bool isProtected = false;
    bool isStatic = false;
    bool isReadonly = false;     // `readonly field`: assigned only by its initializer or, for instance fields, in the class's constructor
    bool isLocked = false;       // `locked field`: a constant, assigned only by its (constant) initializer
    int owner = -1;              // class that declared the field when it was inherited (-1: the class itself)
};

struct PropertyDecl {
    SourceLoc loc;
    std::string name;
    Type type;
    bool isPrivate = false;
    bool isProtected = false;
    int owner = -1;
    bool hasGet = false, hasSet = false;  // the accessors are methods "get_<name>" / "set_<name>" of the class
};

// class <name> { members }   or   model <name> { fields }
struct ClassDecl {
    int unit = 0;                // index into Program::modules
    bool exported = false;
    bool isModel = false;        // a constructor taking every field in order is generated when none is declared
    bool isShared = false;       // `shared class`: every member is shared (static); it cannot be instantiated
    bool isInterface = false;    // `interface I { method m(...): T }`: signatures only
    std::vector<std::string> typeParams;  // `class Box<T>`: a template; instantiated per set of arguments (generics.cpp)
    std::vector<Type> superTypes;         // `class D : Base<int>, I`: the supertypes as written (names are in superNames)
    std::vector<std::string> superNames;  // `class D : Base, I1, I2` (parser output)
    std::vector<SourceLoc> superLocs;
    std::vector<int> supers;     // resolved direct supertypes: the base class first, then interfaces (filled by sema)
    SourceLoc loc;
    std::string name;
    std::vector<FieldDecl> fields;
    std::vector<PropertyDecl> properties;
    std::vector<MethodDecl> methods;  // parser output; the loader moves them into Program::methods
};

// The ML name of a class: its own name in the entry file, prefixed with the file index elsewhere.
inline std::string classMlName(const ClassDecl& c) { return c.unit == 0 ? c.name : "m" + std::to_string(c.unit) + "." + c.name; }

// use "<namespace, library or package>"
struct ImportDecl {
    SourceLoc loc;
    std::string spec;
    bool isUse = false;  // `use "Name"` / `use "./file.lib"` (a library) instead of `import`
};

// One source file of the program. Module 0 is the entry file.
struct Module {
    std::string path;                 // as shown in diagnostics and in the ML '.file' directive
    std::string ns;                   // the namespace the file declares (empty: none)
    std::vector<ImportDecl> imports;
    std::vector<int> importIndex;     // resolved module of each import (filled by the loader)
    std::vector<int> libSiblings;     // other files of the same .lib (they see each other's exports)
    std::string libName;              // non-empty for a file that came out of a .lib
    std::vector<std::string> libRequires;  // libraries that .lib asks to load with it (made visible to this file)
};

struct Program {
    std::string ns;                   // `namespace A.B` at the top of the file (empty: the global namespace)
    SourceLoc nsLoc;
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
