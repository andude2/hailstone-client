// The 3D target indicator's rules (game/target_indicator.h).
#include <doctest/doctest.h>

#include "core/ini_file.h"
#include "game/target_indicator.h"

using namespace hailstone;
using game::Con;

TEST_CASE("target indicator: the con picks the section, con 0 hides the ring") {
    CHECK_FALSE(game::target_ring_section(Con::None));
    CHECK(game::target_ring_section(Con::Gray) == 0);      // Trivial
    CHECK(game::target_ring_section(Con::Green) == 1);     // VeryEasy
    CHECK(game::target_ring_section(Con::LightBlue) == 2); // Easy
    CHECK(game::target_ring_section(Con::DarkBlue) == 3);  // FairlyEasy
    CHECK(game::target_ring_section(Con::White) == 4);     // FairMatch
    CHECK(game::target_ring_section(Con::Yellow) == 5);    // Difficult
    CHECK(game::target_ring_section(Con::Red) == 6);       // Deadly
}

TEST_CASE("target indicator: TargetIndicator.ini's colours, LoadIndicatorSettings' defaults") {
    IniFile ini;
    ini.parse("[TargetIndicator]\nAdditive=1\nPointCount=64\n[Deadly]\nAlpha=255\nRed=255\nGreen=0\nBlue=0\n"
              "FloorOffset=0.5\t\n[Easy]\nRed=0\nGreen=100\nBlue=200\nAlpha=256\n");
    auto s = game::TargetIndicatorSettings::parse(ini);
    const auto& deadly = s.styles[6];
    CHECK(deadly.alpha == 255);
    CHECK(deadly.red == 255);
    CHECK(deadly.green == 0);
    CHECK(deadly.blue == 0);
    CHECK(deadly.floor_offset == doctest::Approx(0.5f));
    CHECK(s.styles[2].green == 100);
    CHECK(s.styles[2].alpha == 0); // kept as a byte
    const auto& trivial = s.styles[0]; // no section: the defaults
    CHECK(trivial.alpha == 0xc0);
    CHECK(trivial.red == 0xff);
    CHECK(trivial.floor_offset == doctest::Approx(1.0f));
}

TEST_CASE("target indicator: the ring's size") {
    CHECK(game::target_ring_radius(3.75f, 1.2f, true) == doctest::Approx(4.5f));
    CHECK(game::target_ring_radius(3.75f, 1.2f, false) == doctest::Approx(3.75f));
    CHECK(game::target_ring_radius(1.0f, 1.0f, true) == doctest::Approx(2.0f));
}

TEST_CASE("target indicator: the player's cap on the ring's size") {
    CHECK(game::cap_ring_radius(55.0f, game::kTargetRingMaxSizeOff) == doctest::Approx(55.0f)); // off: the original's
    CHECK(game::cap_ring_radius(55.0f, 12) == doctest::Approx(12.0f));
    CHECK(game::cap_ring_radius(6.0f, 12) == doctest::Approx(6.0f)); // a smaller ring is left alone
    CHECK(game::cap_ring_radius(55.0f, 1) == doctest::Approx(4.0f)); // below the range: its least
    CHECK(game::cap_ring_radius(55.0f, 41) == doctest::Approx(55.0f)); // past the most: off
    CHECK(game::clamp_ring_max_size(-3) == game::kTargetRingMaxSizeOff);
    CHECK(game::clamp_ring_max_size(2) == game::kTargetRingMaxSizeMin);
    CHECK(game::clamp_ring_max_size(20) == 20);
    CHECK(game::clamp_ring_max_size(game::kTargetRingMaxSizeMax + 1) == game::kTargetRingMaxSizeOff);
}

TEST_CASE("target indicator: Hailstone's look, the band's width follows the radius within bounds") {
    CHECK(game::target_ring_width(2.0f) == doctest::Approx(0.15f));  // small: kept readable
    CHECK(game::target_ring_width(5.0f) == doctest::Approx(0.3f));
    CHECK(game::target_ring_width(40.0f) == doctest::Approx(0.5f));  // a dragon's: not a stripe
}

