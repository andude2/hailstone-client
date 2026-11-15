// mq/calc.h: MacroQuest's Calculate (the arithmetic of ${Math.Calc[...]} and ${If[...]}) and its text helpers.
#include <doctest/doctest.h>

#include <limits>
#include <string>

#include "fuzz_support.h"
#include "mq/calc.h"

using hailstone::mq::calculate;

namespace {

double calc(std::string_view f) {
    const auto v = calculate(f);
    REQUIRE_MESSAGE(v.has_value(), f);
    return *v;
}

} // namespace

TEST_CASE("calc: precedence and grouping are MQ's") {
    CHECK(calc("1+2*3") == 7);
    CHECK(calc("(1+2)*3") == 9);
    CHECK(calc("10/4") == 2.5);
    CHECK(calc("2^10") == 1024);
    // Everything groups from the left, the unary operators too.
    CHECK(calc("2^3^2") == 64);
    CHECK(calc("-2^2") == 4);
    CHECK(calc("!!1") == 0); // the second ! goes out before its operand: !0, then 1, then !1
    CHECK(calc("1+2==3") == 1);
    CHECK(calc("1<2&&3>2") == 1);
    CHECK(calc("0||0") == 0);
    CHECK(calc("1|2&3") == 3); // & before |
}

TEST_CASE("calc: operators") {
    CHECK(calc("7\\2") == 3);
    CHECK(calc("-7\\2") == -3);
    CHECK(calc("7%3") == 1);
    CHECK(calc("-7%3") == -1);
    CHECK(calc("6&3") == 2);
    CHECK(calc("6|3") == 7);
    CHECK(calc("6^^3") == 5);
    CHECK(calc("1<<4") == 16);
    CHECK(calc("256>>4") == 16);
    CHECK(calc("1<<33") == 2); // the count modulo 32, as x86
    CHECK(calc("-16>>2") == -4);
    CHECK(calc("!0") == 1);
    CHECK(calc("!5") == 0);
    CHECK(calc("~0") == -1);
    CHECK(calc("3<=3") == 1);
    CHECK(calc("3>=4") == 0);
    CHECK(calc("2!=2") == 0);
    CHECK(calc("2==2.0") == 1);
}

TEST_CASE("calc: signs, spaces and numbers as MQ reads them") {
    CHECK(calc("3--2") == 5); // "--" is an addition
    CHECK(calc("3++2") == 5);
    CHECK(calc("3 - -2") == 5);
    CHECK(calc("2*-3") == -6);
    CHECK(calc("(4) -1") == 3); // after ')' a '-' subtracts
    CHECK(calc("1 2") == 12);   // spaces are skipped, digits join
    CHECK(calc("1.2.3") == doctest::Approx(1.2));
    CHECK(calc(".") == 0);
    CHECK(calc(".5") == 0.5);
}

TEST_CASE("calc: NULL, TRUE and FALSE in any case") {
    CHECK(calc("TRUE") == 1);
    CHECK(calc("true&&false") == 0);
    CHECK(calc("Null+1") == 1);
    CHECK(calc("FALSE||TRUE") == 1);
}

TEST_CASE("calc: errors and MQ's lenient cases") {
    CHECK_FALSE(calculate(""));
    CHECK_FALSE(calculate("abc"));
    CHECK_FALSE(calculate("1,2"));
    CHECK_FALSE(calculate("1=1"));
    CHECK_FALSE(calculate("1/0"));
    CHECK_FALSE(calculate("1\\0"));
    CHECK_FALSE(calculate("5%0"));
    CHECK_FALSE(calculate("1+2)"));
    CHECK_FALSE(calculate("+")); // an operator with no operand at all
    // An unclosed '(' is ignored, a missing left operand is 0.
    CHECK(calc("(1+2") == 3);
    CHECK(calc("5+") == 5);
    CHECK(calc("*5") == 0);
    // Numbers side by side: the last one (MQ overruns its stack here).
    CHECK(calc("(1)(2)(3)(4)(5)") == 5);
}

