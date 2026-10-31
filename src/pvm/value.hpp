#pragma once

#include <memory>
#include <string>

#include "ml.hpp"

namespace pvm {

struct Obj;  // managed heap object (see heap.hpp)

// Runtime value. Scalars and strings are held by value; arrays, maps and class instances are references to heap
// objects that the garbage collector owns. `kind` is the kind of the value itself (never Nullable):
// a value of a nullable type T? has T's kind plus `isNull`. A null `ref` is a default-initialized
// array or map slot. A value of type obj has kind Obj and holds the boxed value in `boxed` (or is null).
struct Value {
    Type::Kind kind = Type::Void;
    long long i = 0;   // Int value, Char code point, or Bool as 0/1
    double d = 0;      // Double value
    std::string s;     // String value, or the canonical text of a Guid
    Obj* ref = nullptr;                    // Array or Map
    bool isNull = false;                   // null of a nullable or obj type
    std::shared_ptr<const Value> boxed;    // Obj: the boxed value

    static Value ofInt(long long v) { Value x; x.kind = Type::Int; x.i = v; return x; }
    static Value ofBool(bool v) { Value x; x.kind = Type::Bool; x.i = v ? 1 : 0; return x; }
    static Value ofChar(long long cp) { Value x; x.kind = Type::Char; x.i = cp; return x; }
    static Value ofDouble(double v) { Value x; x.kind = Type::Double; x.d = v; return x; }
    static Value ofString(std::string v) { Value x; x.kind = Type::String; x.s = std::move(v); return x; }
    static Value ofGuid(std::string v) { Value x; x.kind = Type::Guid; x.s = std::move(v); return x; }
    static Value ofArray(Obj* o) { Value x; x.kind = Type::Array; x.ref = o; return x; }
    static Value ofMap(Obj* o) { Value x; x.kind = Type::Map; x.ref = o; return x; }
    static Value ofClosure(Obj* o) { Value x; x.kind = Type::Fn; x.ref = o; return x; }
    static Value ofObject(Obj* o) { Value x; x.kind = Type::Class; x.ref = o; return x; }
    static Value nullObj() { Value x; x.kind = Type::Obj; x.isNull = true; return x; }
    static Value none() { return Value{}; }

    static Value defaultOf(const Type& t) {
        switch (t.kind) {
            case Type::Int: return ofInt(0);
            case Type::Bool: return ofBool(false);
            case Type::Char: return ofChar(0);
            case Type::Double: return ofDouble(0.0);
            case Type::String: return ofString("");
            case Type::Guid: return ofGuid("00000000-0000-0000-0000-000000000000");
            case Type::Array: return ofArray(nullptr);
            case Type::Map: return ofMap(nullptr);
            case Type::Class: return ofObject(nullptr);
            case Type::Fn: return ofClosure(nullptr);
            case Type::Obj: return nullObj();
            case Type::Nullable: {
                Value v = defaultOf(*t.elem);
                v.isNull = true;
                return v;
            }
            case Type::Void: break;
        }
        return none();
    }
};

} // namespace pvm