TEST_CASE("target indicator: the cues, a sweep on a new target and a pulse while attacking") {
    auto idle = game::target_ring_look(4.0f, 10.0, 5.0, false, true);
    CHECK(idle.pulse == 0.0f);
    CHECK(idle.flash == 0.0f);
    CHECK(game::target_ring_spin_rate(false, true) == doctest::Approx(game::kTargetRingSpinIdle));

    auto fresh = game::target_ring_look(4.0f, 10.0, 0.0, false, true);
    CHECK(fresh.flash == doctest::Approx(1.0f));
    auto half = game::target_ring_look(4.0f, 10.0, game::kTargetRingFlashSeconds / 2.0, false, true);
    CHECK(half.flash == doctest::Approx(0.5f));
    CHECK(game::target_ring_look(4.0f, 10.0, game::kTargetRingFlashSeconds, false, true).flash == 0.0f);
    CHECK(game::target_ring_look(4.0f, 10.0, -1.0, false, true).flash == 0.0f); // unknown: no sweep

    // The pulse: down at a whole beat, up half a beat later.
    const double beat = 1.0 / game::kTargetRingPulseHz;
    CHECK(game::target_ring_look(4.0f, 8.0 * beat, 5.0, true, true).pulse == doctest::Approx(0.0f).epsilon(1e-3));
    CHECK(game::target_ring_look(4.0f, 8.5 * beat, 5.0, true, true).pulse == doctest::Approx(1.0f).epsilon(1e-3));
    CHECK(game::target_ring_spin_rate(true, true) == doctest::Approx(game::kTargetRingSpinAttack));
}

TEST_CASE("target indicator: with the cues off the ring keeps still") {
    auto look = game::target_ring_look(4.0f, 8.5 / game::kTargetRingPulseHz, 0.0, true, false);
    CHECK(look.pulse == 0.0f);
    CHECK(look.flash == 0.0f);
    CHECK(look.width == doctest::Approx(game::target_ring_width(4.0f)));
    CHECK(game::target_ring_spin_rate(true, false) == doctest::Approx(game::kTargetRingSpinIdle));
}

TEST_CASE("target indicator: the player's ring colours, read and written as RRGGBB") {
    CHECK(game::parse_ring_rgb("ff8000") == 0xff8000u);
    CHECK(game::parse_ring_rgb("#00A0ff") == 0x00a0ffu);
    CHECK_FALSE(game::parse_ring_rgb(""));
    CHECK_FALSE(game::parse_ring_rgb("fff"));
    CHECK_FALSE(game::parse_ring_rgb("12345g"));
    CHECK_FALSE(game::parse_ring_rgb("-12345"));
    CHECK(game::format_ring_rgb(0x00a0ffu) == "00a0ff");
    CHECK(game::format_ring_rgb(0xff123456u) == "123456"); // the alpha byte dropped
}

TEST_CASE("target indicator: the player's colour replaces the ini's, the opacity scales its alpha") {
    game::TargetRingStyle ini;
    ini.alpha = 0xc0;
    ini.red = 0x10;
    ini.green = 0x20;
    ini.blue = 0x30;
    game::TargetRingColors colors;
    auto same = game::apply_ring_colors(ini, 3, colors);
    CHECK(same.red == 0x10);
    CHECK(same.alpha == 0xc0);

    colors.rgb[3] = 0xff8000u;
    auto mine = game::apply_ring_colors(ini, 3, colors);
    CHECK(mine.red == 0xff);
    CHECK(mine.green == 0x80);
    CHECK(mine.blue == 0x00);
    CHECK(game::apply_ring_colors(ini, 4, colors).red == 0x10); // another section keeps the ini's
    CHECK(game::apply_ring_colors(ini, 7, colors).red == 0x10); // the free target's sections are not the player's

    colors.opacity = 50;
    CHECK(game::apply_ring_colors(ini, 3, colors).alpha == 0x60);
    colors.opacity = 200;
    CHECK(game::apply_ring_colors(ini, 3, colors).alpha == 255); // at most opaque
    colors.opacity = 0;                                          // below the range: its least
    CHECK(game::apply_ring_colors(ini, 3, colors).alpha == 48);
}

TEST_CASE("target indicator: the glow") {
    CHECK(game::target_ring_core_boost(0) == 0.0f);
    CHECK(game::target_ring_core_boost(game::kTargetRingGlowDefault) == doctest::Approx(0.7f));
    CHECK(game::target_ring_core_boost(100) == doctest::Approx(1.4f));
    CHECK(game::target_ring_core_boost(500) == doctest::Approx(1.4f));
}