TEST_CASE("calc: whole-number operators on values outside 32 bits, and the divide fault") {
    constexpr double kMin = std::numeric_limits<std::int32_t>::min();
    CHECK(calc("3000000000|0") == kMin); // x86's conversion of a value past int
    CHECK(calc("-2147483648\\-1") == kMin);
    CHECK(calc("-2147483648%-1") == 0);
    CHECK(calc("2^31\\1") == kMin);
}

TEST_CASE("calc: the formula's length and a NUL byte") {
    CHECK(calculate(std::string(2047, '1')));
    CHECK_FALSE(calculate(std::string(2048, '1')));
    CHECK(calc(std::string("1+2\0junk", 8)) == 3);
    // Deep nesting and long operator runs stay within the vectors.
    CHECK(calc(std::string(1000, '(') + "1" + std::string(1000, ')')) == 1);
    // 500 "--" additions, the first with nothing under it: MQ's "Illegal arithmetic".
    CHECK_FALSE(calculate(std::string(1000, '-') + "1"));
    CHECK(calc(std::string(999, '~') + "1") == -2); // each ~ but the last goes out before the 1
}

TEST_CASE("calc: MQ's number-from-text helpers read a leading number") {
    using namespace hailstone::mq;
    CHECK(int_from_text("12abc", 0) == 12);
    CHECK(int_from_text(" 5 ", 0) == 5);
    CHECK(int_from_text("abc", 7) == 7);
    CHECK(int_from_text("99999999999", 3) == 3);
    CHECK(int_from_text("+5", 1) == 1); // no '+', as std::from_chars
    CHECK(int64_from_text("99999999999", 0) == 99999999999);
    CHECK(double_from_text("2.5x", 0) == 2.5);
    CHECK(double_from_text("", 4) == 4);
}

TEST_CASE("calc: GetArg's arguments") {
    using hailstone::mq::get_arg;
    CHECK(get_arg("a b  c", 1) == "a");
    CHECK(get_arg("a b  c", 2) == "b");
    CHECK(get_arg("a b  c", 3) == "c");
    CHECK(get_arg("a b  c", 4) == "");
    CHECK(get_arg("a\tb", 2) == "b");
    CHECK(get_arg("  x", 0) == "x"); // below 1 is the first
    CHECK(get_arg("\"a b\" c", 1) == "a b");
    CHECK(get_arg("\"a b\" c", 2) == "c");
    CHECK(get_arg("x \"ab", 2) == "ab"); // an unclosed quote runs to the end
    CHECK(get_arg("\"ab c", 2) == "");   // and swallows what follows
    CHECK(get_arg("a,,b", 2, false, ',') == "b");
    CHECK(get_arg("a b,c", 2, false, ',') == "c");
    CHECK(get_arg("1, 2,3", 2, true) == "2");
    CHECK(get_arg("1, 2,3", 3, true) == "3");
    // An argument far past the text's end is empty at once, not after 2^31 passes over nothing.
    CHECK(get_arg("a b", 2147483647) == "");
    CHECK(get_arg("a b", 2147483647, true, ',') == "");
}

TEST_CASE("calc: mutated formulas and arguments never crash") {
    // Text from scripts and other players' names reaches these through ${...}; run with -DHAILSTONE_SANITIZE=ON.
    hailstone::fuzz::Rng rng(0x6d71ca1c);
    const char* seeds[] = {"(1+2)*3-4/5\\6%7", "TRUE&&FALSE||NULL", "1<<2>>3^4^^5&6|7", "!~-(((2)))", "\"a b\" c,d"};
    for (const char* seed : seeds) {
        const std::string s = seed;
        const hailstone::fuzz::Bytes bytes(reinterpret_cast<const std::byte*>(s.data()),
                                           reinterpret_cast<const std::byte*>(s.data() + s.size()));
        for (int i = 0; i < 2000; ++i) {
            const auto m = hailstone::fuzz::mutate(bytes, rng);
            const std::string text(reinterpret_cast<const char*>(m.data()), m.size());
            (void)calculate(text);
            (void)hailstone::mq::get_arg(text, static_cast<int>(i % 5), (i & 1) != 0, (i & 2) ? ',' : '\0');
        }
    }
}
