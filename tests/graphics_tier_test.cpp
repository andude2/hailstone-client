#include <doctest/doctest.h>

#include "core/config.h"
#include "render/graphics_tier.h"

#include <cstdint>
#include <string>

using namespace hailstone;
using namespace hailstone::render;

namespace {

constexpr std::uint64_t kGiB = 1024ull * 1024ull * 1024ull;

GpuProfile gpu(GpuKind kind, std::uint64_t memory, unsigned threads = 16, std::uint32_t vendor = 0x10de) {
    GpuProfile p;
    p.name = "test GPU";
    p.kind = kind;
    p.device_local_bytes = memory;
    p.cpu_threads = threads;
    p.vendor_id = vendor;
    return p;
}

} // namespace

TEST_CASE("graphics tier: a discrete GPU by its own memory") {
    CHECK(detect_graphics_tier(gpu(GpuKind::Discrete, 8 * kGiB)) == GraphicsTier::High);
    CHECK(detect_graphics_tier(gpu(GpuKind::Discrete, 6 * kGiB)) == GraphicsTier::High);
    CHECK(detect_graphics_tier(gpu(GpuKind::Discrete, 6 * kGiB - 1)) == GraphicsTier::Medium);
    CHECK(detect_graphics_tier(gpu(GpuKind::Discrete, 4 * kGiB)) == GraphicsTier::Medium);
    CHECK(detect_graphics_tier(gpu(GpuKind::Discrete, 3 * kGiB)) == GraphicsTier::Medium);
    CHECK(detect_graphics_tier(gpu(GpuKind::Discrete, 2 * kGiB)) == GraphicsTier::Low);
}

TEST_CASE("graphics tier: integrated GPUs are Low but Apple's, anything else Low") {
    // An integrated GPU's heap is the system's memory, however large: it says nothing of its speed.
    CHECK(detect_graphics_tier(gpu(GpuKind::Integrated, 16 * kGiB, 16, 0x8086)) == GraphicsTier::Low);
    CHECK(detect_graphics_tier(gpu(GpuKind::Integrated, 16 * kGiB, 16, 0x1002)) == GraphicsTier::Low);
    CHECK(detect_graphics_tier(gpu(GpuKind::Integrated, 16 * kGiB, 8, kVendorApple)) == GraphicsTier::Medium);
    CHECK(detect_graphics_tier(gpu(GpuKind::Other, 16 * kGiB)) == GraphicsTier::Low); // llvmpipe, a virtual GPU
}

TEST_CASE("graphics tier: a small CPU caps the tier") {
    CHECK(detect_graphics_tier(gpu(GpuKind::Discrete, 12 * kGiB, 2)) == GraphicsTier::Low);
    CHECK(detect_graphics_tier(gpu(GpuKind::Discrete, 12 * kGiB, 3)) == GraphicsTier::Low);
    CHECK(detect_graphics_tier(gpu(GpuKind::Discrete, 12 * kGiB, 4)) == GraphicsTier::Medium);
    CHECK(detect_graphics_tier(gpu(GpuKind::Discrete, 12 * kGiB, 5)) == GraphicsTier::Medium);
    CHECK(detect_graphics_tier(gpu(GpuKind::Discrete, 12 * kGiB, 6)) == GraphicsTier::High);
    CHECK(detect_graphics_tier(gpu(GpuKind::Discrete, 2 * kGiB, 4)) == GraphicsTier::Low); // never raised
    CHECK(detect_graphics_tier(gpu(GpuKind::Discrete, 12 * kGiB, 0)) == GraphicsTier::High); // unknown: no cap
}

TEST_CASE("graphics tier: names round-trip") {
    for (GraphicsTier t : {GraphicsTier::Low, GraphicsTier::Medium, GraphicsTier::High})
        CHECK(parse_graphics_tier(graphics_tier_name(t)) == t);
    CHECK_FALSE(parse_graphics_tier("ultra"));
}

TEST_CASE("graphics tier: every tier decides the same keys, and High leaves each to its default") {
    const auto low = graphics_tier_settings(GraphicsTier::Low);
    const auto medium = graphics_tier_settings(GraphicsTier::Medium);
    const auto high = graphics_tier_settings(GraphicsTier::High);
    REQUIRE(low.size() == medium.size());
    REQUIRE(low.size() == high.size());
    for (std::size_t i = 0; i < low.size(); ++i) {
        CHECK(low[i].key == medium[i].key);
        CHECK(low[i].key == high[i].key);
        CHECK_FALSE(low[i].keep); // Low decides every key: set, or its default
        CHECK(high[i].value.empty());
    }
}

