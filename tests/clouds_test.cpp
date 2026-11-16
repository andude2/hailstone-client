#include <doctest/doctest.h>

#include <cmath>
#include <set>

#include "render/clouds.h"

using namespace hailstone::render;

TEST_CASE("Volumetric clouds: the cover by sky and storm") {
    const CloudCover normal = cloud_cover(1, 0.0f);
    const CloudCover desert = cloud_cover(2, 0.0f);
    const CloudCover cottony = cloud_cover(4, 0.0f);
    // A few clouds over the desert, more over the normal sky, most over the cottony one; none overcast.
    CHECK(desert.coverage < normal.coverage);
    CHECK(normal.coverage < cottony.coverage);
    CHECK(cottony.coverage < 1.0f);
    CHECK(normal.darkness == 0.0f);
    // Any other sky index is taken as the normal one.
    CHECK(cloud_cover(7, 0.0f).coverage == normal.coverage);

    // A full storm closes every sky over, thicker, flatter and dark at its base, whatever it was before.
    for (int sky : {1, 2, 4}) {
        const CloudCover storm = cloud_cover(sky, 1.0f);
        CHECK(storm.coverage == doctest::Approx(1.0f));
        CHECK(storm.density > cloud_cover(sky, 0.0f).density);
        CHECK(storm.type < cloud_cover(sky, 0.0f).type);
        CHECK(storm.darkness > 0.5f);
    }
    // In between it eases, and out of range it is held to 0..1.
    const CloudCover half = cloud_cover(1, 0.5f);
    CHECK(half.coverage > normal.coverage);
    CHECK(half.coverage < 1.0f);
    CHECK(cloud_cover(1, 3.0f).coverage == doctest::Approx(1.0f));
    CHECK(cloud_cover(1, -1.0f).coverage == normal.coverage);
}

TEST_CASE("Volumetric clouds: half the scene's size, rounded up") {
    CHECK(cloud_extent(1920, 1080) == glm::uvec2(960, 540));
    CHECK(cloud_extent(1281, 721) == glm::uvec2(641, 361));
    CHECK(cloud_extent(1, 1) == glm::uvec2(1, 1));
    CHECK(cloud_extent(0, 0) == glm::uvec2(1, 1));
}

TEST_CASE("Volumetric clouds: every pixel of a 2x2 block is marched once in four frames") {
    std::set<std::uint32_t> seen;
    for (std::uint64_t f = 0; f < 4; ++f) seen.insert(cloud_update_slot(f));
    CHECK(seen == std::set<std::uint32_t>{0, 1, 2, 3});
    for (std::uint64_t f = 0; f < 64; ++f) CHECK(cloud_update_slot(f) == cloud_update_slot(f + 4));
    // The Bayer order: the first two diagonal to each other.
    CHECK(cloud_update_slot(0) == 0);
    CHECK(cloud_update_slot(1) == 3);
}

TEST_CASE("Volumetric clouds: the history is kept while the sky moves slowly") {
    CloudHistoryKey last{glm::normalize(glm::vec3(0.3f, -0.5f, 0.8f)), 0.2f, glm::vec2(10.0f, -4.0f)};
    CloudHistoryKey now = last;
    CHECK_FALSE(cloud_history_stale(last, now));
    // A frame's worth of wind and storm.
    now.wind += cloud_wind_step(1.0f / 60.0f, now.storm);
    now.storm += 0.001f;
    CHECK_FALSE(cloud_history_stale(last, now));
    // The clock jumping (the sun turned by a tenth of a radian), the storm jumping, the wind wrapping round.
    CloudHistoryKey jump = last;
    jump.to_light = glm::normalize(glm::vec3(0.3f, -0.5f, 0.8f) + glm::vec3(0.0f, 0.1f, 0.0f));
    CHECK(cloud_history_stale(last, jump));
    jump = last;
    jump.storm = 0.5f;
    CHECK(cloud_history_stale(last, jump));
    jump = last;
    jump.wind = glm::vec2(600.0f, 0.0f);
    CHECK(cloud_history_stale(last, jump));
    // A broken light direction is a jump, not kept history.
    jump = last;
    jump.to_light = glm::vec3(std::nanf(""));
    CHECK(cloud_history_stale(last, jump));
}

TEST_CASE("Volumetric clouds: the wind drifts east, faster in a storm, and never jumps") {
    const glm::vec2 fair = cloud_wind_step(1.0f, 0.0f);
    CHECK(fair.y < 0.0f); // east is -Y
    CHECK(glm::length(fair) == doctest::Approx(kCloudWindSpeed));
    CHECK(glm::length(cloud_wind_step(1.0f, 1.0f)) == doctest::Approx(2.0f * kCloudWindSpeed));
    // A long stall moves it no more than a second's worth; a clock going back, or none, not at all.
    CHECK(glm::length(cloud_wind_step(30.0f, 0.0f)) == doctest::Approx(kCloudWindSpeed));
    CHECK(cloud_wind_step(-1.0f, 0.0f) == glm::vec2(0.0f));
    CHECK(cloud_wind_step(std::nanf(""), 0.0f) == glm::vec2(0.0f));
}
