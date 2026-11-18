// render/horizon.h: the land's generated terrain (horizon_terrain), its wear held off the known ground, its coast, and
// its cache.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

#include "render/horizon.h"

using namespace hailstone;
using render::HorizonTriangle;

namespace {

void add_square(std::vector<HorizonTriangle>& out, float x0, float y0, float x1, float y1, float z, bool liquid = false) {
    const glm::vec3 a(x0, y0, z), b(x1, y0, z), c(x1, y1, z), d(x0, y1, z);
    out.push_back({a, b, c, liquid});
    out.push_back({a, c, d, liquid});
}

template <typename Height>
void add_grid(std::vector<HorizonTriangle>& out, float x0, float y0, float x1, float y1, float cell, Height height,
              bool liquid = false) {
    for (float x = x0; x < x1; x += cell)
        for (float y = y0; y < y1; y += cell) add_square(out, x, y, x + cell, y + cell, height(x, y), liquid);
}

// A flat zone (0..2000 x 0..2000 at 20) with a sea along its west (its surface at 0).
struct Coast {
    std::vector<HorizonTriangle> tris;
    render::HorizonField field;
};

Coast make_coast(float seed = 4.0f) {
    Coast c;
    add_grid(c.tris, 0.0f, 0.0f, 2000.0f, 2000.0f, 100.0f, [](float, float) { return 20.0f; });
    add_grid(c.tris, -1000.0f, 0.0f, 0.0f, 2000.0f, 100.0f, [](float, float) { return 0.0f; }, true);
    c.field = render::horizon_field(c.tris, render::horizon_land(c.tris, seed));
    return c;
}

} // namespace

TEST_CASE("horizon terrain: the same land wears the same way, another zone's otherwise") {
    const Coast c = make_coast();
    REQUIRE(c.field.valid());
    const auto a = render::horizon_terrain(c.field);
    const auto b = render::horizon_terrain(c.field);
    REQUIRE(a.valid());
    REQUIRE(b.valid());
    CHECK(a.nx == b.nx);
    CHECK(a.ny == b.ny);
    CHECK(a.delta == b.delta);
    CHECK(a.wet == b.wet);
    // The wear moves the free land, within its bounds.
    CHECK(a.worn_lo < -1.0f);
    CHECK(a.worn_lo >= -render::kHorizonTerrainMaxCut - 0.5f);
    CHECK(a.worn_hi <= render::kHorizonTerrainMaxFill + 0.5f);
    const Coast other = make_coast(9.0f);
    CHECK(render::horizon_terrain(other.field).delta != a.delta);
}

TEST_CASE("horizon terrain: the known ground is not moved: the zone, the sea") {
    const Coast c = make_coast();
    auto field = c.field;
    const auto t = render::horizon_terrain(field);
    REQUIRE(t.valid());
    std::size_t zone = 0, sea = 0;
    for (int y = 0; y < t.ny; ++y)
        for (int x = 0; x < t.nx; ++x) {
            const glm::vec2 p = t.lo + glm::vec2(static_cast<float>(x), static_cast<float>(y)) * t.cell;
            const float d = t.delta[static_cast<std::size_t>(y) * static_cast<std::size_t>(t.nx) + static_cast<std::size_t>(x)];
            if (p.x > 100.0f && p.x < 1900.0f && p.y > 100.0f && p.y < 1900.0f) {
                CHECK(d == 0.0f);
                ++zone;
            }
            // Straight off the west shore, the open sea.
            if (p.x < -2500.0f && p.y > 300.0f && p.y < 1700.0f) {
                CHECK(std::abs(d) < 0.01f);
                ++sea;
            }
        }
    CHECK(zone > 50);
    CHECK(sea > 50);
    // The land's own sea floor is where it was, under the surface.
    field.terrain = std::make_shared<const render::HorizonTerrain>(t);
    CHECK(render::horizon_height(field, {-4000.0f, 1000.0f}) < -100.0f);
}

TEST_CASE("horizon terrain: the land's sea follows the ground, not the squares") {
    Coast c = make_coast();
    c.field.terrain = std::make_shared<const render::HorizonTerrain>(render::horizon_terrain(c.field));
    const auto mesh = render::horizon_mesh(c.field);
    REQUIRE_FALSE(mesh.water.empty());
    // Some water squares have a corner whose ground stands above the surface: the land rises through the water there,
    // so the shoreline runs where the ground crosses the surface.
    const float level = mesh.water_level;
    std::size_t through = 0;
    for (std::size_t i = 0; i + 5 < mesh.water.size(); i += 6) {
        bool above = false;
        for (std::size_t k = 0; k < 6; ++k) {
            const glm::vec3 w = mesh.positions[mesh.water[i + k]];
            if (const auto z = render::horizon_surface(c.field, mesh, glm::vec2(w))) above |= *z > level + 1.0f;
        }
        through += above ? 1 : 0;
    }
    CHECK(through > 10);
    // The ground and rock are one draw, splatted by the vertices' weights.
    REQUIRE(mesh.splat.size() == mesh.positions.size());
    bool rock = false, sand = false;
    for (const glm::vec2& s : mesh.splat) {
        CHECK(s.x >= 0.0f);
        CHECK(s.x <= 1.0f);
        CHECK(s.y >= 0.0f);
        CHECK(s.y <= 1.0f);
        rock |= s.x > 0.5f;
        sand |= s.y > 0.5f;
    }
    CHECK(rock);
    CHECK(sand);
}

TEST_CASE("horizon terrain: cached and read back for the same land; another land's or a damaged file is refused") {
    const Coast c = make_coast();
    const auto t = render::horizon_terrain(c.field);
    REQUIRE(t.valid());
    const std::uint64_t key = render::horizon_terrain_key(c.field);
    const auto bytes = render::write_horizon_terrain(t, key);
    const auto back = render::read_horizon_terrain(bytes, key);
    REQUIRE(back);
    CHECK(back->nx == t.nx);
    CHECK(back->ny == t.ny);
    CHECK(back->lo == t.lo);
    CHECK(back->cell == t.cell);
    CHECK(back->delta == t.delta); // stored as generated (both already on the cache's steps)
    CHECK(back->wet == t.wet);
    // Another land (another seed) has another key, and its file does not fit this one.
    const Coast other = make_coast(9.0f);
    CHECK(render::horizon_terrain_key(other.field) != key);
    CHECK_FALSE(render::read_horizon_terrain(bytes, key + 1));
    // Damaged: cut short anywhere, or with its sizes changed.
    for (std::size_t cut : {std::size_t{0}, std::size_t{3}, std::size_t{20}, bytes.size() / 2, bytes.size() - 1})
        CHECK_FALSE(render::read_horizon_terrain(std::span(bytes).first(cut), key));
    auto bent = bytes;
    bent[4] = std::byte{0x7f}; // the version
    CHECK_FALSE(render::read_horizon_terrain(bent, key));
}
