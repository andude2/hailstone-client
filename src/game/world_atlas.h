#pragma once

// The world atlas (docs/graphics.md, "World atlas"): where the zones a player has been in lie in relation to each
// other, so the land round an outdoor zone (render/horizon.h) takes the shape of the zones past its neighbours, not
// only of the neighbours themselves. Hailstone's own; the original drew nothing past its zone's own geometry.
//
// Two kinds of record, both kept in the cache (app/world_atlas.h, docs/layout.md):
// - a visit (per server): what the server said of a zone the client was in: its id, whether it is outdoor (OP_NewZone)
//   and its zone points (OP_SendZonepoints), which the server sends only for the zone one is in;
// - a zone (from the asset folder, any server): its box, its ground and water on a grid, and its zone lines' cells.
//
// From these, place_atlas walks out from the current zone across the zone lines, zone by zone, and finds each zone's
// offset into the current zone's coordinates (the neighbour zones' arithmetic, game/neighbour_zones.h, with the
// ground grids standing in for the zones' geometry). Only zones visited on this server and outdoor there join: a
// zone's zone points and whether it has a sky come from its server alone.
//
// All positions are world (zone file) coordinates (game/coords.h).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "game/neighbour_zones.h"

namespace hailstone::game {

// What the server said of a zone the client was in.
struct AtlasVisit {
    std::string zone;          // short name
    std::uint32_t zone_id = 0;
    bool outdoor = false;      // OP_NewZone: a sky and an outdoor time type (1, 2, 5)
    std::vector<ZonePointTarget> points;
};
std::vector<std::byte> write_atlas_visit(const AtlasVisit& visit);
// nullopt when the bytes are not a visit of this version, or are damaged.
std::optional<AtlasVisit> read_atlas_visit(std::span<const std::byte> data);

// The atlas's grid: the land's squares (render::kHorizonCell; app/world_atlas.cpp checks they agree).
inline constexpr float kAtlasCell = 64.0f;
// A zone's grid at most this many squares across.
inline constexpr int kAtlasMaxSide = 1024;
// A zone with less of its ground under open sky than this does not join (AtlasZone::open_sky).
inline constexpr float kAtlasMinOpenSky = 0.5f;

// What the atlas keeps of a zone's files.
struct AtlasZone {
    std::string zone;
    std::uint64_t key = 0; // the zone's files' size (app::atlas_zone_key): another size, measured again
    glm::vec3 lo{0.0f}, hi{0.0f}; // its box
    glm::vec2 grid_lo{0.0f};
    float cell = kAtlasCell;
    int nx = 0, ny = 0;
    // Per square, NaN where none: its terrain's ground as seen from above (the median of its samples), the highest of
    // them, and its water's surface.
    std::vector<float> ground, top, water;
    // Per square, its ground's colour (the average of the texture on the terrain at the ground there, sRGB, R in the
    // low byte; alpha 255), 0 where none is known.
    std::vector<std::uint32_t> color;
    // The share of its squares with ground that have open sky over it (no surface within kOpenSkyHeight): a dungeon
    // the server gives a sky still has its roof, and does not join (kAtlasMinOpenSky).
    float open_sky = 1.0f;
    std::vector<ZoneLineCell> lines;
    bool valid() const {
        const auto n = static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny);
        return nx > 0 && ny > 0 && ground.size() == n && top.size() == n && water.size() == n && color.size() == n;
    }
    // The ground in the square holding p; nullopt outside the grid or where it has none.
    std::optional<float> ground_at(glm::vec2 p) const;
};
std::vector<std::byte> write_atlas_zone(const AtlasZone& zone);
std::optional<AtlasZone> read_atlas_zone(std::span<const std::byte> data);

// A zone line out of a zone as the atlas has it: to which zone, its seam and the offset (in the zone's coordinates:
// the other zone's + offset = this one's).
struct AtlasEdge {
    std::string to;
    std::uint32_t to_id = 0;
    Seam seam;
    NeighbourOffset offset;
    bool agree = true; // the targets agree within kMaxTargetSpread (else no one offset fits; not followed)
    // Its seam is one zone edge the lines face; false when the offset came from the lines one by one
    // (atlas_line_offset), whose seam runs through the zone (a city's gates and tunnels), so nothing is cut there.
    bool cut = true;
};
// The zone lines out of `zone` as `visit` gives their targets, each refined against the zone it leads to when its
// record is known (`other`: its line back and its ground at the seam). Where the targets disagree taken together
// (several lines round a city), the lines one by one: atlas_line_offset.
std::vector<AtlasEdge> atlas_edges(const AtlasZone& zone, const AtlasVisit& visit,
                                   const std::function<const AtlasZone*(const std::string&)>& other);
// The offset from each zone-line region to `to_id` on its own (its cells' middle minus its target; 0 on an axis its
// target keeps), when more than half of them, two at least, agree within kMaxTargetSpread: their median. Lines into
// a city often lead from small doorways scattered round it, each to its own place past the other zone's wall, which
// one seam does not fit but each line does.
std::optional<NeighbourOffset> atlas_line_offset(const std::vector<ZoneLineCell>& cells,
                                                 const std::vector<ZonePointTarget>& points, std::uint32_t to_id);

// A zone placed in the current zone's coordinates.
struct AtlasPlacement {
    std::string zone;
    int hop = 0;          // 0 the current zone, 1 its neighbours, ...
    glm::vec3 offset{0.0f};
    std::string parent;   // the zone it was reached from
    Seam seam;            // the line it was reached across, in the current zone's coordinates (hop >= 1)
    bool cut = true;      // AtlasEdge::cut
};
struct AtlasLookup {
    std::function<const AtlasZone*(const std::string&)> zone;
    std::function<const AtlasVisit*(const std::string&)> visit;
    // The offset the neighbour zones measured for one of the current zone's own neighbours (its cached ground,
    // app/neighbour_zones.h), better than the atlas's own; nullopt when none.
    std::function<std::optional<glm::vec3>(const std::string&)> measured;
};
struct AtlasPlan {
    std::vector<AtlasPlacement> zones;  // the current zone first, then by hop
    std::vector<std::string> missing;   // visited, outdoor zones the walk needed whose record is not there yet
    std::vector<std::string> notes;     // for the log
};
// Out from `current` across the zone lines, at most `max_hops` lines and `max_zones` zones, keeping the zones whose
// box lies within `reach` of the current zone's and whose ground is mostly under open sky (kAtlasMinOpenSky). A zone reached twice keeps its first (nearest) placement; where the
// two disagree by more than kAtlasDisagree it is noted.
inline constexpr float kAtlasDisagree = 200.0f;
AtlasPlan place_atlas(const std::string& current, const AtlasLookup& lookup, float reach, int max_hops,
                      std::size_t max_zones);

// The planes each placed zone's ground is cut by (render::cut_horizon_source), by zone: a zone keeps what lies past
// the line it was reached across, and the zone it was reached from what lies short of it. The current zone gets none,
// nor a zone placed by its lines one by one (AtlasPlacement::cut).
std::map<std::string, std::vector<glm::vec4>> atlas_cuts(const AtlasPlan& plan);

} // namespace hailstone::game