TEST_CASE("graphics tier: Low and Medium match the Options window's presets") {
    // ui/modern/options.lua PRESETS, as display_option reads them back: shadow size, samples, AO, blur, view distance.
    Config c;
    apply_graphics_tier(c, GraphicsTier::Low);
    CHECK(c.get_int("display.shadows", -1) == 0);
    CHECK(c.get_int("display.msaa", -1) == 1);
    CHECK_FALSE(c.get_bool("display.ao", true));
    CHECK_FALSE(c.get_bool("display.distance_blur", true));
    CHECK(c.get_float("display.view_distance", 0) == doctest::Approx(1.5f));
    CHECK_FALSE(c.get("display.render_scale")); // full size: the upscaler is off

    apply_graphics_tier(c, GraphicsTier::Medium);
    CHECK(c.get_int("display.shadows", -1) == 1024);
    CHECK(c.get_int("display.msaa", -1) == 2);
    CHECK_FALSE(c.get_bool("display.ao", true));
    CHECK_FALSE(c.get("display.distance_blur")); // its default, on
    CHECK(c.get_float("display.view_distance", 0) == doctest::Approx(2.0f));

    apply_graphics_tier(c, GraphicsTier::High);
    for (const TierSetting& s : graphics_tier_settings(GraphicsTier::High))
        if (!s.keep) CHECK_FALSE(c.get(s.key)); // the kept ones (upscaler, frame generation, lighting) stay as Low left them
}

TEST_CASE("graphics tier: Low turns upscaling and frame generation off; Medium and High keep the player's") {
    Config c;
    c.set("display.upscaler", "fsr3");
    c.set("display.frame_gen", "1");
    apply_graphics_tier(c, GraphicsTier::High);
    CHECK(c.get_string("display.upscaler") == "fsr3");
    CHECK(c.get_string("display.frame_gen") == "1");
    apply_graphics_tier(c, GraphicsTier::Medium);
    CHECK(c.get_string("display.upscaler") == "fsr3");
    CHECK(c.get_string("display.frame_gen") == "1");
    apply_graphics_tier(c, GraphicsTier::Low);
    CHECK(c.get_string("display.upscaler") == "off");
    CHECK_FALSE(c.get_bool("display.frame_gen", true));
}

TEST_CASE("graphics tier: Low draws with the classic lighting and no radial flora; Medium and High keep the player's") {
    Config c;
    apply_graphics_tier(c, GraphicsTier::Low);
    CHECK(c.get_string("display.lighting") == "classic");
    CHECK_FALSE(c.get_bool("display.grass", true)); // radial flora off
    apply_graphics_tier(c, GraphicsTier::Medium);
    CHECK(c.get_string("display.lighting") == "classic"); // not forced back to modern
    c.set("display.lighting", "modern");
    apply_graphics_tier(c, GraphicsTier::High);
    CHECK(c.get_string("display.lighting") == "modern");
}

TEST_CASE("graphics tier: the first start applies the tier to a new install only, once") {
    SUBCASE("a new install") {
        Config c;
        c.set("assets.path", "/games/eq"); // the asset picker's, before the settings are read
        auto applied = first_start_graphics(c, gpu(GpuKind::Integrated, 16 * kGiB, 8, 0x8086));
        REQUIRE(applied);
        CHECK(*applied == GraphicsTier::Low);
        CHECK(c.get_string(kGraphicsTierKey) == "low");
        CHECK(c.get_int("display.shadows", -1) == 0);
        // Once: a later start leaves what the player changed since.
        c.set("display.shadows", "4096");
        CHECK_FALSE(first_start_graphics(c, gpu(GpuKind::Integrated, 16 * kGiB, 8, 0x8086)));
        CHECK(c.get_int("display.shadows", -1) == 4096);
    }
    SUBCASE("an existing install keeps its settings, and the tier is recorded") {
        Config c;
        c.set("display.lighting", "modern");
        CHECK_FALSE(first_start_graphics(c, gpu(GpuKind::Discrete, 2 * kGiB)));
        CHECK(c.get_string(kGraphicsTierKey) == "low");
        CHECK_FALSE(c.get("display.shadows"));
    }
    SUBCASE("a High computer writes nothing but the tier") {
        Config c;
        REQUIRE(first_start_graphics(c, gpu(GpuKind::Discrete, 12 * kGiB)));
        const auto entries = c.entries();
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].first == kGraphicsTierKey);
        CHECK(entries[0].second == "high");
    }
}

TEST_CASE("graphics tier: the tier key is shared, kept in hailstone.ini with a character's settings") {
    CHECK(Config::is_shared_key(kGraphicsTierKey));
}
