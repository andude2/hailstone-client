// game/world_atlas.h: the atlas's records, and the walk that places the zones round the current one.
#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/byte_io.h"
#include "game/world_atlas.h"
#include "game/zone_names.h"

using namespace hailstone;
using game::AtlasVisit;
using game::AtlasZone;

namespace {

// Three zones in a row along +x, each 2000 x 2000 with flat ground at 0 (C's at 30 in its own coordinates, its east
// half under water at 40): A's line on its +x edge leads to B's -x edge, B's +x edge to C's -x edge. Each line is
// absolute (its tag names the zone and the arrival, server coordinates: x is world y), arriving 100 in from the edge.
// Real zone names, so their ids resolve: A qeytoqrg, B qeynos2, C qeynos.
const std::string kA = "qeytoqrg", kB = "qeynos2", kC = "qeynos";

game::ZoneLineCell line(float x0, float x1, const std::string& to, float arrive_x, std::uint32_t region) {
    game::ZoneLineCell c;
    c.region = region;
    c.lo = {x0, 500.0f, -50.0f};
    c.hi = {x1, 1500.0f, 150.0f};
    c.zone_id = game::zone_id_by_short_name(to);
    c.server_x = 1000.0f;   // world y
    c.server_y = arrive_x;  // world x
    c.server_z = 10.0f;
    c.tag = "line to " + to;
    return c;
}

AtlasZone zone(const std::string& name, float ground, float water_from = 1e9f) {
    AtlasZone z;
    z.zone = name;
    z.key = 42;
    z.lo = {0.0f, 0.0f, -100.0f};
    z.hi = {2000.0f, 2000.0f, 500.0f};
    z.grid_lo = {0.0f, 0.0f};
    z.nx = z.ny = 32; // 2048 units
    const auto n = static_cast<std::size_t>(z.nx * z.ny);
    z.ground.assign(n, ground);
    z.top.assign(n, ground + 5.0f);
    z.water.assign(n, std::nanf(""));
    z.color.assign(n, 0xff336699u);
    z.trees.assign(n, 0);
    if (n > 3) z.trees[3] = 7;
    for (int y = 0; y < z.ny; ++y)
        for (int x = 0; x < z.nx; ++x)
            if ((static_cast<float>(x) + 0.5f) * z.cell >= water_from)
                z.water[static_cast<std::size_t>(y * z.nx + x)] = ground + 10.0f;
    return z;
}

struct World {
    std::map<std::string, AtlasZone> zones;
    std::map<std::string, AtlasVisit> visits;
    std::map<std::string, glm::vec3> measured;
    game::AtlasLookup lookup() const {
        game::AtlasLookup l;
        l.zone = [this](const std::string& n) -> const AtlasZone* {
            auto it = zones.find(n);
            return it == zones.end() ? nullptr : &it->second;
        };
        l.visit = [this](const std::string& n) -> const AtlasVisit* {
            auto it = visits.find(n);
            return it == visits.end() ? nullptr : &it->second;
        };
        l.measured = [this](const std::string& n) -> std::optional<glm::vec3> {
            auto it = measured.find(n);
            if (it == measured.end()) return std::nullopt;
            return it->second;
        };
        return l;
    }
};

World make_world() {
    World w;
    AtlasZone a = zone(kA, 0.0f), b = zone(kB, 0.0f), c = zone(kC, 30.0f, 1000.0f);
    a.lines.push_back(line(1950.0f, 2000.0f, kB, 100.0f, 1));
    b.lines.push_back(line(0.0f, 50.0f, kA, 1900.0f, 1));
    b.lines.push_back(line(1950.0f, 2000.0f, kC, 100.0f, 2));
    c.lines.push_back(line(0.0f, 50.0f, kB, 1900.0f, 1));
    for (auto* z : {&a, &b, &c}) {
        AtlasVisit v;
        v.zone = z->zone;
        v.zone_id = game::zone_id_by_short_name(z->zone);
        v.outdoor = true;
        w.visits[z->zone] = v;
        w.zones[z->zone] = std::move(*z);
    }
    return w;
}

const game::AtlasPlacement* find(const game::AtlasPlan& plan, const std::string& name) {
    auto it = std::find_if(plan.zones.begin(), plan.zones.end(), [&](const auto& p) { return p.zone == name; });
    return it == plan.zones.end() ? nullptr : &*it;
}

} // namespace

