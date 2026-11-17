#pragma once

// The main loop's frame cap and whether a frame is drawn (app/client_app.cpp): display.max_fps, and
// display.background_fps while another window has the focus. Below kBackgroundTickHz in the background, the loop
// keeps going at kBackgroundTickHz (the game clock, the connection and the UI pack's scripts run on) and draws only
// as often as the setting says, or never at kBackgroundNoDraw. The game clock catches up at most 250 ms a frame
// (core/step_clock.h kMaxSteps), so a loop slower than 4 Hz would slow the rules down. On the login and loading screens
// (including the last stretch of a zone-in) an unlimited max_fps counts as kLightScreenCap, so they do not spin.

#include <algorithm>
#include <chrono>

namespace hailstone::app {

inline constexpr int kBackgroundNoDraw = -1; // display.background_fps: nothing drawn while unfocused
inline constexpr int kBackgroundTickHz = 10;
inline constexpr int kLightScreenCap = 240; // display.max_fps 0 on a login/loading screen counts as this

struct FramePace {
    int cap = 0;      // the loop's frames a second (0 none)
    bool draw = true; // this frame is drawn
};

// display.background_fps as it is kept: 0 (as max_fps), kBackgroundNoDraw, or 1 .. 1000.
inline int background_fps_setting(int value) {
    return value < 0 ? kBackgroundNoDraw : value == 0 ? 0 : std::min(value, 1000);
}

// The light-screen condition, kept here so it is testable without the heavy client_app_internal.h. in_game is
// screen_ == Screen::InGame; settling is zone_settling(). The last stretch of a zone-in still draws the loading
// overlay while in_game is already true, so settling must keep the screen light.
inline bool is_light_screen(bool in_game, bool settling) {
    return !in_game || settling;
}

// max_fps 0: none. background_fps 0: as max_fps; kBackgroundNoDraw: none drawn; else drawn frames a second.
// since_drawn: the time since the last drawn frame.
// light_screen: a login/loading screen, where an unlimited cap would spin; max_fps 0 counts as kLightScreenCap there.
// Callers pass false during normal play (so max_fps 0 stays unlimited); a set max_fps is followed exactly either way.
inline FramePace frame_pace(int max_fps, int background_fps, bool focused, std::chrono::microseconds since_drawn,
    bool light_screen = false) {
    const int cap = light_screen && max_fps == 0 ? kLightScreenCap : max_fps;
    FramePace pace{cap, true};
    if (focused || background_fps == 0) return pace;
    const int loop = std::max(background_fps, kBackgroundTickHz);
    pace.cap = cap > 0 ? std::min(cap, loop) : loop;
    if (background_fps == kBackgroundNoDraw) pace.draw = false;
    else if (background_fps < kBackgroundTickHz)
        // Half a tick early: the loop's ticks land on the interval or just after it, not a whole tick late.
        pace.draw = since_drawn >= std::chrono::microseconds(1000000 / background_fps - 500000 / pace.cap);
    return pace;
}

} // namespace hailstone::app
