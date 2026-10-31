#pragma once

#include <string>

#include "host.hpp"
#include "ml.hpp"

namespace pvm {

// Verifies a loaded module before execution and resolves host/entry references in place.
// Throws MlError ("verification error") on the first problem. See docs/ml-spec.md for the rules.
void verify(Module& module, const HostRegistry& registry, const std::string& file);

} // namespace pvm
