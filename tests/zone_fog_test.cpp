// render/zone_fog.h: the fog bank past the zone's edge, from the land's field.
#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "render/horizon.h"
#include "render/zone_fog.h"

using namespace hailstone;
using render::HorizonTriangle;

namespace {

// Level squares `cell` across over x0..x1, y0..y1 at height z, skipping those `hole` says.
template <typename Hole>
void add_ground(std::vector<HorizonTriangle>& out, float x0, float y0, float x1, float y1, float z, Hole hole) {
    constexpr float cell = 100.0f;
    for (float x = x0; x < x1; x += cell)
        for (float y = y0; y < y1; y += cell) {
            if (hole(x, y)) continue;
            glm::vec3 a(x, y, z), b(x + cell, y, z), c(x + cell, y + cell, z), d(x, y + cell, z);
            out.push_back({a, b, c, false});
            out.push_back({a, c, d, false});
        }
}

render::ZoneFogField fog_of(const std::vector<HorizonTriangle>& tris) {
    return render::zone_fog_field(render::horizon_field(tris, render::horizon_land(tris, 0.0f)));
}

// A level zone 2000 across at height z.
render::ZoneFogField level_zone(float z) {
    std::vector<HorizonTriangle> tris;
    add_ground(tris, 0.0f, 0.0f, 2000.0f, 2000.0f, z, [](float, float) { return false; });
    return fog_of(tris);
}

} // namespace

TEST_CASE("zone fog: clear over the zone's own ground") {
    const auto fog = level_zone(20.0f);
    REQUIRE(fog.valid());
    CHECK(render::zone_fog_density(fog, {1000.0f, 1000.0f, 26.0f}) == 0.0f);
    CHECK(render::zone_fog_transmittance(fog, {200.0f, 1000.0f, 26.0f}, {1800.0f, 1000.0f, 26.0f}) == 1.0f);
}

TEST_CASE("zone fog: across a zone line at eye height nothing shows through it") {
    const auto fog = level_zone(20.0f);
    REQUIRE(fog.valid());
    // From near the east edge, looking 3000 units out over the land.
    CHECK(render::zone_fog_transmittance(fog, {1900.0f, 1000.0f, 26.0f}, {4900.0f, 1000.0f, 26.0f}) < 0.01f);
    // Just past the edge it is still light: it thickens over kZoneFogRamp.
    CHECK(render::zone_fog_transmittance(fog, {1900.0f, 1000.0f, 26.0f}, {2150.0f, 1000.0f, 26.0f}) > 0.9f);
    // Past the grid the distance goes on growing: full density far out.
    CHECK(render::zone_fog_density(fog, {9000.0f, 1000.0f, 20.0f}) == doctest::Approx(render::kZoneFogDensity));
}

TEST_CASE("zone fog: the sky above it stays clear") {
    const auto fog = level_zone(20.0f);
    REQUIRE(fog.valid());
    const glm::vec3 eye(1000.0f, 1000.0f, 26.0f);
    // 30 degrees up, out to the march's reach.
    const glm::vec3 up30 = eye + glm::vec3(std::cos(0.5236f), 0.0f, std::sin(0.5236f)) * render::kZoneFogReach;
    CHECK(render::zone_fog_transmittance(fog, eye, up30) > 0.9f);
    // Level, the horizon is fogged out.
    CHECK(render::zone_fog_transmittance(fog, eye, eye + glm::vec3(render::kZoneFogReach, 0.0f, 0.0f)) < 0.01f);
}

TEST_CASE("zone fog: its floor is the zone's edge, so a plateau's fog lies on the plateau") {
    const auto low = level_zone(20.0f), high = level_zone(800.0f);
    REQUIRE(low.valid());
    REQUIRE(high.valid());
    const glm::vec2 out(3500.0f, 1000.0f);
    CHECK(high.floor_at(out) == doctest::Approx(800.0f).epsilon(0.02));
    CHECK(render::zone_fog_density(high, {out.x, out.y, 820.0f}) ==
          doctest::Approx(render::zone_fog_density(low, {out.x, out.y, 40.0f})).epsilon(0.05));
    // Over its top there is none: a hill taller than the bank stands clear of it.
    CHECK(render::zone_fog_density(high, {out.x, out.y, 800.0f + render::kZoneFogTop + render::kZoneFogSoft}) == 0.0f);
    CHECK(render::zone_fog_density(high, {out.x, out.y, 800.0f + render::kZoneFogTop - render::kZoneFogSoft}) ==
          doctest::Approx(render::kZoneFogDensity));
}

