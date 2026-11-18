#pragma once

// The current zone's exits (docs/navigation.md, "Zone atlas"): its zone-line regions grouped by the zone they lead
// to, each with where it leads, for the map's zone finder (app/lua_eq_map.cpp, EqHost::zone_exits).
//
// All positions are world (zone file) coordinates (game/coords.h); the zone points' targets come in server
// coordinates and are swapped on the way in.

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace hailstone::assets { class ZoneRegions; }

namespace hailstone::game {

// A zone point's target as OP_SendZonepoints gives it (net::ZonePoint), in server coordinates; 999999 keeps that
// coordinate of the player's.
struct ZonePointTarget {
    std::uint32_t number = 0;
    std::uint32_t zone_id = 0;
    float server_x = 0.0f, server_y = 0.0f, server_z = 0.0f;
};

// The value that keeps a coordinate (EQEmu's zone_points, OP_SendZonepoints).
inline constexpr float kKeepCoordinate = 999999.0f;
inline bool keeps(float v) { return v >= kKeepCoordinate - 0.5f; }

// Where a zone line leads, in world coordinates; keep[i] when that coordinate stays the player's (then 0).
struct LineTarget {
    glm::vec3 position{0.0f};
    glm::bvec3 keep{false};
};
LineTarget line_target_from_server(float server_x, float server_y, float server_z);

// The convex piece of space one BSP leaf of a zone-line region holds (assets::ZoneRegions::leaf_cells).
struct LineCell {
    glm::vec3 lo{0.0f}, hi{0.0f};
};

// Every zone line of the current zone that leads to one other zone.
struct ZoneExitLines {
    std::uint32_t zone_id = 0;
    std::vector<LineCell> cells;      // the lines' pieces of space
    std::vector<LineTarget> targets;  // one per zone point (or absolute tag) leading there
    glm::vec3 lo{0.0f}, hi{0.0f};     // round all the cells
};

// One BSP leaf of a zone-line region, with the region's target as its tag gives it: an absolute target (zone id and
// server coordinates) or a reference to a zone point the server sends.
struct ZoneLineCell {
    std::uint32_t region = 0;
    glm::vec3 lo{0.0f}, hi{0.0f};
    bool reference = false;
    std::int32_t zone_point = -1;
    std::uint32_t zone_id = 0;
    float server_x = 0.0f, server_y = 0.0f, server_z = 0.0f;
    std::string tag;
};
// Every zone-line region's leaves within the zone's box (and 200 units round it).
std::vector<ZoneLineCell> zone_line_cells(const assets::ZoneRegions& regions, const glm::vec3& zone_lo,
                                          const glm::vec3& zone_hi);

// The zone lines by the zone they lead to: each zone-line region's leaves, its target from its tag (an absolute
// target) or from the zone point it names. Lines to `current_zone`, to zone 0 and to unknown zone points are left out.
std::vector<ZoneExitLines> collect_zone_exits(const assets::ZoneRegions& regions, const glm::vec3& zone_lo,
                                              const glm::vec3& zone_hi, const std::vector<ZonePointTarget>& points,
                                              std::uint32_t current_zone);
// The same from the cells zone_line_cells found.
std::vector<ZoneExitLines> collect_zone_exits(const std::vector<ZoneLineCell>& cells,
                                              const std::vector<ZonePointTarget>& points, std::uint32_t current_zone);

} // namespace hailstone::game
