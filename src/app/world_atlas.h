#pragma once

// The world atlas on disk and in the land round the zone (docs/graphics.md "World atlas"; the records and the
// walk are game/world_atlas.h). A visit is written for each zone the client enters on a server; a zone's record is
// measured from its files on the neighbour zones' loader thread (app/neighbour_zones.h) and kept for every server.
// When a zone is entered, the zones placed round it give the land their ground (render::HorizonSource), past the
// neighbours whose ground the neighbour zones measured themselves.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "game/world_atlas.h"
#include "render/horizon.h"

namespace hailstone { class GameDir; }
namespace hailstone::assets { struct Zone; }

namespace hailstone::app {

// cache/atlas/<server>/<zone>.visit, and cache/atlas/zones/<zone>.bin (docs/layout.md).
std::filesystem::path atlas_visit_path(std::string_view server, std::string_view zone);
std::filesystem::path atlas_zone_path(std::string_view zone);
// The size of the zone's main file (its .s3d, else its .eqg); 0 when missing.
std::uint64_t atlas_zone_key(const GameDir& game, std::string_view zone);

// Writes the visit (when it says something new: the file is rewritten only when it differs).
void save_atlas_visit(std::string_view server, const game::AtlasVisit& visit);
std::optional<game::AtlasVisit> load_atlas_visit(std::string_view server, std::string_view zone);
// The zone's record when it was measured from the files the asset folder holds now.
std::optional<game::AtlasZone> load_atlas_zone(const GameDir& game, std::string_view zone);

// A zone's record from its loaded files: its terrain's ground as seen from above (a collision mesh without its placed
// objects: trees and houses are not its ground; the median of 2 x 2 samples a square, from two hits at least, and the
// highest of them), its water's surface (the classic regions' liquid triangles, render::classic_liquid_triangles, and
// the top of a water region standing over its ground), and
// its zone lines' cells. Empty when `cancelled` is set before it is done. The zone's placements are taken out while
// its terrain is measured, and put back.
game::AtlasZone measure_atlas_zone(assets::Zone& zone, std::uint64_t key, const std::atomic<bool>* cancelled = nullptr);

// A zone's water's surface over a grid of `nx` x `ny` squares `cell` across from `lo`, in the coordinates the zone is
// moved into by `offset` (its own: 0), NaN where none: the classic regions' liquid triangles over each square (along
// their edges too: a canal narrower than a square), and the top of a water region standing over the square's
// `ground` (a harbour whose surface the liquid triangles leave out), and an EQG zone's level RegionWater surfaces.
// Stops early when `cancelled` is set.
std::vector<float> measure_water(const assets::Zone& zone, glm::vec2 lo, float cell, int nx, int ny,
                                 const std::vector<float>& ground, const glm::vec3& offset,
                                 const std::atomic<bool>* cancelled = nullptr);

// The colour of each square's ground over a grid as measure_water's, 0 where none: the average of the texture (its
// first frame) on the terrain whose height at the square's middle is nearest the square's `ground` (within
// kAtlasColorReach), leaving out the water, what is not drawn, cut out (leaves) or glows. The far land takes it on
// where an atlas zone's ground is (render::HorizonSource::color).
inline constexpr float kAtlasColorReach = 24.0f;
std::vector<std::uint32_t> measure_ground_color(const assets::Zone& zone, glm::vec2 lo, float cell, int nx, int ny,
                                                const std::vector<float>& ground,
                                                const std::atomic<bool>* cancelled = nullptr);

// How far from the current zone's box a zone may lie and join, and how far the walk goes.
inline constexpr float kAtlasReach = 12000.0f;
inline constexpr int kAtlasMaxHops = 4;
inline constexpr std::size_t kAtlasMaxZones = 16;

struct AtlasLand {
    // The land's sources: the neighbours' ground the cache holds (`measured`), and the atlas's zones past them, each
    // cut where the zone past it begins (game::atlas_cuts).
    render::HorizonSources sources;
    // The cuts by zone, for a neighbour zone's own ground measured later this visit (app/neighbour_zones.h).
    std::map<std::string, std::vector<glm::vec4>> cuts;
    // The zones the atlas wanted whose record is not measured yet (the neighbour zones' loader measures them).
    std::vector<std::string> missing;
    std::size_t placed = 0; // zones of the atlas's own that give the land their ground
};
// The land's sources for `zone` on `server`: `measured` (the neighbour zones' cached ground, with the offsets it was
// moved by) and the atlas's zones within kAtlasReach.
AtlasLand atlas_land(const GameDir& game, std::string_view server, std::string_view zone,
                     const std::vector<render::HorizonCacheEntry>& measured);

} // namespace hailstone::app
