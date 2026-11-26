// Checked 64-bit arithmetic that also builds with MSVC (which has no __builtin_*_overflow).
// Each function returns true on overflow (like the GCC/Clang builtins) and stores the wrapped result in *r otherwise.
#pragma once
#include <climits>

namespace pvm {

inline bool addOverflow(long long a, long long b, long long* r) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_add_overflow(a, b, r);
#else
    if ((b > 0 && a > LLONG_MAX - b) || (b < 0 && a < LLONG_MIN - b)) return true;
    *r = a + b;
    return false;
#endif
}

inline bool subOverflow(long long a, long long b, long long* r) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_sub_overflow(a, b, r);
#else
    if ((b < 0 && a > LLONG_MAX + b) || (b > 0 && a < LLONG_MIN + b)) return true;
    *r = a - b;
    return false;
#endif
}

inline bool mulOverflow(long long a, long long b, long long* r) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_mul_overflow(a, b, r);
#else
    if (a > 0) {
        if (b > 0) { if (a > LLONG_MAX / b) return true; }
        else if (b < LLONG_MIN / a) return true;
    } else if (a < 0) {
        if (b > 0) { if (a < LLONG_MIN / b) return true; }
        else if (b < LLONG_MAX / a) return true;
    }
    *r = a * b;
    return false;
#endif
}

}  // namespace pvm
