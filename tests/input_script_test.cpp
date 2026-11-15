// app/input_script.h: the numbers a test script gives, kept finite before they reach a cast.
#include <doctest/doctest.h>

#include "app/input_script.h"

using hailstone::app::script_number;

TEST_CASE("Input script: a number that is not finite takes the fallback") {
    CHECK(script_number("0.5") == doctest::Approx(0.5));
    CHECK(script_number("-1") == doctest::Approx(-1.0));
    CHECK(script_number("nan") == 0.0);
    CHECK(script_number("-inf", 0.25) == 0.25);
    CHECK(script_number("1e999", 0.08) == 0.08);
    CHECK(script_number("", 3.0) == 0.0); // no number is 0, as atof read it
}
