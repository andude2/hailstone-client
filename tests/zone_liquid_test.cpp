#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "assets/regions.h"
#include "assets/wld.h"
#include "assets/zone.h"
#include "render/zone_renderer.h"

using namespace hailstone;
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

} // namespace

TEST_CASE("classic liquids: passable surfaces on a liquid region's top, carried through a dry zone line") {
    // North Ro's shore in miniature: under z 0 the sea (region 1, "WT") for x <= 100, a zone line's dry region
    // (region 2, "DR..TP") for 100 < x <= 120, plain ground (region 3) beyond and above.
    std::vector<wld::BspNode> nodes = {
        split({0, 0, -1}, 0, 2, 6),    // 1: z <= 0
        split({-1, 0, 0}, 100, 4, 3),  // 2: x <= 100
        split({-1, 0, 0}, 120, 5, 6),  // 3: x <= 120
        leaf(1),                       // 4
        leaf(2),                       // 5
        leaf(3),                       // 6
    };
    assets::Zone zone;
    zone.regions = assets::ZoneRegions::build(
        nodes, 3, {tag("WT_ZONE", {0}), tag("DRNTP00255000002000000000000000_ZONE", {1})});

    assets::Mesh m;
    auto tri = [&](float x0, float x1, float z0, float z1) {
        for (glm::vec3 p : {glm::vec3(x0, 0, z0), glm::vec3(x0, 10, z0), glm::vec3(x1, 0, z1)}) {
            assets::Vertex v;
            v.position = p;
            m.indices.push_back(static_cast<std::uint32_t>(m.vertices.size()));
            m.vertices.push_back(v);
        }
    };
    // The water material, passable (part 0): triangles 0..3.
    tri(10, 20, -0.5f, -0.5f);   // 0: on the sea's top
    tri(105, 115, -0.5f, -0.5f); // 1: the same surface over the zone line's dry region
    tri(105, 115, -10, -10);     // 2: the same material lower down, in the dry region: not the surface
    tri(30, 40, -0.5f, 9.5f);    // 3: sloped
    // Solid sand at the waterline (part 1): triangle 4.
    tri(50, 60, -0.5f, -0.5f);
    m.materials.resize(2);
    m.materials[0].name = "W1_MDF";
    m.materials[0].frames = {"w1.bmp"};
    m.materials[1].name = "SFS1_MDF";
    m.materials[1].frames = {"sfs1.bmp"};
    m.parts.push_back({0, 12, 0, false});
    m.parts.push_back({12, 3, 1, true});
    zone.terrain.push_back(m);

    const auto liquid = render::classic_liquid_triangles(zone);
    REQUIRE(liquid.size() == 1);
    REQUIRE(liquid[0].size() == 5);
    CHECK(liquid[0][0] == render::kLiquidWater);
    CHECK(liquid[0][1] == render::kLiquidWater);
    CHECK(liquid[0][2] == 0);
    CHECK(liquid[0][3] == 0);
    CHECK(liquid[0][4] == 0); // solid: the beach, not the sea

    // A passable sand triangle there would be the sea's.
    zone.terrain[0].parts[1].solid = false;
    CHECK(render::classic_liquid_triangles(zone)[0][4] == render::kLiquidWater);
}

TEST_CASE("zone renderer: a prepare that is cancelled stops and returns nothing") {
    // A load thrown away asks between placements and materials.
    assets::Zone zone;
    zone.placements.resize(3);
    render::ZoneRenderOptions options;
    options.cancelled = [] { return true; };
    CHECK_FALSE(render::ZoneRenderer::prepare(zone, options, 16));
    int asked = 0;
    options.cancelled = [&asked] {
        ++asked;
        return false;
    };
    CHECK(render::ZoneRenderer::prepare(zone, options, 16));
    CHECK(asked > 0);
}

namespace {

// A flat terrain mesh of `cell` squares over x0..x1, y0..y1 at height z, one plain material on `texture`.
assets::Mesh flat_terrain(float x0, float y0, float x1, float y1, float cell, float z, const char* texture) {
    assets::Mesh m;
    for (float x = x0; x < x1; x += cell)
        for (float y = y0; y < y1; y += cell)
            for (glm::vec3 p : {glm::vec3(x, y, z), glm::vec3(x + cell, y, z), glm::vec3(x + cell, y + cell, z),
                                glm::vec3(x, y, z), glm::vec3(x + cell, y + cell, z), glm::vec3(x, y + cell, z)}) {
                assets::Vertex v;
                v.position = p;
                v.normal = {0, 0, 1};
                v.uv = glm::vec2(p) / 64.0f;
                v.color = 0xff808080u;
                m.indices.push_back(static_cast<std::uint32_t>(m.vertices.size()));
                m.vertices.push_back(v);
            }
    m.materials.resize(1);
    m.materials[0].name = texture;
    m.materials[0].frames = {texture};
    m.parts.push_back({0, static_cast<std::uint32_t>(m.indices.size()), 0, true});
    return m;
}

} // namespace

