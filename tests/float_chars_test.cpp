#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <string_view>

#include "core/float_chars.h"

using hailstone::detail::strtod_from_chars;

namespace {
template <typename T> std::from_chars_result parse(std::string_view s, T& v) {
    return strtod_from_chars(s.data(), s.data() + s.size(), v);
}
} // namespace

TEST_CASE("float_from_chars fallback: ordinary numbers and the syntax from_chars refuses") {
    double d = 0;
    auto r = parse("1.5x", d);
    CHECK(r.ec == std::errc());
    CHECK(d == 1.5);
    CHECK(*r.ptr == 'x');
    CHECK(parse("-2e3", d).ec == std::errc());
    CHECK(d == -2000.0);
    CHECK(parse("+1", d).ec == std::errc::invalid_argument);
    CHECK(parse(" 1", d).ec == std::errc::invalid_argument);
    float f = 0;
    CHECK(parse("0.25", f).ec == std::errc());
    CHECK(f == 0.25f);
}

TEST_CASE("float_from_chars fallback: a value outside the type's range is result_out_of_range, as from_chars") {
    double d = 7.0;
    std::string_view big = "1e999";
    auto r = parse(big, d);
    CHECK(r.ec == std::errc::result_out_of_range);
    CHECK(r.ptr == big.data() + big.size());
    CHECK(d == 7.0); // untouched
    CHECK(parse("-1e999", d).ec == std::errc::result_out_of_range);
    CHECK(parse("1e-999", d).ec == std::errc::result_out_of_range);
    CHECK(d == 7.0);

    // Beyond float's range but within double's: parsed as a float, never narrowed from a double.
    float f = 3.0f;
    CHECK(parse("1e39", f).ec == std::errc::result_out_of_range);
    CHECK(parse("-1e39", f).ec == std::errc::result_out_of_range);
    CHECK(f == 3.0f);
    CHECK(parse("3e38", f).ec == std::errc());
    CHECK(f == doctest::Approx(3e38f));

    // A subnormal is a value, not an error.
    CHECK(parse("1e-310", d).ec == std::errc());
    CHECK(d > 0.0);
    CHECK(d < std::numeric_limits<double>::min());

    // "inf" written out is a value.
    CHECK(parse("inf", d).ec == std::errc());
    CHECK(std::isinf(d));
}

TEST_CASE("float_from_chars fallback agrees with std::from_chars where both exist") {
    for (std::string_view s : {"0", "-0", "1.25", "1e999", "-1e999", "1e-999", "12abc", "abc", "", "-", ".5", "5.",
                               "1e39", "nan", "inf", "0x1p3"}) {
        CAPTURE(s);
        double a = 1.0, b = 1.0;
        auto ra = strtod_from_chars(s.data(), s.data() + s.size(), a);
        auto rb = hailstone::float_from_chars(s.data(), s.data() + s.size(), b);
        CHECK(ra.ec == rb.ec);
        CHECK(ra.ptr == rb.ptr);
        if (ra.ec == std::errc() && !std::isnan(a)) CHECK(a == b);
    }
}

TEST_CASE("finite_float_from_text: a typed setting is a finite number or none") {
    using hailstone::finite_float_from_text;
    CHECK(finite_float_from_text<float>("45") == 45.0f);
    CHECK(finite_float_from_text<float>("-2.5") == -2.5f);
    CHECK(finite_float_from_text<float>("30deg") == 30.0f); // the number it starts with, as before
    CHECK_FALSE(finite_float_from_text<float>(""));
    CHECK_FALSE(finite_float_from_text<float>("abc"));
    CHECK_FALSE(finite_float_from_text<float>("nan"));
    CHECK_FALSE(finite_float_from_text<float>("-NaN"));
    CHECK_FALSE(finite_float_from_text<float>("inf"));
    CHECK_FALSE(finite_float_from_text<float>("-infinity"));
    CHECK_FALSE(finite_float_from_text<float>("1e39")); // past float's range
}
