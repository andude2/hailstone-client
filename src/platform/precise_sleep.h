#pragma once

// A wait to a moment on the steady clock, to within tens of microseconds, for the frame cap (app/frame_pace.h):
// std::this_thread::sleep_until on Windows wakes on the system timer's tick (1 ms at best, 15.6 ms by default), so a
// 144 fps cap ran at about 128.
//
// The system's own sleep takes it to shortly before the moment (precise_sleep_margin), and the rest is a spin that
// yields the processor between looks at the clock. On Windows the sleep is a high-resolution waitable timer (Windows 10
// 1803 and later), which wakes within about half a millisecond without changing the system's timer resolution; where
// there is none, the spin takes the last system tick. On POSIX the sleep is nanosleep (std::this_thread::sleep_until).

#include <chrono>

namespace hailstone::platform {

// Returns at `deadline` or just after it; at once when it has passed.
void precise_sleep_until(std::chrono::steady_clock::time_point deadline);

// How long before the deadline the system's sleep ends and the spin begins, on this machine.
std::chrono::microseconds precise_sleep_margin();

// The part of `remaining` to give the system's sleep: all but `margin`, none when that leaves nothing.
inline std::chrono::nanoseconds coarse_sleep_for(std::chrono::nanoseconds remaining, std::chrono::nanoseconds margin) {
    return remaining > margin ? remaining - margin : std::chrono::nanoseconds(0);
}

} // namespace hailstone::platform
