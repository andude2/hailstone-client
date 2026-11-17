// platform::precise_sleep_until: the frame cap's wait (app/frame_pace.h).
#include <doctest/doctest.h>

#include <chrono>

#include "platform/precise_sleep.h"

using namespace std::chrono;
using hailstone::platform::coarse_sleep_for;

TEST_CASE("Precise sleep: the system's part leaves the margin to the spin") {
    CHECK(coarse_sleep_for(milliseconds(5), milliseconds(1)) == milliseconds(4));
    CHECK(coarse_sleep_for(microseconds(800), milliseconds(1)) == nanoseconds(0));
    CHECK(coarse_sleep_for(milliseconds(1), milliseconds(1)) == nanoseconds(0));
    CHECK(coarse_sleep_for(nanoseconds(-5), milliseconds(1)) == nanoseconds(0));
    CHECK(hailstone::platform::precise_sleep_margin() > microseconds(0));
}

TEST_CASE("Precise sleep: never returns before the deadline, and at once for one passed") {
    // Only the lower bound is checked: how late a busy test machine wakes is not this test's business.
    for (auto wait : {microseconds(200), microseconds(1500), microseconds(3000)}) {
        const auto deadline = steady_clock::now() + wait;
        hailstone::platform::precise_sleep_until(deadline);
        CHECK(steady_clock::now() >= deadline);
    }
    const auto before = steady_clock::now();
    hailstone::platform::precise_sleep_until(before - milliseconds(10));
    CHECK(steady_clock::now() - before < milliseconds(50));
}
