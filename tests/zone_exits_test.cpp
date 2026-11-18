// The current zone's exits (game/zone_exits.h): its zone-line regions grouped by the zone they lead to.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "assets/regions.h"
#include "assets/wld.h"
#include "game/zone_exits.h"

using namespace hailstone;
using namespace hailstone::game;
namespace wld = hailstone::assets::wld;

namespace {

wld::BspNode split(glm::vec3 n, float d, std::uint32_t front, std::uint32_t back) {
    wld::BspNode node;
    node.normal = n;
    node.distance = d;
    node.front = front;
    node.back = back;
    return node;
}

wld::BspNode leaf(std::uint32_t region) {
    wld::BspNode node;
    node.region = region;
    return node;
}

wld::RegionTag tag(std::string text, std::vector<std::uint32_t> regions) {
    wld::RegionTag t;
    t.tag = std::move(text);
    t.regions = std::move(regions);
    return t;
}

// East Commonlands' west line in miniature (world coordinates: world y is the server's x): a slab y in [-1610, -1600]
// across x in [-577, 583], the rest of the zone region 2. Region 1 is tagged as a reference to zone point 3.
assets::ZoneRegions west_line_zone() {
    std::vector<wld::BspNode> nodes = {
        split({0, 1, 0}, 1610, 2, 7),   // 1: y > -1610
        split({0, -1, 0}, -1600, 3, 7), // 2: y < -1600
        split({1, 0, 0}, 577, 4, 7),    // 3: x > -577
        split({-1, 0, 0}, 583, 5, 7),   // 4: x < 583
        leaf(1),                        // 5
        leaf(2),                        // 6 (unused)
        leaf(2),                        // 7
    };
    return assets::ZoneRegions::build(nodes, 2, {tag("DRNTP00255000003000000000000000_ZONE", {0})});
}

const glm::vec3 kZoneLo(-3177, -2230, -271), kZoneHi(1908, 5601, 720);

} // namespace

TEST_CASE("zone exits: a BSP leaf's cell is its split planes cut from the bounds") {
    auto regions = west_line_zone();
    auto cells = regions.leaf_cells(kZoneLo, kZoneHi, [](std::uint32_t r) { return r == 1; });
    REQUIRE(cells.size() == 1);
    CHECK(cells[0].region == 1);
    CHECK(cells[0].lo.x == doctest::Approx(-577));
    CHECK(cells[0].hi.x == doctest::Approx(583));
    CHECK(cells[0].lo.y == doctest::Approx(-1610));
    CHECK(cells[0].hi.y == doctest::Approx(-1600));
    CHECK(cells[0].lo.z == doctest::Approx(-271));
    CHECK(cells[0].hi.z == doctest::Approx(720));

    // An oblique split: the cell's box is the cut polytope's, not the bounds'.
    std::vector<wld::BspNode> oblique = {split(glm::normalize(glm::vec3(1, 1, 0)), 0, 2, 3), leaf(1), leaf(2)};
    auto o = assets::ZoneRegions::build(oblique, 2, {});
    auto back = o.leaf_cells(glm::vec3(-10), glm::vec3(10), [](std::uint32_t r) { return r == 2; });
    REQUIRE(back.size() == 1);
    CHECK(back[0].lo.x == doctest::Approx(-10));
    CHECK(back[0].hi.x == doctest::Approx(10));
    CHECK(back[0].hi.y == doctest::Approx(10));
    auto none = o.leaf_cells(glm::vec3(-10), glm::vec3(10), [](std::uint32_t) { return false; });
    CHECK(none.empty());
}

TEST_CASE("zone exits: zone point targets in world coordinates, 999999 kept") {
    auto t = line_target_from_server(775.0f, -28.0f, 999999.0f);
    CHECK(t.position.x == doctest::Approx(-28.0f)); // world x = server y
    CHECK(t.position.y == doctest::Approx(775.0f)); // world y = server x
    CHECK(!t.keep.x);
    CHECK(!t.keep.y);
    CHECK(t.keep.z);
    CHECK(t.position.z == 0.0f);
    auto k = line_target_from_server(-1592.0f, 999999.0f, -54.0f);
    CHECK(k.keep.x);
    CHECK(!k.keep.y);
}

TEST_CASE("zone exits: the lines by destination; lines home, to zone 0 and to unknown points left out") {
    auto regions = west_line_zone();
    // ecommons' zone point 3: freportw (9) at server (775, -28, -24); point 5 leads elsewhere and has no line.
    std::vector<ZonePointTarget> points = {{3, 9, 775.0f, -28.0f, -24.0f}, {5, 25, 0.0f, 0.0f, 0.0f}};
    auto lines = collect_zone_exits(regions, kZoneLo, kZoneHi, points, 22);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].zone_id == 9);
    REQUIRE(lines[0].targets.size() == 1);
    CHECK(lines[0].targets[0].position.y == doctest::Approx(775.0f));
    REQUIRE(lines[0].cells.size() == 1);
    CHECK(lines[0].lo.y == doctest::Approx(-1610.0f));
    CHECK(lines[0].hi.y == doctest::Approx(-1600.0f));
    // Lines back into the current zone are left out, and so is a line whose zone point the server did not send.
    CHECK(collect_zone_exits(regions, kZoneLo, kZoneHi, points, 9).empty());
    CHECK(collect_zone_exits(regions, kZoneLo, kZoneHi, {{4, 9, 0.0f, 0.0f, 0.0f}}, 22).empty());
    CHECK(collect_zone_exits(regions, kZoneLo, kZoneHi, {{3, 0, 0.0f, 0.0f, 0.0f}}, 22).empty());
}

TEST_CASE("zone exits: two regions to one zone are one exit, each zone point counted once") {
    std::vector<wld::BspNode> nodes = {
        split({0, 1, 0}, 1610, 2, 8), split({0, -1, 0}, -1600, 3, 8), split({1, 0, 0}, 577, 4, 8),
        split({-1, 0, 0}, 583, 5, 8), split({1, 0, 0}, 0, 6, 7),      leaf(1), leaf(3), leaf(2)};
    auto two = assets::ZoneRegions::build(nodes, 3,
                                          {tag("DRNTP00255000003000000000000000_ZONE", {0}),
                                           tag("DRNTP00255000004000000000000000_ZONE", {2})});
    std::vector<ZonePointTarget> points = {{3, 9, 775.0f, -28.0f, -24.0f}, {4, 9, 780.0f, 999999.0f, -24.0f}};
    auto lines = collect_zone_exits(two, kZoneLo, kZoneHi, points, 22);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].cells.size() == 2);
    CHECK(lines[0].targets.size() == 2);
    // The same point named by both regions is one target.
    std::vector<ZonePointTarget> one = {{3, 9, 775.0f, -28.0f, -24.0f}, {4, 9, 775.0f, -28.0f, -24.0f}};
    auto shared = assets::ZoneRegions::build(nodes, 3,
                                             {tag("DRNTP00255000003000000000000000_ZONE", {0}),
                                              tag("DRNTP00255000003000000000000000_ZONE", {2})});
    auto same = collect_zone_exits(shared, kZoneLo, kZoneHi, one, 22);
    REQUIRE(same.size() == 1);
    CHECK(same[0].targets.size() == 1);
}
