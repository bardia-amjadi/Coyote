// Runtime services of the PVM that do not need the heap or the operand stack: number formatting,
// parsing, equality, and the dynamic (dyn) operations. Everything here throws OpError on failure;
// the interpreter turns that into a runtime error with the source line.
#pragma once

#include <string>

#include "value.hpp"
#include "heap.hpp"
#include "ml.hpp"

namespace pvm {

struct OpError {
    std::string message;
};

std::string formatDouble(double v);
void appendUtf8(std::string& out, long long codePoint);
bool parseIntStrict(const std::string& s, long long& out);       // optional '-'/'+', digits only, fits in 32 bits
bool parseDoubleStrict(const std::string& s, double& out);       // decimal or exponent form; no spaces, no hex
bool parseGuidStrict(const std::string& s, std::string& canonical);  // 8-4-4-4-12 hex, any case; canonical is lower case

// Canonical text of a map key (keys are int, string, char, boolean or Guid).
std::string mapKeyText(const Value& key);

// Human-readable text of any value; null is "null". Used by string(obj) and by dyn concatenation.
std::string display(const Value& v);

// Name of the runtime type of a value (for error messages).
std::string runtimeTypeName(const Value& v);

// Does the boxed/plain value `v` have exactly type `t` (t not nullable)?
bool hasType(const Value& v, const Type& t);

// A value of type obj holding `inner` (null obj if `inner` is a null value or a null reference).
Value boxValue(const Value& inner);

// Converts an obj to type `t` (obj -> T). Null converts only to nullable types and obj. An int
// converts to double; every other conversion requires the exact type. Throws OpError.
Value unboxTo(const Value& obj, const Type& t);

// obj equality (eq.obj): same runtime type and equal value; references compare by identity.
bool objEquals(const Value& a, const Value& b);

// Dynamic operators. Operands and results are obj values.
Value dynBinary(Op op, const Value& a, const Value& b);
Value dynUnary(Op op, const Value& a);

} // namespace pvm
