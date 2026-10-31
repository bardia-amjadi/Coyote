// In-memory representation of an ML (Middle Language) module, as defined in docs/ml-spec.md.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pvm {

// A runtime/ML type. Arrays and nullable types carry an element type; maps also carry a key type.
struct Type {
    enum Kind { Void, Int, String, Bool, Array, Double, Char, Guid, Map, Nullable, Obj, Class, Fn };

    Kind kind = Void;
    std::shared_ptr<const Type> elem;  // Array: element; Map: value; Nullable: the wrapped type
    std::shared_ptr<const Type> key;   // Map only
    std::string name;                  // Class only: the class name
    std::shared_ptr<const std::vector<Type>> params;  // Fn only: parameter types (the return type is `elem`)

    Type() = default;
    Type(Kind k) : kind(k) {}  // implicit, so `Type::Int` works wherever a Type is expected

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
    static Type fnOf(std::vector<Type> ps, const Type& ret) {
        Type t(Fn);
        t.params = std::make_shared<const std::vector<Type>>(std::move(ps));
        t.elem = std::make_shared<const Type>(ret);
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
    // Types whose values are references to heap objects.
    bool isRef() const { return kind == Array || kind == Map || kind == Class || kind == Fn; }

    friend bool operator==(const Type& a, const Type& b) {
        if (a.kind != b.kind) return false;
        switch (a.kind) {
            case Array: case Nullable: return *a.elem == *b.elem;
            case Map: return *a.key == *b.key && *a.elem == *b.elem;
            case Class: return a.name == b.name;
            case Fn: return *a.elem == *b.elem && *a.params == *b.params;
            default: return true;
        }
    }
    friend bool operator!=(const Type& a, const Type& b) { return !(a == b); }
};

inline std::string typeName(const Type& t) {
    switch (t.kind) {
        case Type::Void: return "void";
        case Type::Int: return "int";
        case Type::String: return "string";
        case Type::Bool: return "boolean";
        case Type::Double: return "double";
        case Type::Char: return "char";
        case Type::Guid: return "Guid";
        case Type::Obj: return "obj";
        case Type::Array: return "array<" + typeName(*t.elem) + ">";
        case Type::Map: return "map<" + typeName(*t.key) + "," + typeName(*t.elem) + ">";
        case Type::Nullable: return typeName(*t.elem) + "?";
        case Type::Class: return t.name;
        case Type::Fn: {
            std::string r = "fn(";
            for (size_t i = 0; i < t.params->size(); ++i) r += (i ? "," : "") + typeName((*t.params)[i]);
            return r + ")->" + typeName(*t.elem);
        }
    }
    return "?";
}

struct Sig {
    std::vector<Type> params;
    Type ret = Type::Void;
};

inline std::string sigToString(const Sig& s) {
    std::string r = "(";
    for (size_t i = 0; i < s.params.size(); ++i) {
        if (i) r += ", ";
        r += typeName(s.params[i]);
    }
    return r + ") -> " + typeName(s.ret);
}

inline bool sameSig(const Sig& a, const Sig& b) { return a.params == b.params && a.ret == b.ret; }

struct HostDecl {
    std::string name;
    Sig sig;
    int mlLine = 0;
};

enum class Op {
    PushStr, PushInt, PushBool, Load, Store,
    AddI, SubI, MulI, DivI, RemI, NegI,
    Concat, StrInt, StrBool,
    EqI, NeI, LtI, LeI, GtI, GeI, EqS, NeS, EqB, NeB, Not,
    Jump, JumpFalse, Call, HostCall, Pop, Ret,
    NewArray, ArrayLit, ALoad, AStore, ALen, APush, ACopy, EqRef, NeRef,
    // version 4: double, char, Guid
    PushDbl, PushChr, AddD, SubD, MulD, DivD, RemD, NegD,
    EqD, NeD, LtD, LeD, GtD, GeD, EqC, NeC, LtC, LeC, GtC, GeC, EqG, NeG,
    I2D, D2I, C2I, I2C, StrDbl, StrChr, StrGuid, ParseInt, ParseDbl, ParseGuid,
    // version 4: nullable types
    PushNull, Wrap, Unwrap, IsNull, Dup,
    // version 4: maps
    NewMap, MapGet, MapSet, MapHas, MapDel, MapLen, MapKeys, MapValues, MapTry,
    // version 4: obj and dynamic operations
    Box, Unbox, IsType, ToStrObj, EqObj, NeObj,
    DynAdd, DynSub, DynMul, DynDiv, DynRem, DynNeg, DynNot,
    DynEq, DynNe, DynLt, DynLe, DynGt, DynGe, DynIndex, DynSetIndex, DynMember, DynInvoke,
    // version 5: classes and globals
    NewObj, GetField, SetField, GetGlobal, SetGlobal,
    // version 6: function values
    MkClosure, CallFn
};

// Instruction names and the ML version that introduced each.
struct OpInfo {
    Op op;
    const char* name;
    int since;
};

inline const std::vector<OpInfo>& opTable() {
    static const std::vector<OpInfo> t = {
        {Op::PushStr, "push.str", 1},   {Op::PushInt, "push.int", 1},   {Op::HostCall, "hostcall", 1},
        {Op::Pop, "pop", 1},            {Op::Ret, "ret", 1},
        {Op::PushBool, "push.bool", 2}, {Op::Load, "load", 2},          {Op::Store, "store", 2},
        {Op::AddI, "add.int", 2},       {Op::SubI, "sub.int", 2},       {Op::MulI, "mul.int", 2},
        {Op::DivI, "div.int", 2},       {Op::RemI, "rem.int", 2},       {Op::NegI, "neg.int", 2},
        {Op::Concat, "concat", 2},      {Op::StrInt, "str.int", 2},     {Op::StrBool, "str.bool", 2},
        {Op::EqI, "eq.int", 2},         {Op::NeI, "ne.int", 2},         {Op::LtI, "lt.int", 2},
        {Op::LeI, "le.int", 2},         {Op::GtI, "gt.int", 2},         {Op::GeI, "ge.int", 2},
        {Op::EqS, "eq.str", 2},         {Op::NeS, "ne.str", 2},         {Op::EqB, "eq.bool", 2},
        {Op::NeB, "ne.bool", 2},        {Op::Not, "not", 2},            {Op::Jump, "jump", 2},
        {Op::JumpFalse, "jump.false", 2}, {Op::Call, "call", 2},
        {Op::NewArray, "newarray", 3},  {Op::ArrayLit, "arraylit", 3},  {Op::ALoad, "aload", 3},
        {Op::AStore, "astore", 3},      {Op::ALen, "alen", 3},          {Op::APush, "apush", 3},
        {Op::ACopy, "acopy", 3},        {Op::EqRef, "eq.ref", 3},       {Op::NeRef, "ne.ref", 3},
        {Op::PushDbl, "push.dbl", 4},   {Op::PushChr, "push.chr", 4},
        {Op::AddD, "add.dbl", 4},       {Op::SubD, "sub.dbl", 4},       {Op::MulD, "mul.dbl", 4},
        {Op::DivD, "div.dbl", 4},       {Op::RemD, "rem.dbl", 4},       {Op::NegD, "neg.dbl", 4},
        {Op::EqD, "eq.dbl", 4},         {Op::NeD, "ne.dbl", 4},         {Op::LtD, "lt.dbl", 4},
        {Op::LeD, "le.dbl", 4},         {Op::GtD, "gt.dbl", 4},         {Op::GeD, "ge.dbl", 4},
        {Op::EqC, "eq.chr", 4},         {Op::NeC, "ne.chr", 4},         {Op::LtC, "lt.chr", 4},
        {Op::LeC, "le.chr", 4},         {Op::GtC, "gt.chr", 4},         {Op::GeC, "ge.chr", 4},
        {Op::EqG, "eq.guid", 4},        {Op::NeG, "ne.guid", 4},
        {Op::I2D, "i2d", 4},            {Op::D2I, "d2i", 4},            {Op::C2I, "c2i", 4},
        {Op::I2C, "i2c", 4},            {Op::StrDbl, "str.dbl", 4},     {Op::StrChr, "str.chr", 4},
        {Op::StrGuid, "str.guid", 4},   {Op::ParseInt, "parse.int", 4}, {Op::ParseDbl, "parse.dbl", 4},
        {Op::ParseGuid, "parse.guid", 4},
        {Op::PushNull, "push.null", 4}, {Op::Wrap, "wrap", 4},          {Op::Unwrap, "unwrap", 4},
        {Op::IsNull, "isnull", 4},      {Op::Dup, "dup", 4},
        {Op::NewMap, "newmap", 4},      {Op::MapGet, "mapget", 4},      {Op::MapSet, "mapset", 4},
        {Op::MapHas, "maphas", 4},      {Op::MapDel, "mapdel", 4},      {Op::MapLen, "maplen", 4},
        {Op::MapKeys, "mapkeys", 4},    {Op::MapValues, "mapvalues", 4}, {Op::MapTry, "maptry", 4},
        {Op::Box, "box", 4},            {Op::Unbox, "unbox", 4},        {Op::IsType, "istype", 4},
        {Op::ToStrObj, "tostr.obj", 4}, {Op::EqObj, "eq.obj", 4},       {Op::NeObj, "ne.obj", 4},
        {Op::DynAdd, "dyn.add", 4},     {Op::DynSub, "dyn.sub", 4},     {Op::DynMul, "dyn.mul", 4},
        {Op::DynDiv, "dyn.div", 4},     {Op::DynRem, "dyn.rem", 4},     {Op::DynNeg, "dyn.neg", 4},
        {Op::DynNot, "dyn.not", 4},     {Op::DynEq, "dyn.eq", 4},       {Op::DynNe, "dyn.ne", 4},
        {Op::DynLt, "dyn.lt", 4},       {Op::DynLe, "dyn.le", 4},       {Op::DynGt, "dyn.gt", 4},
        {Op::DynGe, "dyn.ge", 4},       {Op::DynIndex, "dyn.index", 4}, {Op::DynSetIndex, "dyn.setindex", 4},
        {Op::DynMember, "dyn.member", 4}, {Op::DynInvoke, "dyn.invoke", 4},
        {Op::NewObj, "newobj", 5},      {Op::GetField, "getfield", 5},  {Op::SetField, "setfield", 5},
        {Op::GetGlobal, "getglobal", 5}, {Op::SetGlobal, "setglobal", 5},
        {Op::MkClosure, "mkclosure", 6}, {Op::CallFn, "callfn", 6},
    };
    return t;
}

inline const char* opName(Op op) {
    for (const OpInfo& i : opTable())
        if (i.op == op) return i.name;
    return "?";
}

struct Instr {
    Op op = Op::Ret;
    int mlLine = 0;      // line in the ML file (for loader/verifier errors)
    int srcLine = 0;     // line in the original .coy file from the preceding '.line' (0 = unknown)
    int srcFile = 0;     // index into Module::files of the source file ('.file'); 0 = the 'source' file
    std::string str;     // PushStr text; HostCall host name; Call function name; Jump label name
    double dval = 0;     // PushDbl value
    long long ival = 0;  // PushInt value, PushBool 0/1, Load/Store slot, HostCall/Call argument count, ArrayLit length
    Type type;           // NewArray/ArrayLit: the element type; NewMap: the map type; PushNull/Unbox/IsType: the type
    std::shared_ptr<const Type> objType;  // NewArray/ArrayLit: array<elem>; NewMap: the map type (set by the loader)
    int host = -1;       // HostCall: index into Module::hosts (resolved by the verifier)
    int cls = -1;        // NewObj/GetField/SetField: class index (resolved by the verifier); field index is in ival
    int target = -1;     // Call: function index; GetGlobal/SetGlobal: global index; Jump/JumpFalse: instruction index (resolved by the verifier)
};

struct Function {
    std::string name;
    Sig sig;
    int mlLine = 0;
    std::vector<Type> locals;                          // declared locals (slots after the parameters)
    std::map<int, std::string> varNames;               // debug information from ".var": slot -> source name
    std::map<std::string, std::pair<int, int>> labels; // name -> (instruction index, ML line)
    std::vector<Instr> code;
};

struct FieldDecl {
    std::string name;
    Type type;
};

struct ClassDecl {
    std::string name;
    int mlLine = 0;
    std::vector<FieldDecl> fields;
};

struct GlobalDecl {
    std::string name;
    Type type;
    int mlLine = 0;
};

struct Module {
    int version = 0;
    std::string source;   // original .coy path from the 'source' directive
    bool hasSource = false;
    std::vector<std::string> files;  // [0] = source; more from '.file' directives (version 5)
    const std::string& fileName(int i) const { return i > 0 && static_cast<size_t>(i) < files.size() ? files[static_cast<size_t>(i)] : source; }
    std::vector<HostDecl> hosts;
    std::vector<ClassDecl> classes;   // version 5
    std::vector<GlobalDecl> globals;  // version 5
    std::vector<Function> funcs;
    std::string entry;
    int entryLine = 0;
    int entryFunc = -1;   // resolved by the verifier
};

// Error in an ML file (syntax, structure or verification). Rendered as
// "<file>:<line>: <kind>: <message>".
struct MlError {
    std::string kind;  // "ml error" or "verification error"
    std::string file;
    int line = 0;
    std::string message;
};

inline std::string formatMlError(const MlError& e) {
    return e.file + ":" + std::to_string(e.line) + ": " + e.kind + ": " + e.message;
}

} // namespace pvm
