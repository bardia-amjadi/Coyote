#pragma once

#include <string>

#include "ml.hpp"

namespace pvm {

// Parses ML text into a Module. Throws MlError ("ml error") on malformed text, unsupported
// versions, unknown instructions and unterminated functions. Semantic checks happen in verify().
Module loadMl(const std::string& text, const std::string& file);

} // namespace pvm
