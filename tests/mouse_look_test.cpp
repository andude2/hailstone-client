// The mouse look's presses and releases (game/mouse_look.h): CEverQuest::RMouseDown @0x518910, RMouseUp @0x537d30,
// MouseLookEngage @0x518870 and LMouseUp @0x5384d0 with the Mouse page's MouseActive and MouseTurnZoom.

#include <doctest/doctest.h>

#include "game/mouse_look.h"

using hailstone::game::MouseLook;
using hailstone::game::MouseLookOptions;

TEST_CASE("mouse look: the right button looks with MouseActive, and a short press still clicks") {
    MouseLookOptions on{true, true};
    MouseLook m;
    m.right_down(on, 1000);
    CHECK(m.active);
    CHECK(m.by_right);
    auto up = m.right_up(1400); // held 400 ms
    CHECK_FALSE(m.active);
    CHECK(up.click);
    CHECK_FALSE(up.center_cursor);

    m.right_down(on, 2000);
    up = m.right_up(2501); // over 500 ms: the look only (CenterMouseCursor: the game's copy of the cursor)
    CHECK_FALSE(m.active);
    CHECK_FALSE(up.click);
    CHECK(up.center_cursor);
    CHECK(m.by_right); // stays until a left-button look
}

TEST_CASE("mouse look: without MouseActive the right button only clicks") {
    MouseLookOptions off{false, true};
    MouseLook m;
    m.right_down(off, 1000);
    CHECK_FALSE(m.active);
    CHECK_FALSE(m.by_right);
    auto up = m.right_up(9000); // any length
    CHECK(up.click);
    // nor does the left button look in the camera 6
    m.left_down(off, true, 9100);
    CHECK_FALSE(m.active);
    CHECK(m.left_up(9200));
}

TEST_CASE("mouse look: MouseTurnZoom's left-button look in the camera 6") {
    MouseLookOptions on{true, true};
    MouseLook m;
    m.left_down(on, false, 1000); // first person: no look
    CHECK_FALSE(m.active);
    CHECK(m.left_up(1100));      // a click
    CHECK_FALSE(m.left_up(1000 + 0x2ef)); // held too long for one (from the same press)

    m.left_down(on, true, 2000);
    CHECK(m.active);
    CHECK_FALSE(m.by_right);
    CHECK(m.turns_camera(true)); // the camera turns, not the player
    CHECK(m.left_up(2300));      // the look ends and a short press clicks
    CHECK_FALSE(m.active);

    m.left_down(on, true, 3000);
    CHECK_FALSE(m.left_up(4000)); // held 1 s: no click

    MouseLookOptions no_turn_zoom{true, false};
    m.left_down(no_turn_zoom, true, 5000);
    CHECK_FALSE(m.active);
}

TEST_CASE("mouse look: both buttons") {
    MouseLookOptions on{true, true};
    MouseLook m;
    // right look, then the left button: both, no click at its release
    m.right_down(on, 1000);
    m.left_down(on, true, 1100);
    CHECK(m.both_buttons);
    CHECK(m.active);
    CHECK_FALSE(m.turns_camera(true));
    CHECK_FALSE(m.left_up(1200));
    CHECK_FALSE(m.both_buttons);
    CHECK(m.active); // the right button still looks
    m.right_up(1300);

    // left look in the camera 6, then the right button: both, the player turns
    m.left_down(on, true, 2000);
    m.right_down(on, 2100);
    CHECK(m.both_buttons);
    CHECK_FALSE(m.by_right);
    CHECK_FALSE(m.turns_camera(true));
    auto up = m.right_up(2200); // the left look goes on: no right click
    CHECK_FALSE(up.click);
    CHECK(m.active);
    CHECK_FALSE(m.both_buttons);
    m.left_up(2300);
    CHECK_FALSE(m.active);

    m.right_down(on, 3000);
    m.cancel(); // dead or knocked out
    CHECK_FALSE(m.active);
}

TEST_CASE("pointer speed: ProcessMouseInput's cursor (MousePointerSpeedMod)") {
    using hailstone::game::pointer_step;
    // 0 and 1 move it as the mouse moves; 2 and up multiply the motion.
    auto s = pointer_step(100, 100, 5, -3, 0, 800, 600);
    CHECK((s.x == 105 && s.y == 97 && !s.leave));
    s = pointer_step(100, 100, 5, -3, 1, 800, 600);
    CHECK((s.x == 105 && s.y == 97));
    s = pointer_step(100, 100, 5, -3, 4, 800, 600);
    CHECK((s.x == 120 && s.y == 88));
    // Clamped to 0..width - 1, 0..height - 1; pushing on past an edge it is already at lets it go.
    s = pointer_step(790, 10, 10, 0, 2, 800, 600);
    CHECK((s.x == 799 && !s.leave));
    s = pointer_step(799, 10, 1, 0, 2, 800, 600);
    CHECK((s.x == 799 && s.leave));
    s = pointer_step(5, 0, 0, -1, 3, 800, 600);
    CHECK((s.y == 0 && s.leave));
    s = pointer_step(5, 0, 0, 1, 3, 800, 600);
    CHECK((s.y == 3 && !s.leave));
}

TEST_CASE("mouse buttons: a click inside one frame is a press and a release") {
    using hailstone::game::button_edges;
    // Down and up again before the frame read the state (a quick click at 15 fps).
    auto quick = button_edges(true, false, false);
    CHECK(quick.pressed);
    CHECK(quick.released);
    // Held over frames: one press, then nothing, then one release.
    CHECK(button_edges(true, true, false).pressed);
    auto held = button_edges(false, true, true);
    CHECK_FALSE(held.pressed);
    CHECK_FALSE(held.released);
    auto up = button_edges(false, false, true);
    CHECK_FALSE(up.pressed);
    CHECK(up.released);
    // Up and down again inside one frame while it was held: a new press (the release between is not seen).
    auto again = button_edges(true, true, true);
    CHECK(again.pressed);
    CHECK_FALSE(again.released);
    CHECK_FALSE(button_edges(false, false, false).pressed);
}

TEST_CASE("pointer speed: /mousespeed's number is held to eqclient.ini's 0..10") {
    using hailstone::game::held_pointer_speed;
    CHECK(held_pointer_speed(0) == 0);
    CHECK(held_pointer_speed(7) == 7);
    CHECK(held_pointer_speed(10) == 10);
    CHECK(held_pointer_speed(999999) == 10);
    CHECK(held_pointer_speed(-5) == 0);
    CHECK(held_pointer_speed(INT32_MIN) == 0);
}
