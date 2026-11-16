#pragma once

// Graphics settings for the computer Hailstone runs on (docs/graphics.md "Detected settings"). At the first start the
// GPU and the CPU are sorted into a tier, and a tier below High writes the Options window's matching quality preset
// (ui/modern/options.lua PRESETS) and a few more of the costlier settings into hailstone.ini, so a weak machine does
// not start with everything at its highest. Kept apart from Vulkan (render/vk_context.cpp fills GpuProfile) so the
// thresholds are tested.
//
// The thresholds are a heuristic, not a measurement: Options > Graphics' "Detect" applies the tier again, and the
// preset or any setting can be changed as before.

#include "render/gpu_requirements.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace hailstone { class Config; }

namespace hailstone::render {

enum class GraphicsTier : std::uint8_t { Low, Medium, High };

// What the tier is chosen from.
struct GpuProfile {
    std::string name;
    GpuKind kind = GpuKind::Other;
    std::uint32_t vendor_id = 0;           // VkPhysicalDeviceProperties::vendorID
    std::uint64_t device_local_bytes = 0;  // the largest DEVICE_LOCAL memory heap (a discrete GPU's own memory)
    unsigned cpu_threads = 0;              // std::thread::hardware_concurrency, 0 unknown
};

inline constexpr std::uint32_t kVendorApple = 0x106b;

// Discrete: 6 GiB of its own memory or more High, 3 GiB or more Medium, less Low. Integrated: Apple's Medium (their
// unified memory and GPUs are far ahead of other integrated ones), every other Low. Anything else (a software
// renderer such as llvmpipe, a virtual GPU) Low. Then the CPU: under 4 threads Low, under 6 at most Medium.
GraphicsTier detect_graphics_tier(const GpuProfile& gpu);

std::string_view graphics_tier_name(GraphicsTier tier); // "low", "medium", "high"
std::optional<GraphicsTier> parse_graphics_tier(std::string_view name);

struct TierSetting {
    std::string_view key, value;
    bool keep = false; // the tier leaves the key as it is
};

// The hailstone.ini keys a tier decides, each with the tier's value; an empty value is the built-in default (High has
// every one at its default or kept, so later changes to the defaults reach it).
std::span<const TierSetting> graphics_tier_settings(GraphicsTier tier);

// Writes the tier's settings: each key with a value is set, each without is erased (its default), a kept one is left.
void apply_graphics_tier(Config& config, GraphicsTier tier);

// The key that records the tier found (and that the detection ran): shared, in hailstone.ini.
inline constexpr std::string_view kGraphicsTierKey = "display.gpu_tier";

// The first start's detection: when the config has no kGraphicsTierKey, it is set to the tier of `gpu`, and the tier
// is applied when the config holds no display.* key yet (a new install; an existing one keeps its settings). Returns
// the tier when it was applied. The caller saves the config.
std::optional<GraphicsTier> first_start_graphics(Config& config, const GpuProfile& gpu);

} // namespace hailstone::render
