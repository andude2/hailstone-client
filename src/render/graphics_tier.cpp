#include "render/graphics_tier.h"

#include "core/config.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace hailstone::render {

namespace {

constexpr std::uint64_t kGiB = 1024ull * 1024ull * 1024ull;

// Each tier lists the same keys in the same order (tests/graphics_tier_test.cpp checks). The first five are the
// Options window's quality preset (ui/modern/options.lua PRESETS: Low, Medium, High), so the window shows the tier's
// name there; view_distance is written as its slider writes it.
// The upscaler and frame generation: Low turns upscaling and frame generation off, as the defaults have them (FSR 3.1
// costs a fixed share of the frame at the output's size, whatever the render scale, too much for an integrated GPU),
// so Detect turns them off too; Medium and High keep the player's.
// The lighting: Low draws with the classic lighting (the original's look, far lighter than the modern one's per-pixel
// sun, sky and point lights); Medium and High keep the player's choice.
// Radial flora (the Options window's Radial Flora, ShowGrass): off on Low; Medium and High keep the player's.
// The volumetric clouds (a compute pass each frame): off on Low, the painted cloud layer instead; Medium and High keep
// the player's.
constexpr std::size_t kKeyCount = 15;

constexpr std::array<TierSetting, kKeyCount> kLow = {{
    {"display.shadows", "0"},
    {"display.msaa", "1"},
    {"display.ao", "0"},
    {"display.distance_blur", "0"},
    {"display.view_distance", "1.50"},
    {"display.light_shadows", "0"},
    {"display.render_scale", ""}, // the upscaler is off: the view is drawn at full size
    {"display.horizon_hills", "0"},
    {"display.volumetric_clouds", "0"},
    {"display.grass_density", "50"},
    {"display.tree_detail", "50"},
    {"display.upscaler", "off"},
    {"display.frame_gen", "0"},
    {"display.lighting", "classic"},
    {"display.grass", "0"},
}};

constexpr std::array<TierSetting, kKeyCount> kMedium = {{
    {"display.shadows", "1024"},
    {"display.msaa", "2"},
    {"display.ao", "0"},
    {"display.distance_blur", ""},
    {"display.view_distance", "2.00"},
    {"display.light_shadows", "4"},
    {"display.render_scale", ""},
    {"display.horizon_hills", ""},
    {"display.volumetric_clouds", ""},
    {"display.grass_density", "75"},
    {"display.tree_detail", ""},
    {"display.upscaler", "", true},
    {"display.frame_gen", "", true},
    {"display.lighting", "", true},
    {"display.grass", "", true},
}};

constexpr std::array<TierSetting, kKeyCount> kHigh = {{
    {"display.shadows", ""},
    {"display.msaa", ""},
    {"display.ao", ""},
    {"display.distance_blur", ""},
    {"display.view_distance", ""},
    {"display.light_shadows", ""},
    {"display.render_scale", ""},
    {"display.horizon_hills", ""},
    {"display.volumetric_clouds", ""},
    {"display.grass_density", ""},
    {"display.tree_detail", ""},
    {"display.upscaler", "", true},
    {"display.frame_gen", "", true},
    {"display.lighting", "", true},
    {"display.grass", "", true},
}};

} // namespace

GraphicsTier detect_graphics_tier(const GpuProfile& gpu) {
    GraphicsTier tier = GraphicsTier::Low;
    if (gpu.kind == GpuKind::Discrete)
        tier = gpu.device_local_bytes >= 6 * kGiB   ? GraphicsTier::High
               : gpu.device_local_bytes >= 3 * kGiB ? GraphicsTier::Medium
                                                    : GraphicsTier::Low;
    else if (gpu.kind == GpuKind::Integrated && gpu.vendor_id == kVendorApple)
        tier = GraphicsTier::Medium;
    if (gpu.cpu_threads > 0 && gpu.cpu_threads < 4) tier = GraphicsTier::Low;
    else if (gpu.cpu_threads > 0 && gpu.cpu_threads < 6) tier = std::min(tier, GraphicsTier::Medium);
    return tier;
}

std::string_view graphics_tier_name(GraphicsTier tier) {
    switch (tier) {
    case GraphicsTier::Low: return "low";
    case GraphicsTier::Medium: return "medium";
    case GraphicsTier::High: return "high";
    }
    return "high";
}

std::optional<GraphicsTier> parse_graphics_tier(std::string_view name) {
    for (GraphicsTier t : {GraphicsTier::Low, GraphicsTier::Medium, GraphicsTier::High})
        if (graphics_tier_name(t) == name) return t;
    return std::nullopt;
}

std::span<const TierSetting> graphics_tier_settings(GraphicsTier tier) {
    switch (tier) {
    case GraphicsTier::Low: return kLow;
    case GraphicsTier::Medium: return kMedium;
    case GraphicsTier::High: return kHigh;
    }
    return kHigh;
}

void apply_graphics_tier(Config& config, GraphicsTier tier) {
    for (const TierSetting& s : graphics_tier_settings(tier)) {
        if (s.keep) continue;
        if (s.value.empty()) config.erase(s.key);
        else config.set(s.key, s.value);
    }
}

std::optional<GraphicsTier> first_start_graphics(Config& config, const GpuProfile& gpu) {
    if (config.get(kGraphicsTierKey)) return std::nullopt;
    const GraphicsTier tier = detect_graphics_tier(gpu);
    const auto entries = config.entries();
    const bool new_install = std::none_of(entries.begin(), entries.end(),
                                          [](const auto& e) { return e.first.starts_with("display."); });
    config.set(kGraphicsTierKey, graphics_tier_name(tier));
    if (!new_install) return std::nullopt;
    apply_graphics_tier(config, tier);
    return tier;
}

} // namespace hailstone::render
