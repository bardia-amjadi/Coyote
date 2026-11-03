#pragma once

#include <vector>

#include "ast.hpp"

namespace coyote {

// Generics are expanded before semantic analysis (monomorphization): every use of `Box<int>` or
// `Id<int>(x)` gets its own copy of the generic class or method with the type parameters replaced, named
// `Box$int$`. The generic declarations themselves are removed afterwards, so later passes and the ML
// output never see a type parameter. Appends diagnostics for misuse (wrong argument count, ...).
void expandGenerics(Program& program, std::vector<Diagnostic>& diagnostics);

} // namespace coyote
