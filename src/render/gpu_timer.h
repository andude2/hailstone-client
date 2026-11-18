#pragma once

// The GPU time of the frame's passes, for the performance window (app/perf_ui.h "GPU passes"): VkContext::gpu_mark
// writes a timestamp after a pass's commands, and each mark's section is the GPU time since the one before it (the
// first, the frame's start), as core/frame_sections.h's Lap charges the CPU's. The frame's timestamps are read back
// when its frame-in-flight fence has been waited for, so nothing waits on the GPU; the sections shown are a frame or
// two old.

#include <cstdint>
#include <span>
#include <vector>

namespace hailstone::render {

// The most marks a frame keeps (and one more timestamp, the frame's start); later marks in the frame are dropped.
inline constexpr std::uint32_t kMaxGpuMarks = 31;

struct GpuSection {
    const char* name; // static storage, compared by address (as core::frame_sections)
    float ms;
};

// The sections between `ticks[0]` (the frame's start) and each later tick, named by `names` (one per tick after the
// first), in `period_ns` nanoseconds a tick (VkPhysicalDeviceLimits::timestampPeriod). Only the low `valid_bits` of
// a tick count (VkQueueFamilyProperties::timestampValidBits), so a counter that wrapped between two marks still
// gives their difference. The same name twice adds up. Into `out` (cleared first); nothing for a period of 0 or
// ticks that do not match the names.
void gpu_sections(std::span<const char* const> names, std::span<const std::uint64_t> ticks, float period_ns,
                  std::uint32_t valid_bits, std::vector<GpuSection>& out);

} // namespace hailstone::render