TEST_CASE("world atlas: a visit is written and read back; a damaged one is refused") {
    AtlasVisit v;
    v.zone = "qeytoqrg";
    v.zone_id = 4;
    v.outdoor = true;
    v.points = {{1, 2, 10.0f, 20.0f, 999999.0f}, {7, 1, -5.5f, 3.25f, 1.0f}};
    const auto bytes = game::write_atlas_visit(v);
    const auto back = game::read_atlas_visit(bytes);
    REQUIRE(back);
    CHECK(back->zone == v.zone);
    CHECK(back->zone_id == 4);
    CHECK(back->outdoor);
    REQUIRE(back->points.size() == 2);
    CHECK(back->points[1].number == 7);
    CHECK(back->points[1].zone_id == 1);
    CHECK(back->points[1].server_x == -5.5f);
    CHECK(back->points[0].server_z == 999999.0f);
    CHECK_FALSE(game::read_atlas_visit(std::span(bytes).first(bytes.size() - 1)));
    auto longer = bytes;
    longer.push_back(std::byte{0});
    CHECK_FALSE(game::read_atlas_visit(longer));
    auto other = bytes;
    other[4] = std::byte{9};
    CHECK_FALSE(game::read_atlas_visit(other));
    CHECK_FALSE(game::read_atlas_visit({}));
}

TEST_CASE("world atlas: a zone's record is written and read back; a damaged one is refused") {
    const World w = make_world();
    AtlasZone c = w.zones.at(kC);
    c.open_sky = 0.75f;
    const auto bytes = game::write_atlas_zone(c);
    const auto back = game::read_atlas_zone(bytes);
    REQUIRE(back);
    REQUIRE(back->valid());
    CHECK(back->zone == kC);
    CHECK(back->key == 42);
    CHECK(back->lo == c.lo);
    CHECK(back->hi == c.hi);
    CHECK(back->nx == 32);
    CHECK(back->open_sky == 0.75f);
    CHECK(back->ground == c.ground);
    CHECK(back->color == c.color);
    CHECK(back->trees == c.trees);
    for (std::size_t i = 0; i < c.water.size(); ++i) CHECK(std::isnan(back->water[i]) == std::isnan(c.water[i]));
    REQUIRE(back->lines.size() == 1);
    CHECK(back->lines[0].zone_id == c.lines[0].zone_id);
    CHECK(back->lines[0].tag == c.lines[0].tag);
    CHECK(back->lines[0].hi == c.lines[0].hi);
    CHECK(back->ground_at({1000.0f, 1000.0f}) == doctest::Approx(30.0f));
    CHECK_FALSE(back->ground_at({-10.0f, 1000.0f}));
    CHECK_FALSE(game::read_atlas_zone(std::span(bytes).first(bytes.size() - 3)));
    auto other = bytes;
    other[0] = std::byte{0};
    CHECK_FALSE(game::read_atlas_zone(other));
}

TEST_CASE("world atlas: the zones are placed out from the current one, line by line") {
    const World w = make_world();
    const auto plan = game::place_atlas(kA, w.lookup(), 12000.0f, 4, 16);
    REQUIRE(plan.zones.size() == 3);
    CHECK(plan.zones[0].zone == kA);
    CHECK(plan.zones[0].hop == 0);
    const auto* b = find(plan, kB);
    const auto* c = find(plan, kC);
    REQUIRE(b);
    REQUIRE(c);
    CHECK(b->hop == 1);
    CHECK(c->hop == 2);
    CHECK(c->parent == kB);
    // A's line face (1950) and B's line back (its inner face, 50) line up; the ground (0 both sides) sets z.
    CHECK(b->offset.x == doctest::Approx(1900.0f));
    CHECK(b->offset.y == doctest::Approx(0.0f));
    CHECK(b->offset.z == doctest::Approx(0.0f));
    // C: B's offset and B's own to C; z from the ground at B's line (0 there, 30 past it).
    CHECK(c->offset.x == doctest::Approx(3800.0f));
    CHECK(c->offset.z == doctest::Approx(-30.0f));
    // C's seam is B's line, in A's coordinates.
    CHECK(c->seam.face == doctest::Approx(1950.0f + 1900.0f));
    CHECK(plan.missing.empty());
}

