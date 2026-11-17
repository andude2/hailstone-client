// The main loop's frame cap and drawn frames (app/frame_pace.h): display.max_fps, and display.background_fps while
// another window has the focus, plus the 240 cap on login/loading screens when max_fps is unlimited. The loop itself
// (app/client_app.cpp) is not driven here.

#include <doctest/doctest.h>

#include <chrono>

#include "app/frame_pace.h"

using namespace hailstone;
using std::chrono::milliseconds;

TEST_CASE("Frame pace: focused, or a background rate of 0, keeps the frame limit and draws every frame") {
    for (int bg : {0, app::kBackgroundNoDraw, 1, 5, 30}) {
        auto p = app::frame_pace(144, bg, true, milliseconds(0));
        CHECK(p.cap == 144);
        CHECK(p.draw);
    }
    auto p = app::frame_pace(0, 0, false, milliseconds(0));
    CHECK(p.cap == 0);
    CHECK(p.draw);
}

TEST_CASE("Frame pace: a background rate of 10 or more caps the loop and draws every frame") {
    CHECK(app::frame_pace(0, 30, false, milliseconds(0)).cap == 30);
    CHECK(app::frame_pace(144, 60, false, milliseconds(0)).cap == 60);
    CHECK(app::frame_pace(30, 60, false, milliseconds(0)).cap == 30); // the lower of the two
    CHECK(app::frame_pace(0, 10, false, milliseconds(0)).draw);
}

TEST_CASE("Frame pace: below 10 in the background the loop runs at 10 and draws that many frames a second") {
    for (int bg : {1, 5, app::kBackgroundNoDraw}) CHECK(app::frame_pace(0, bg, false, milliseconds(0)).cap == 10);
    // 1: once a second, on the loop's tenth tick (not the eleventh after a late wake).
    CHECK_FALSE(app::frame_pace(0, 1, false, milliseconds(900)).draw);
    CHECK(app::frame_pace(0, 1, false, milliseconds(1000)).draw);
    CHECK(app::frame_pace(0, 1, false, milliseconds(990)).draw);
    // 5: every second tick.
    CHECK_FALSE(app::frame_pace(0, 5, false, milliseconds(100)).draw);
    CHECK(app::frame_pace(0, 5, false, milliseconds(200)).draw);
    // 0 (kBackgroundNoDraw): never.
    CHECK_FALSE(app::frame_pace(0, app::kBackgroundNoDraw, false, milliseconds(60000)).draw);
}

TEST_CASE("Frame pace: display.background_fps as it is kept") {
    CHECK(app::background_fps_setting(0) == 0);
    CHECK(app::background_fps_setting(-1) == app::kBackgroundNoDraw);
    CHECK(app::background_fps_setting(-50) == app::kBackgroundNoDraw);
    CHECK(app::background_fps_setting(1) == 1);
    CHECK(app::background_fps_setting(5) == 5);
    CHECK(app::background_fps_setting(5000) == 1000);
}

TEST_CASE("Frame pace: an unlimited cap on a light screen counts as 240") {
    auto p = app::frame_pace(0, 0, true, milliseconds(0), true);
    CHECK(p.cap == 240);
    CHECK(p.draw);
}

TEST_CASE("Frame pace: a set max_fps on a light screen is followed exactly") {
    CHECK(app::frame_pace(60, 0, true, milliseconds(0), true).cap == 60);
    CHECK(app::frame_pace(144, 0, true, milliseconds(0), true).cap == 144);
    CHECK(app::frame_pace(500, 0, true, milliseconds(0), true).cap == 500); // not capped at 240
}

TEST_CASE("Frame pace: outside a light screen max_fps 0 stays unlimited and set caps are followed") {
    CHECK(app::frame_pace(0, 0, true, milliseconds(0), true).cap == 240); // for contrast
    CHECK(app::frame_pace(0, 0, true, milliseconds(0), false).cap == 0);
    CHECK(app::frame_pace(144, 0, true, milliseconds(0), false).cap == 144);
    CHECK(app::frame_pace(1000, 0, true, milliseconds(0), false).cap == 1000);
}

TEST_CASE("Frame pace: the background limit still works on a light screen") {
    CHECK(app::frame_pace(0, 30, false, milliseconds(0), true).cap == 30);
    CHECK(app::frame_pace(0, 5, false, milliseconds(0), true).cap == 10);
    CHECK_FALSE(app::frame_pace(0, 5, false, milliseconds(100), true).draw);
    CHECK(app::frame_pace(0, 5, false, milliseconds(200), true).draw);
    CHECK_FALSE(app::frame_pace(0, app::kBackgroundNoDraw, false, milliseconds(60000), true).draw);
    CHECK(app::frame_pace(60, 144, false, milliseconds(0), true).cap == 60); // min of all three
}

TEST_CASE("Frame pace: an unlimited cap on a light screen stays capped in the background") {
    // background_fps 0 follows max_fps (the 240 light-screen cap); a high background rate cannot raise it.
    CHECK(app::frame_pace(0, 0, false, milliseconds(0), true).cap == 240);
    CHECK(app::frame_pace(0, 1000, false, milliseconds(0), true).cap == 240);
    CHECK(app::frame_pace(500, 1000, false, milliseconds(0), true).cap == 500); // a set max_fps is still followed
}

TEST_CASE("Frame pace: the light-screen condition keeps the zone-in settle light") {
    CHECK_FALSE(app::is_light_screen(true, false)); // InGame without settle: normal play
    CHECK(app::is_light_screen(true, true));        // InGame while settling: the loading overlay is still drawn
    CHECK(app::is_light_screen(false, false));      // a pre-game screen
    CHECK(app::is_light_screen(false, true));
}
