#include "game/target_indicator.h"

#include <algorithm>
#include <cmath>
#include <charconv>
#include <cstdlib>
#include <format>
#include <string>

#include "core/ini_file.h"

namespace hailstone::game {

TargetIndicatorSettings TargetIndicatorSettings::parse(const IniFile& ini) {
    TargetIndicatorSettings out;
    for (std::size_t k = 0; k < kTargetIndicatorSections.size(); ++k) {
        std::string_view section = kTargetIndicatorSections[k];
        TargetRingStyle& s = out.styles[k];
        auto byte = [&](std::string_view key, std::uint8_t fallback) {
            return static_cast<std::uint8_t>(ini.get_int(section, key, fallback) & 0xff);
        };
        s.alpha = byte("Alpha", 0xc0);
        s.red = byte("Red", 0xff);
        s.green = byte("Green", 0xff);
        s.blue = byte("Blue", 0xff);
        if (auto v = ini.get(section, "FloorOffset")) s.floor_offset = std::strtof(v->c_str(), nullptr);
    }
    return out;
}

std::optional<int> target_ring_section(Con con) {
    switch (con) {
    case Con::Gray: return 0;
    case Con::Green: return 1;
    case Con::LightBlue: return 2;
    case Con::DarkBlue: return 3;
    case Con::White: return 4;
    case Con::Yellow: return 5;
    case Con::Red: return 6;
    default: return std::nullopt;
    }
}

float target_ring_radius(float avatar_height, float actor_scale, bool has_actor) {
    float r = has_actor ? avatar_height * actor_scale : avatar_height;
    return std::max(r, 2.0f);
}

int clamp_ring_max_size(int max_size) {
    if (max_size <= kTargetRingMaxSizeOff || max_size > kTargetRingMaxSizeMax) return kTargetRingMaxSizeOff;
    return std::max(max_size, kTargetRingMaxSizeMin);
}

float cap_ring_radius(float radius, int max_size) {
    max_size = clamp_ring_max_size(max_size);
    if (max_size == kTargetRingMaxSizeOff) return radius;
    return std::min(radius, static_cast<float>(max_size));
}

std::optional<std::uint32_t> parse_ring_rgb(std::string_view text) {
    if (!text.empty() && text.front() == '#') text.remove_prefix(1);
    if (text.size() != 6) return std::nullopt;
    std::uint32_t v = 0;
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v, 16);
    if (ec != std::errc() || end != text.data() + text.size()) return std::nullopt;
    return v;
}

std::string format_ring_rgb(std::uint32_t rgb) { return std::format("{:06x}", rgb & 0xffffffu); }

TargetRingStyle apply_ring_colors(const TargetRingStyle& ini, int section, const TargetRingColors& colors) {
    TargetRingStyle out = ini;
    if (section >= 0 && section < kTargetRingConSections)
        if (auto rgb = colors.rgb[static_cast<std::size_t>(section)]) {
            out.red = static_cast<std::uint8_t>((*rgb >> 16) & 0xff);
            out.green = static_cast<std::uint8_t>((*rgb >> 8) & 0xff);
            out.blue = static_cast<std::uint8_t>(*rgb & 0xff);
        }
    int opacity = std::clamp(colors.opacity, kTargetRingOpacityMin, kTargetRingOpacityMax);
    out.alpha = static_cast<std::uint8_t>(std::min(255, (static_cast<int>(ini.alpha) * opacity + 50) / 100));
    return out;
}

float target_ring_core_boost(int glow) { return static_cast<float>(std::clamp(glow, 0, 100)) * 0.014f; }

float target_ring_width(float radius) { return std::clamp(radius * 0.06f, 0.15f, 0.5f); }

float target_ring_spin_rate(bool attacking, bool cues) {
    return cues && attacking ? kTargetRingSpinAttack : kTargetRingSpinIdle;
}

TargetRingLook target_ring_look(float radius, double seconds, double since_change, bool attacking, bool cues) {
    TargetRingLook out;
    out.width = target_ring_width(radius);
    if (!cues) return out;
    if (attacking) {
        double turn = std::fmod(seconds * kTargetRingPulseHz, 1.0);
        out.pulse = static_cast<float>(0.5 - 0.5 * std::cos(turn * 2.0 * 3.14159265358979323846));
    }
    if (since_change >= 0.0 && since_change < kTargetRingFlashSeconds)
        out.flash = 1.0f - static_cast<float>(since_change) / kTargetRingFlashSeconds;
    return out;
}

} // namespace hailstone::game