TEST_CASE("world atlas: a measured neighbour's offset wins over the atlas's own") {
    World w = make_world();
    w.measured[kB] = {1890.0f, 5.0f, -2.0f};
    const auto plan = game::place_atlas(kA, w.lookup(), 12000.0f, 4, 16);
    const auto* b = find(plan, kB);
    const auto* c = find(plan, kC);
    REQUIRE(b);
    REQUIRE(c);
    CHECK(b->offset == glm::vec3(1890.0f, 5.0f, -2.0f));
    CHECK(c->offset.x == doctest::Approx(1890.0f + 1900.0f));
}

TEST_CASE("world atlas: only visited, outdoor zones within reach join; one not measured yet is asked for") {
    SUBCASE("out of reach") {
        const World w = make_world();
        const auto plan = game::place_atlas(kA, w.lookup(), 1000.0f, 4, 16);
        CHECK(find(plan, kB));
        CHECK_FALSE(find(plan, kC));
    }
    SUBCASE("hops") {
        const World w = make_world();
        const auto plan = game::place_atlas(kA, w.lookup(), 12000.0f, 1, 16);
        CHECK(find(plan, kB));
        CHECK_FALSE(find(plan, kC));
    }
    SUBCASE("indoor") {
        World w = make_world();
        w.visits[kC].outdoor = false;
        const auto plan = game::place_atlas(kA, w.lookup(), 12000.0f, 4, 16);
        CHECK_FALSE(find(plan, kC));
    }
    SUBCASE("under a roof") {
        World w = make_world();
        w.zones[kC].open_sky = 0.2f;
        const auto plan = game::place_atlas(kA, w.lookup(), 12000.0f, 4, 16);
        CHECK_FALSE(find(plan, kC));
        CHECK(find(plan, kB));
    }
    SUBCASE("not visited") {
        World w = make_world();
        w.visits.erase(kC);
        const auto plan = game::place_atlas(kA, w.lookup(), 12000.0f, 4, 16);
        CHECK_FALSE(find(plan, kC));
        CHECK(plan.missing.empty());
    }
    SUBCASE("not measured") {
        World w = make_world();
        w.zones.erase(kC);
        const auto plan = game::place_atlas(kA, w.lookup(), 12000.0f, 4, 16);
        CHECK_FALSE(find(plan, kC));
        REQUIRE(plan.missing.size() == 1);
        CHECK(plan.missing[0] == kC);
    }
    SUBCASE("the current zone not measured") {
        World w = make_world();
        w.zones.erase(kA);
        const auto plan = game::place_atlas(kA, w.lookup(), 12000.0f, 4, 16);
        CHECK(plan.zones.empty());
        REQUIRE(plan.missing.size() == 1);
        CHECK(plan.missing[0] == kA);
    }
    SUBCASE("the current zone not visited") {
        World w = make_world();
        w.visits.erase(kA);
        const auto plan = game::place_atlas(kA, w.lookup(), 12000.0f, 4, 16);
        CHECK(plan.zones.empty());
        CHECK(plan.missing.empty());
    }
}

TEST_CASE("world atlas: lines whose targets disagree are not followed") {
    World w = make_world();
    // A second line to B whose arrival is 1000 units off the first's.
    auto extra = line(1950.0f, 2000.0f, kB, 1100.0f, 2);
    extra.lo.y = 1600.0f;
    extra.hi.y = 1900.0f;
    extra.tag += " (east gate)";
    w.zones[kA].lines.push_back(extra);
    const auto plan = game::place_atlas(kA, w.lookup(), 12000.0f, 4, 16);
    CHECK_FALSE(find(plan, kB));
    CHECK(std::any_of(plan.notes.begin(), plan.notes.end(),
                      [](const std::string& n) { return n.find("disagree") != std::string::npos; }));
}

