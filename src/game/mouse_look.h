#pragma once

// The original's mouse look: which button press turns it on, what the release does, and whether the click under
// the cursor still happens. __ProcessMouseEvents @0x539e60 sends a world press or release (one the windows did not
// take, or any while mouse look is on) to CEverQuest::RMouseDown @0x518910 / RMouseUp @0x537d30 and, for the left
// button, MouseLookEngage @0x518870 / CEverQuest::LMouseUp @0x5384d0. ProcessMouseInput @0x5f9e30 swaps the two
// buttons first unless MouseRightHanded (0xddf744) is set. The Options window's Mouse page (MousePageWndNotification
// @0x703a30) sets the options these read.

#include <cstdint>

namespace hailstone::game {

// A mouse button's press and release this frame, from its state (held now, held last frame) and whether it went down
// in the frame's events. A click that is down and up inside one frame (common at low frame rates) is both.
struct ButtonEdges {
    bool pressed = false;
    bool released = false;
};
inline ButtonEdges button_edges(bool went_down, bool held, bool was_held) {
    bool pressed = went_down || (held && !was_held);
    return {pressed, (was_held || pressed) && !held};
}

struct MouseLookOptions {
    bool mouse_active = true; // 0xddf674 (Options MouseActive; the MOUSELOOK key toggles it): the right button looks
    bool turn_zoom = true;    // 0xde0d14 (Options MouseTurnZoom): the left button looks too, in the camera 6
};

struct MouseLook {
    bool active = false;       // __MouseLook
    bool by_right = false;     // EverQuestInfo::bDefaultMouseLook: the right button turned it on (it stays set after
                               // the release until the left button turns mouse look on)
    bool both_buttons = false; // 0xddf673: the other button went down during mouse look (moves forward; in the
                               // camera 6 the mouse turns the player, not the camera)
    std::uint32_t right_down_at = 0; // __RMouseHeldTime
    std::uint32_t left_down_at = 0;  // __LMouseHeldTime

    // CEverQuest::RMouseDown (3D on). During a left-button look the press only sets both_buttons; otherwise mouse
    // look takes MouseActive and, when on, by_right is set.
    void right_down(const MouseLookOptions& o, std::uint32_t now);
    // CEverQuest::RMouseUp: false when the release only ended a look (a right-button look held over 500 ms, which
    // also calls CenterMouseCursor: the game's copy of the cursor only, read back from the system's cursor the next
    // frame, so the cursor does not move; platform/mouse_capture.h) or a left-button look goes on; true when the
    // right click acts (ClickedPlayer and RightClickedOnPlayer).
    struct RightUp {
        bool click = false;
        bool center_cursor = false; // CenterMouseCursor (no lasting effect: see above)
    };
    RightUp right_up(std::uint32_t now);
    // MouseLookEngage: during a right-button look the press only sets both_buttons; else in the camera 6 with
    // MouseTurnZoom mouse look takes MouseActive, and a look turned on clears by_right.
    void left_down(const MouseLookOptions& o, bool camera6, std::uint32_t now);
    // CEverQuest::LMouseUp: a left-button look ends; the click (ClickedPlayer, LeftClickedOnPlayer, ...) goes on
    // when mouse look is off and the button was held under 0x2ef ms.
    bool left_up(std::uint32_t now);
    // __ProcessMouseEvents while the player is dead or knocked out (Cur_HP below 1, or the PC's state 3 / 4): mouse
    // look ends (the windows still get the mouse).
    void cancel() { active = false; }
    // HandleMouseLook @0x516d40 in the camera 6: the mouse turns the camera's heading, not the player's, during a
    // left-button look without the right button.
    bool turns_camera(bool camera6) const { return camera6 && !by_right && !both_buttons; }
};

// ProcessMouseInput @0x5f9e30's cursor (__Mouse / 0xe67b58): the mouse's movement this frame, multiplied by
// MousePointerSpeedMod (0xddf6a0; eqclient.ini 0..10, else 0) when that is above 1, moves it, clamped to 0..width - 1
// and 0..height - 1. `leave` is this client's: the step pushed on past an edge the cursor was already at (the
// system's cursor is let go there, docs/unknowns.md).
struct PointerStep {
    float x = 0.0f, y = 0.0f;
    bool leave = false;
};
PointerStep pointer_step(float x, float y, float dx, float dy, std::int32_t speed, float width, float height);

// /mousespeed's number as this client applies it: held to eqclient.ini's 0..10. A deliberate change: Cmd_mousespeed
// @0x4f5130 keeps the argument as typed, so /mousespeed 999999 multiplied every step by it (and -5 was no speed).
inline constexpr std::int32_t kMaxPointerSpeed = 10;
constexpr std::int32_t held_pointer_speed(std::int32_t speed) {
    return speed < 0 ? 0 : speed > kMaxPointerSpeed ? kMaxPointerSpeed : speed;
}

// CEverQuest::LMouseUp's limit: a press held this long (ms) is no longer a click.
inline constexpr std::uint32_t kLeftClickMaxHeldMs = 0x2ef;
// CEverQuest::RMouseUp's: a right-button look held longer is not a right click.
inline constexpr std::uint32_t kRightLookClickMaxHeldMs = 500;

} // namespace hailstone::game
