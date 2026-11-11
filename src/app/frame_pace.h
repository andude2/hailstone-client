#pragma once

// The main loop's frame cap and whether a frame is drawn (app/client_app.cpp): display.max_fps, and
// display.background_fps while another window has the focus. Below kBackgroundTickHz in the background, the loop
// keeps going at kBackgroundTickHz (the game clock, the connection and the UI pack's scripts run on) and draws only
// as often as the setting says, or never at kBackgroundNoDraw. The game clock catches up at most 250 ms a frame
// (core/step_clock.h kMaxSteps), so a loop slower than 4 Hz would slow the rules down.

#include <algorithm>
#include <chrono>

namespace hailstone::app {

inline constexpr int kBackgroundNoDraw = -1; // display.background_fps: nothing drawn while unfocused
inline constexpr int kBackgroundTickHz = 10;

struct FramePace {
    int cap = 0;      // the loop's frames a second (0 none)
    bool draw = true; // this frame is drawn
};

// display.background_fps as it is kept: 0 (as max_fps), kBackgroundNoDraw, or 1 .. 1000.
inline int background_fps_setting(int value) {
    return value < 0 ? kBackgroundNoDraw : value == 0 ? 0 : std::min(value, 1000);
}

// max_fps 0: none. background_fps 0: as max_fps; kBackgroundNoDraw: none drawn; else drawn frames a second.
// since_drawn: the time since the last drawn frame.
inline FramePace frame_pace(int max_fps, int background_fps, bool focused, std::chrono::microseconds since_drawn) {
    FramePace pace{max_fps, true};
    if (focused || background_fps == 0) return pace;
    const int loop = std::max(background_fps, kBackgroundTickHz);
    pace.cap = max_fps > 0 ? std::min(max_fps, loop) : loop;
    if (background_fps == kBackgroundNoDraw) pace.draw = false;
    else if (background_fps < kBackgroundTickHz)
        // Half a tick early: the loop's ticks land on the interval or just after it, not a whole tick late.
        pace.draw = since_drawn >= std::chrono::microseconds(1000000 / background_fps - 500000 / pace.cap);
    return pace;
}

} // namespace hailstone::app