TEST_CASE("world atlas: each zone keeps what lies past its line, the zone before it what lies short of it") {
    const World w = make_world();
    const auto plan = game::place_atlas(kA, w.lookup(), 12000.0f, 4, 16);
    const auto cuts = game::atlas_cuts(plan);
    CHECK_FALSE(cuts.contains(kA));
    REQUIRE(cuts.contains(kB));
    REQUIRE(cuts.contains(kC));
    auto kept = [&](const std::string& z, float x) {
        const glm::vec3 p(x, 1000.0f, 0.0f);
        return std::all_of(cuts.at(z).begin(), cuts.at(z).end(),
                           [&](const glm::vec4& pl) { return glm::dot(glm::vec3(pl), p) + pl.w >= 0.0f; });
    };
    // B: past A's line (1950), short of its own line to C (3850).
    CHECK_FALSE(kept(kB, 1900.0f));
    CHECK(kept(kB, 2500.0f));
    CHECK_FALSE(kept(kB, 3900.0f));
    // C: past B's line.
    CHECK_FALSE(kept(kC, 3800.0f));
    CHECK(kept(kC, 4500.0f));
}

TEST_CASE("world atlas: zone lines kept as cells group as the regions do") {
    const World w = make_world();
    const auto& b = w.zones.at(kB);
    const auto groups = game::collect_neighbour_lines(b.lines, {}, game::zone_id_by_short_name(kB));
    REQUIRE(groups.size() == 2);
    // A reference to a zone point the server did not send is left out; one it sent joins.
    auto cells = b.lines;
    game::ZoneLineCell ref = cells[0];
    ref.region = 9;
    ref.reference = true;
    ref.zone_point = 3;
    ref.zone_id = 0;
    cells.push_back(ref);
    CHECK(game::collect_neighbour_lines(cells, {}, 0).size() == 2);
    const std::vector<game::ZonePointTarget> points{{3, 77, 5.0f, 6.0f, 7.0f}};
    const auto with = game::collect_neighbour_lines(cells, points, 0);
    CHECK(with.size() == 3);
    CHECK(std::any_of(with.begin(), with.end(), [](const auto& g) { return g.zone_id == 77; }));
}

TEST_CASE("world atlas: lines scattered round a city are placed one by one, and cut nothing") {
    World w = make_world();
    // A's lines to B: three doorways inside A, each leading to its own place in B, all 1900 + (0, 300) off; and one
    // that fits none of them.
    auto& lines = w.zones[kA].lines;
    lines.clear();
    auto door = [&](float x, float y, float to_x, float to_y, std::uint32_t region) {
        game::ZoneLineCell c = line(x - 7.0f, x + 7.0f, kB, to_x, region);
        c.lo.y = y - 7.0f;
        c.hi.y = y + 7.0f;
        c.server_x = to_y;
        c.tag += std::to_string(region);
        lines.push_back(c);
    };
    door(1000.0f, 1000.0f, -900.0f, 700.0f, 1);
    door(1200.0f, 400.0f, -700.0f, 100.0f, 2);
    door(600.0f, 1500.0f, -1300.0f, 1200.0f, 3);
    door(300.0f, 300.0f, 1500.0f, 1500.0f, 4);
    const auto one = game::atlas_line_offset(lines, {}, game::zone_id_by_short_name(kB));
    REQUIRE(one);
    CHECK(one->offset.x == doctest::Approx(1900.0f));
    CHECK(one->offset.y == doctest::Approx(300.0f));
    CHECK(one->estimates == 3);
    const auto plan = game::place_atlas(kA, w.lookup(), 12000.0f, 4, 16);
    const auto* b = find(plan, kB);
    REQUIRE(b);
    CHECK_FALSE(b->cut);
    CHECK(b->offset.x == doctest::Approx(1900.0f));
    // B is cut only where C begins (its own line on), not at A's doorways.
    const auto cuts = game::atlas_cuts(plan);
    REQUIRE(cuts.contains(kB));
    CHECK(cuts.at(kB).size() == 1);
    // Two of four is not more than half.
    lines.pop_back();
    door(300.0f, 300.0f, 1500.0f, 1500.0f, 4);
    door(310.0f, 1800.0f, 1500.0f, 200.0f, 5);
    lines.erase(lines.begin());
    CHECK_FALSE(game::atlas_line_offset(lines, {}, game::zone_id_by_short_name(kB)));
}

// ---- Measuring a zone ----

#include "app/world_atlas.h"
#include "assets/zone.h"

