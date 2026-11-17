#include "platform/precise_sleep.h"

#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002 // Windows 10 1803; older SDKs lack the name
#endif
#endif

namespace hailstone::platform {

namespace {

using Clock = std::chrono::steady_clock;

#ifdef _WIN32
// One timer per thread that sleeps (the main loop's), made at its first sleep; null where the system has no
// high-resolution timers (before Windows 10 1803).
struct HighResolutionTimer {
    HANDLE handle = nullptr;
    HighResolutionTimer() {
        handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    ~HighResolutionTimer() {
        if (handle) CloseHandle(handle);
    }
    HighResolutionTimer(const HighResolutionTimer&) = delete;
    HighResolutionTimer& operator=(const HighResolutionTimer&) = delete;
};

HighResolutionTimer& timer() {
    thread_local HighResolutionTimer t;
    return t;
}

// The timer's wake-ups land within about 0.5 ms; without it the system's tick (up to 15.6 ms) is left to the spin.
constexpr std::chrono::microseconds kTimerMargin{1000};
constexpr std::chrono::microseconds kTickMargin{16000};

void coarse_sleep(std::chrono::nanoseconds d) {
    if (HANDLE h = timer().handle) {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(d.count() / 100); // relative, in 100 ns units
        if (due.QuadPart < 0 && SetWaitableTimer(h, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(h, INFINITE);
            return;
        }
    }
    std::this_thread::sleep_for(d);
}
#else
// nanosleep wakes within tens of microseconds on Linux (its timer slack is 50 us) and about as well on macOS.
constexpr std::chrono::microseconds kSleepMargin{500};

void coarse_sleep(std::chrono::nanoseconds d) { std::this_thread::sleep_for(d); }
#endif

} // namespace

std::chrono::microseconds precise_sleep_margin() {
#ifdef _WIN32
    return timer().handle ? kTimerMargin : kTickMargin;
#else
    return kSleepMargin;
#endif
}

void precise_sleep_until(Clock::time_point deadline) {
    const auto margin = precise_sleep_margin();
    const auto now = Clock::now();
    if (now >= deadline) return;
    const auto coarse = coarse_sleep_for(deadline - now, margin);
    if (coarse.count() > 0) coarse_sleep(coarse);
    // The rest: a spin that hands the processor to any other ready thread between looks at the clock.
    while (Clock::now() < deadline) std::this_thread::yield();
}

} // namespace hailstone::platform
