#pragma once

#include <string>
#include <utility>
#include <vector>

namespace coyote::pkg {

// A parsed JSON value. Every value remembers where it started so validation errors can point at
// the offending line and column of the file.
struct Json {
    enum Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Null;
    bool boolean = false;
    double number = 0;
    bool isInteger = false;  // Number written without fraction or exponent, within +/-2^53
    std::string string;
    std::vector<Json> items;                              // Array
    std::vector<std::pair<std::string, Json>> members;    // Object, in file order (keys are unique)
    int line = 1, col = 1;

    const Json* find(const std::string& key) const {
        for (const auto& m : members)
            if (m.first == key) return &m.second;
        return nullptr;
    }
};

struct JsonError {
    int line, col;
    std::string message;
};

// Strict JSON (RFC 8259): no comments, no trailing commas, no duplicate object keys, valid UTF-8
// escapes, nesting up to 64 levels. Throws JsonError.
Json parseJson(const std::string& text);

} // namespace coyote::pkg