TEST_CASE("world atlas: an EQG zone's water surfaces are measured; one draped over a fall is not") {
    assets::Zone zone;
    assets::Mesh m;
    auto quad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, std::uint32_t material) {
        const auto first = static_cast<std::uint32_t>(m.indices.size());
        const auto base = static_cast<std::uint32_t>(m.vertices.size());
        for (const glm::vec3& p : {a, b, c, d}) {
            assets::Vertex v;
            v.position = p;
            m.vertices.push_back(v);
        }
        for (std::uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(base + i);
        m.parts.push_back({first, 6, material});
    };
    m.materials.resize(2);
    m.materials[0].frames = {"ground.dds"};
    m.materials[1].frames = {"water.dds"};
    m.materials[1].eqg_shader = assets::EqgShader::Water;
    quad({0, 0, 0}, {256, 0, 0}, {256, 256, 0}, {0, 256, 0}, 0);
    quad({0, 0, 10}, {96, 0, 10}, {96, 256, 10}, {0, 256, 10}, 1);     // a pond over the west
    quad({200, 0, 0}, {200, 256, 0}, {200, 256, 90}, {200, 0, 90}, 1); // a fall down the east side
    zone.terrain.push_back(m);
    const std::vector<float> ground(16, 0.0f);
    const auto water = app::measure_water(zone, {0.0f, 0.0f}, 64.0f, 4, 4, ground, glm::vec3(0.0f));
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const float w = water[static_cast<std::size_t>(y * 4 + x)];
            if (x < 2) {
                CHECK(w == doctest::Approx(10.0f));
            } else {
                CHECK(std::isnan(w)); // dry, and the fall at 200
            }
        }
}

namespace {
// A 2x1 24-bit BMP, both texels the one colour.
std::vector<std::byte> bmp_of(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    ByteWriter w;
    w.le<std::uint8_t>('B');
    w.le<std::uint8_t>('M');
    w.le<std::uint32_t>(14 + 40 + 8);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(14 + 40);
    w.le<std::uint32_t>(40);
    w.le<std::int32_t>(2);
    w.le<std::int32_t>(1);
    w.le<std::uint16_t>(1);
    w.le<std::uint16_t>(24);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(8);
    w.zeros(16);
    for (int k = 0; k < 2; ++k)
        for (std::uint8_t v : {b, g, r}) w.le(v);
    w.zeros(2);
    return w.take();
}
} // namespace

