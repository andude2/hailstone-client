#pragma once

// A number read from a file, converted to an integer type without undefined behaviour: casting a floating-point
// value outside the target's range (or NaN) is undefined in C++, so asset text such as "1e30" or "-5" for an
// unsigned field must be clamped first. In-range values convert exactly as static_cast does (toward zero).

#include <cmath>
#include <concepts>
#include <limits>

namespace hailstone {

// `v` truncated toward zero and clamped to I's range; NaN is 0.
template <std::integral I, std::floating_point F> I saturate_cast(F v) {
    if (std::isnan(v)) return 0;
    // Where long double is double (MSVC) a 64-bit maximum rounds up to 2^63, which >= still treats as out of range.
    constexpr long double lo = static_cast<long double>(std::numeric_limits<I>::min());
    constexpr long double hi = static_cast<long double>(std::numeric_limits<I>::max());
    const long double t = std::trunc(static_cast<long double>(v));
    if (t <= lo) return std::numeric_limits<I>::min();
    if (t >= hi) return std::numeric_limits<I>::max();
    return static_cast<I>(t);
}

// a + b for unsigned I, held at I's maximum instead of wrapping (a server's count added to a total).
template <std::unsigned_integral I> constexpr I saturating_add(I a, I b) {
    return a > std::numeric_limits<I>::max() - b ? std::numeric_limits<I>::max() : static_cast<I>(a + b);
}

} // namespace hailstone