TEST_CASE("zone renderer: the land round the zone takes the zone's ground, and its sea is drawn from both faces") {
    assets::Zone zone;
    zone.name = "here";
    zone.terrain.push_back(flat_terrain(0, 0, 2000, 1500, 100, 50, "grass.bmp"));
    const auto plain = render::ZoneRenderer::prepare(zone, {}, 64);
    REQUIRE(plain);
    const auto land = render::ZoneRenderer::prepared_land(*plain);
    REQUIRE(land.build);
    CHECK(land.build->field.valid());
    CHECK(land.build->ground.frames == std::vector<std::string>{"grass.bmp"});
    CHECK(land.build->ground.density == doctest::Approx(1.0f / 64.0f));
    CHECK(land.triangles > 0);
    CHECK(land.water_triangles == 0);

    // A sea along the west: an EQG water surface, which (as a classic one's two sheets) says nothing of which face
    // is up.
    assets::Mesh sea = flat_terrain(-1000, 0, 0, 1500, 100, 40, "water.dds");
    sea.materials[0].eqg_shader = assets::EqgShader::Water;
    sea.parts[0].solid = false;
    zone.terrain.push_back(sea);
    const auto wet = render::ZoneRenderer::prepare(zone, {}, 64);
    REQUIRE(wet);
    const auto shore = render::ZoneRenderer::prepared_land(*wet);
    CHECK(shore.water_triangles > 0);
    CHECK(shore.water_two_sided);
}

TEST_CASE("zone renderer: the land's ground is no sea bed under water, and its rock no roof over a floor") {
    // A shore in miniature: grass at z 50 for y < 600 (from x 100); a larger sea bed at z 0 beyond, under a water
    // region (region 1: z <= 10 and y >= 600); a cliff falling from z 150 to the grass at the zone's west edge; and
    // more roof than cliff, sloping as steeply but standing over the grass.
    std::vector<wld::BspNode> nodes = {
        split({0, 0, -1}, 10, 2, 4),   // 1: z <= 10
        split({0, 1, 0}, -600, 3, 4),  // 2: y >= 600
        leaf(1),                       // 3: the sea
        leaf(2),                       // 4: dry
    };
    assets::Zone zone;
    zone.name = "shore";
    zone.regions = assets::ZoneRegions::build(nodes, 2, {tag("WT_ZONE", {0})});
    zone.terrain.push_back(flat_terrain(100, 0, 2000, 600, 100, 50, "grass.bmp"));
    zone.terrain.push_back(flat_terrain(0, 600, 2000, 1500, 100, 0, "seabed.bmp"));
    // A slope of 45 degrees: from (x0, z0) up to (x1, z1) along x, over y0..y1.
    auto ramp = [](float x0, float z0, float x1, float z1, float y0, float y1, const char* texture) {
        assets::Mesh m;
        for (float y = y0; y < y1; y += 100.0f)
            for (glm::vec3 p : {glm::vec3(x0, y, z0), glm::vec3(x1, y, z1), glm::vec3(x1, y + 100, z1),
                                glm::vec3(x0, y, z0), glm::vec3(x1, y + 100, z1), glm::vec3(x0, y + 100, z0)}) {
                assets::Vertex v;
                v.position = p;
                v.normal = glm::normalize(glm::vec3(z0 - z1, 0.0f, x1 - x0));
                v.uv = glm::vec2(p) / 64.0f;
                v.color = 0xff808080u;
                m.indices.push_back(static_cast<std::uint32_t>(m.vertices.size()));
                m.vertices.push_back(v);
            }
        m.materials.resize(1);
        m.materials[0].name = texture;
        m.materials[0].frames = {texture};
        m.parts.push_back({0, static_cast<std::uint32_t>(m.indices.size()), 0, true});
        return m;
    };
    zone.terrain.push_back(ramp(0, 150, 100, 50, 0, 600, "cliff.bmp"));
    for (float x : {400.0f, 700.0f, 1000.0f, 1300.0f}) zone.terrain.push_back(ramp(x, 100, x + 100, 200, 100, 500, "slate.bmp"));
    zone.bounds_min = {0, 0, 0};
    zone.bounds_max = {2000, 1500, 200};

    const auto prepared = render::ZoneRenderer::prepare(zone, {}, 64);
    REQUIRE(prepared);
    const auto land = render::ZoneRenderer::prepared_land(*prepared);
    REQUIRE(land.build);
    REQUIRE_FALSE(land.build->ground.frames.empty());
    REQUIRE_FALSE(land.build->rock.frames.empty());
    CHECK(land.build->ground.frames.front() == "grass.bmp");
    CHECK(land.build->rock.frames.front() == "cliff.bmp");
}
