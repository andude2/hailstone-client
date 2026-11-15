#pragma once

// Locale-independent text to float/double. std::from_chars for floating point is missing from some
// standard libraries (libc++ before LLVM 20, so Apple's Xcode 15), where this falls back to strtod
// with the same accepted syntax: no leading blank or '+', no hex floats (std::chars_format::general).

#include <cerrno>
#include <cmath>
#include <charconv>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace hailstone {

namespace detail {

// The strtod fallback, compiled everywhere so its tests run on every platform. Like std::from_chars it reports a
// value outside T's range (1e999, or 1e39 for float) as result_out_of_range and leaves `value` as it was.
template <typename T>
std::from_chars_result strtod_from_chars(const char* first, const char* last, T& value) {
    static_assert(std::is_floating_point_v<T>);
    const char* p = first;
    if (p != last && *p == '-') ++p;
    // Reject what strtod accepts and from_chars does not: blanks, '+', and hex.
    if (p == last || !((*p >= '0' && *p <= '9') || *p == '.' || *p == 'i' || *p == 'I' || *p == 'n' || *p == 'N'))
        return {first, std::errc::invalid_argument};
    if (last - p >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        value = (first != p) ? T(-0.0) : T(0);
        return {p + 1, std::errc()};
    }
    std::string text(first, last);
    char* end = nullptr;
    const int saved_errno = errno;
    errno = 0;
    // Parsed at T's own precision, so nothing is narrowed afterwards (a double outside float's range would be
    // undefined to convert).
    T v;
    if constexpr (std::is_same_v<T, float>) v = std::strtof(text.c_str(), &end);
    else if constexpr (std::is_same_v<T, double>) v = std::strtod(text.c_str(), &end);
    else v = std::strtold(text.c_str(), &end);
    // ERANGE also marks a subnormal result, which from_chars returns as a value; overflow (inf) and underflow to zero
    // are out of range.
    const bool out_of_range = errno == ERANGE && (v == T(0) || std::isinf(v));
    errno = saved_errno;
    if (end == text.c_str()) return {first, std::errc::invalid_argument};
    const char* stop = first + (end - text.c_str());
    if (out_of_range) return {stop, std::errc::result_out_of_range};
    value = v;
    return {stop, std::errc()};
}

} // namespace detail

template <typename T>
std::from_chars_result float_from_chars(const char* first, const char* last, T& value) {
    static_assert(std::is_floating_point_v<T>);
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    return std::from_chars(first, last, value, std::chars_format::general);
#else
    return detail::strtod_from_chars(first, last, value);
#endif
}

// The number `text` starts with (as float_from_chars reads it), or nullopt for none, NaN or an infinity: for a
// setting a player types, which std::clamp would not hold (NaN passes it) and which would be saved.
template <typename T> std::optional<T> finite_float_from_text(std::string_view text) {
    T v{};
    if (text.empty() || float_from_chars(text.data(), text.data() + text.size(), v).ec != std::errc{} ||
        !std::isfinite(v))
        return std::nullopt;
    return v;
}

} // namespace hailstone
