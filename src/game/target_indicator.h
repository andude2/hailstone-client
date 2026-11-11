#pragma once

// The 3D target indicator (TargetIndicator, pinstTargetIndicator): the ring at the target's feet, coloured by how
// the target cons. Its settings come from the UI skin's TargetIndicator.ini (the default UI's when the skin has
// none; TargetIndicator_LoadIni @0x5c4d60): one section per con colour, Trivial, VeryEasy, Easy, FairlyEasy,
// FairMatch, Difficult, Deadly, then Free and FreeInvalid (the free-target marker), each read by
// TargetIndicator_LoadIndicatorSettings @0x5c4870. The Options window's Show3dTargetIndicator
// (TargetIndicator_SetEnabled @0x5c3f10, from DisplayPageWndNotification) and /indicator on|off turn it on and off.
//
// The original builds four concentric UV-animated circles (0x5c46f0, 0x5c4430); this client draws the
// ring its own way (render/target_ring_renderer.h, docs/graphics.md "The target ring"): only which ring shows, its
// colour and alpha and its size follow the original. TargetRingLook below is Hailstone's.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "game/combat.h"

namespace hailstone {
class IniFile;
}

namespace hailstone::game {

// One section's colour (LoadIndicatorSettings: Alpha default 0xc0, Red / Green / Blue default 0xff, each kept as a
// byte) and FloorOffset (default 1.0).
struct TargetRingStyle {
    std::uint8_t alpha = 0xc0, red = 0xff, green = 0xff, blue = 0xff;
    float floor_offset = 1.0f;
};

inline constexpr std::array<std::string_view, 9> kTargetIndicatorSections{
    "Trivial", "VeryEasy", "Easy", "FairlyEasy", "FairMatch", "Difficult", "Deadly", "Free", "FreeInvalid"};

struct TargetIndicatorSettings {
    std::array<TargetRingStyle, 9> styles{}; // by kTargetIndicatorSections (TargetIndicator +0x1c, 0xac apart)
    // A missing section or key keeps LoadIndicatorSettings' default.
    static TargetIndicatorSettings parse(const IniFile& ini);
};

// TargetIndicator_UpdateTargetRing @0x5c5250: the section the target's PcClient::GetConLevel picks (gray 0
// Trivial .. red 6 Deadly); none for con 0 (corpses and the rest GetConLevel gives 0), which hides the ring.
// With no target, or the indicator off, the ring is hidden too.
std::optional<int> target_ring_section(Con con);

// 0x5c46f0: the ring's size, the target's AvatarHeight (+0x138; 3 without a target) times its
// actor's scale when it has an actor, at least 2.
float target_ring_radius(float avatar_height, float actor_scale, bool has_actor);

// Hailstone, a deliberate difference: the player's cap on the ring's radius (Options > Display "Target Ring" Max
// Size, hailstone.ini display.target_ring_max_size), so a giant's or a dragon's ring does not fill the screen.
// 0 is off, the original's size; otherwise kTargetRingMaxSizeMin .. kTargetRingMaxSizeMax world units.
inline constexpr int kTargetRingMaxSizeOff = 0;
inline constexpr int kTargetRingMaxSizeMin = 4, kTargetRingMaxSizeMax = 40;
// The setting kept in range: 0 (and anything past the most) off, the rest at least the least.
int clamp_ring_max_size(int max_size);
float cap_ring_radius(float radius, int max_size);

// Hailstone's look (not the original's): a soft band with dashes turning round it, a notch where the target faces,
// and, with the cues on (hailstone.ini display.target_ring_cues), a sweep in from outside when the target changes
// and a faster turn and a pulse while the player's auto attack is on.
inline constexpr float kTargetRingFlashSeconds = 0.35f; // the sweep's length
inline constexpr float kTargetRingSpinIdle = 0.25f;      // radians a second
inline constexpr float kTargetRingSpinAttack = 1.1f;
inline constexpr float kTargetRingPulseHz = 1.25f;

struct TargetRingLook {
    float width = 0.0f; // the band's half width, world units
    float pulse = 0.0f; // 0..1, how far the band has swelled
    float flash = 0.0f; // the sweep: 1 just changed .. 0 done
};

// Hailstone: the player's own ring colours and strength over TargetIndicator.ini's (Options > Display "Target
// Ring"; hailstone.ini display.target_ring_color.<Section> as RRGGBB, display.target_ring_opacity in % of the
// section's alpha, display.target_ring_glow 0..100).
inline constexpr int kTargetRingConSections = 7; // Trivial .. Deadly, the ones a con picks
inline constexpr int kTargetRingOpacityMin = 25, kTargetRingOpacityMax = 200, kTargetRingOpacityDefault = 100;
inline constexpr int kTargetRingGlowDefault = 50;

struct TargetRingColors {
    std::array<std::optional<std::uint32_t>, kTargetRingConSections> rgb{}; // 0xRRGGBB, none: the ini's
    int opacity = kTargetRingOpacityDefault;
    int glow = kTargetRingGlowDefault;
};

// "RRGGBB" (hex, an optional leading '#'); nullopt for anything else.
std::optional<std::uint32_t> parse_ring_rgb(std::string_view text);
std::string format_ring_rgb(std::uint32_t rgb);
// The section's style with the player's colour in place of the ini's and its alpha scaled by the opacity (at most 255).
TargetRingStyle apply_ring_colors(const TargetRingStyle& ini, int section, const TargetRingColors& colors);
// How far past the colour the band's core goes (0 none .. 1.4 at glow 100; 0.7 at the default).
float target_ring_core_boost(int glow);

// The band's half width: a share of the radius, kept readable on a small target and not a stripe on a dragon.
float target_ring_width(float radius);
// How fast the dashes turn (the app sums it over the frames, so a change of speed does not jump).
float target_ring_spin_rate(bool attacking, bool cues);
// `seconds` a running clock, `since_change` the time since the target changed (negative: unknown, no sweep).
TargetRingLook target_ring_look(float radius, double seconds, double since_change, bool attacking, bool cues);

} // namespace hailstone::game