TEST_CASE("world atlas: each square's ground takes the colour of the texture on it, not a roof's or the water's") {
    assets::Zone zone;
    zone.generated_files["red.bmp"] = bmp_of(255, 0, 0);
    zone.generated_files["green.bmp"] = bmp_of(0, 255, 0);
    zone.generated_files["white.bmp"] = bmp_of(255, 255, 255);
    assets::Mesh m;
    auto quad = [&](float x0, float x1, float z, std::uint32_t material) {
        const auto first = static_cast<std::uint32_t>(m.indices.size());
        const auto base = static_cast<std::uint32_t>(m.vertices.size());
        for (auto [x, y] : {std::pair{x0, 0.0f}, {x1, 0.0f}, {x1, 256.0f}, {x0, 256.0f}}) {
            assets::Vertex v;
            v.position = {x, y, z};
            m.vertices.push_back(v);
        }
        for (std::uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(base + i);
        m.parts.push_back({first, 6, material});
    };
    m.materials.resize(4);
    m.materials[0].frames = {"red.bmp"};
    m.materials[1].frames = {"green.bmp"};
    m.materials[2].frames = {"white.bmp"};
    m.materials[2].eqg_shader = assets::EqgShader::Water;
    m.materials[3].frames = {"missing.bmp"};
    quad(0.0f, 256.0f, 0.0f, 0);    // the ground
    quad(0.0f, 96.0f, 50.0f, 1);    // a roof over the west
    quad(160.0f, 256.0f, 1.0f, 2);  // water over the east
    quad(0.0f, 256.0f, 2.0f, 3);    // a texture the zone does not have
    zone.terrain.push_back(m);
    // Measured at 1: the water's surface lies there exactly, the ground 1 under it.
    std::vector<float> ground(16, 1.0f);
    ground[15] = std::numeric_limits<float>::quiet_NaN(); // no ground there
    const auto color = app::measure_ground_color(zone, {0.0f, 0.0f}, 64.0f, 4, 4, ground);
    REQUIRE(color.size() == 16);
    for (std::size_t i = 0; i < 15; ++i) CHECK(color[i] == 0xff0000ffu);
    CHECK(color[15] == 0u);
}

TEST_CASE("world atlas: a zone's trees are counted by the square they stand in") {
    assets::Zone zone;
    // A mesh `height` tall with one material.
    auto mesh = [](float height, assets::BlendMode blend, const char* texture) {
        assets::Mesh m;
        assets::Material mat;
        mat.blend = blend;
        mat.frames = {texture};
        m.materials.push_back(mat);
        for (float z : {0.0f, height, 0.0f}) {
            assets::Vertex v;
            v.position = {z, 1.0f, z};
            m.vertices.push_back(v);
        }
        m.indices = {0, 1, 2};
        assets::MeshPart part;
        part.index_count = 3;
        m.parts.push_back(part);
        return m;
    };
    zone.objects["TREE1_ACTORDEF"] = mesh(40.0f, assets::BlendMode::Masked, "leaves.bmp");
    zone.objects["BUSH_ACTORDEF"] = mesh(5.0f, assets::BlendMode::Masked, "bush.bmp");      // too low
    zone.objects["HOUSE_ACTORDEF"] = mesh(60.0f, assets::BlendMode::Opaque, "wall.bmp");    // no plant
    auto place = [&](const char* actor, glm::vec2 at, float scale = 1.0f) {
        assets::Placement p;
        p.actor = actor;
        p.transform = glm::translate(glm::mat4(1.0f), glm::vec3(at, 0.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(scale));
        zone.placements.push_back(p);
    };
    place("TREE1_ACTORDEF", {10.0f, 10.0f});
    place("TREE1_ACTORDEF", {20.0f, 30.0f});
    place("TREE1_ACTORDEF", {100.0f, 10.0f});
    place("TREE1_ACTORDEF", {100.0f, 100.0f}, 0.25f); // 10 units tall at its scale: a bush
    place("BUSH_ACTORDEF", {10.0f, 100.0f});
    place("BUSH_ACTORDEF", {10.0f, 100.0f}, 8.0f); // 40 units tall: a tree
    place("HOUSE_ACTORDEF", {100.0f, 100.0f});
    place("TREE1_ACTORDEF", {900.0f, 10.0f}); // off the grid
    const auto trees = app::measure_trees(zone, {0.0f, 0.0f}, 64.0f, 2, 2);
    REQUIRE(trees.size() == 4);
    CHECK(trees[0] == 2);
    CHECK(trees[1] == 1);
    CHECK(trees[2] == 1);
    CHECK(trees[3] == 0);
}

// ---- Real files (EQ_GAME_DIR) ----

#include "app/world_atlas.h"
#include "assets/zone.h"
#include "render/zone_renderer.h"
#include "game_files.h"

TEST_CASE("world atlas: a zone's record from the game folder" * doctest::skip(!hailstone::test::game_dir())) {
    const auto& game = *hailstone::test::game_dir();
    for (const char* name : {"qeynos", "lakerathe"}) {
        auto zone = assets::load_zone(game, name);
        const auto liquid = render::classic_liquid_triangles(zone);
        std::size_t water_tris = 0;
        for (const auto& m : liquid)
            for (auto k : m) water_tris += k == render::kLiquidWater;
        const auto rec = app::measure_atlas_zone(zone, 1);
        std::size_t ground = 0, water = 0;
        for (float h : rec.ground) ground += !std::isnan(h);
        for (float h : rec.water) water += !std::isnan(h);
        MESSAGE(std::string(name), ": ", water_tris, " water triangles, ", ground, " squares with ground, ", water,
                " with water, ", rec.open_sky * 100.0f, "% open sky");
        // Its ground, its water, its lines; a city and a lake both mostly under open sky.
        CHECK(rec.valid());
        CHECK(ground > 500);
        CHECK(water > 0);
        CHECK_FALSE(rec.lines.empty());
        CHECK(rec.open_sky > game::kAtlasMinOpenSky);
    }
}