TEST_CASE("zone fog: none in a pocket the zone's terrain closes round") {
    std::vector<HorizonTriangle> tris;
    // A ring of ground round a 600-wide hole in the middle.
    add_ground(tris, 0.0f, 0.0f, 3000.0f, 3000.0f, 20.0f,
               [](float x, float y) { return x >= 1200.0f && x < 1800.0f && y >= 1200.0f && y < 1800.0f; });
    const auto fog = fog_of(tris);
    REQUIRE(fog.valid());
    CHECK(fog.distance_at({1500.0f, 1500.0f}) == 0.0f);
    CHECK(render::zone_fog_density(fog, {1500.0f, 1500.0f, 0.0f}) == 0.0f);
}

TEST_CASE("zone fog: no field without the land's") {
    std::vector<HorizonTriangle> tris;
    add_ground(tris, 0.0f, 0.0f, 200.0f, 200.0f, 0.0f, [](float, float) { return false; }); // too little ground
    const auto fog = fog_of(tris);
    CHECK_FALSE(fog.valid());
    CHECK(render::zone_fog_transmittance(fog, {0.0f, 0.0f, 0.0f}, {10000.0f, 0.0f, 0.0f}) == 1.0f);
}

TEST_CASE("zone fog: a wall along the zone's edge does not lift the bank off the open ground") {
    std::vector<HorizonTriangle> tris;
    add_ground(tris, 0.0f, 0.0f, 2000.0f, 2000.0f, 20.0f, [](float, float) { return false; });
    // A wall 600 high standing on the east edge.
    for (float y = 0.0f; y < 2000.0f; y += 100.0f) {
        const glm::vec3 a(1990.0f, y, 20.0f), b(1990.0f, y + 100.0f, 20.0f), c(1990.0f, y + 100.0f, 620.0f),
            d(1990.0f, y, 620.0f);
        tris.push_back({a, b, c, false});
        tris.push_back({a, c, d, false});
    }
    const auto fog = fog_of(tris);
    REQUIRE(fog.valid());
    CHECK(fog.floor_at({3500.0f, 1000.0f}) < 60.0f);
    // So at eye height out past the wall it is thick, and over the wall's top it is clear.
    CHECK(render::zone_fog_density(fog, {3500.0f, 1000.0f, 26.0f}) == doctest::Approx(render::kZoneFogDensity));
    CHECK(render::zone_fog_density(fog, {3500.0f, 1000.0f, 640.0f}) == 0.0f);
}

TEST_CASE("zone fog: never round the camera, even where the field has it outside the zone") {
    const auto fog = level_zone(20.0f);
    REQUIRE(fog.valid());
    // A camera the field puts 400 units past the zone's terrain (ground the field does not count), on ground under
    // the fog's floor there.
    const glm::vec3 eye(2400.0f, 1000.0f, 0.0f);
    const float past = fog.distance_at(glm::vec2(eye));
    REQUIRE(past > 300.0f);
    CHECK(render::zone_fog_density(fog, eye + glm::vec3(100.0f, 0.0f, 0.0f), eye, past) == 0.0f);
    // The sky over it is clear, and across the ground ahead it still thickens.
    const glm::vec3 up30 = eye + glm::vec3(std::cos(0.5236f), 0.0f, std::sin(0.5236f)) * render::kZoneFogReach;
    CHECK(render::zone_fog_transmittance(fog, eye, up30) > 0.9f);
    CHECK(render::zone_fog_transmittance(fog, eye, eye + glm::vec3(4000.0f, 0.0f, 0.0f)) < 0.05f);
}
