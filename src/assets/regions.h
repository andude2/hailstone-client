#pragma once

// Zone regions of a classic (WLD) zone: water, lava, slime, PvP areas, ice and
// zone lines. The zone WLD splits space with a BSP tree (fragment 0x21) whose
// leaves name a region (0x22, by file order); 0x29 fragments tag sets of
// regions with a name such as "WTN_..." or "DRNTP00255000004...".
//
// Tag meanings follow the original client: ParseRegionNameFlags @0x487530
// for the flag bits and
// Teleport_ResolveTeleportIndex @0x506630 for the zone-line fields. The BSP
// walk matches the EQEmu server's water map (zone/water_map_v1.cpp) and
// OpenEQ crates/openeq-assets/src/liquid_regions.rs (MIT OR Apache-2.0).
// Details and corpus counts: docs/formats/wld.md, "Regions".
//
// Positions are world (zone file) coordinates, see game/coords.h.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "assets/wld.h"

namespace hailstone::assets {

// The region type in the low byte of the client's region flags.
enum class RegionType : std::uint8_t {
    Normal = 0,        // "DR" (dry) or untagged
    Water = 5,         // "WT", EQG "AWT"
    Slime = 6,         // "SL" (EQEmu: slime; OpenEQ: water that blocks sight)
    Lava = 7,          // "LA", EQG "ALV"
    FreezingWater = 8, // "VW", EQG "AVW"
    Water2 = 9,        // "W2"
    Water3 = 10,       // "W3"
};

// Bits above the type byte (ParseRegionNameFlags).
inline constexpr std::uint32_t kRegionTeleport = 0x80000000u; // "..TP" at [3]: zone line / teleport
inline constexpr std::uint32_t kRegionPvP = 0x40000000u;      // 'P' at [2]
inline constexpr std::uint32_t kRegionFlagM = 0x20000000u;    // 'M' at [0x1f] (meaning unknown)
inline constexpr std::uint32_t kRegionSlippery = 0x10000000u; // 'S' at [0x20]: ice (friction 0.99)
inline constexpr std::uint32_t kRegionFlagP = 0x08000000u;    // 'P' at [0x21] (meaning unknown)
inline constexpr std::uint32_t kRegionFlagF = 0x04000000u;    // 'F' at [0x21] (meaning unknown)

// Where a zone line goes, decoded from its tag ("DRNTP" + 5-digit zone +
// fields; Teleport_ResolveTeleportIndex):
//   zone 255: a reference; chars 10..15 are the number of a server zone point
//             (OP_SendZonepoints iterator = zone_points.number).
//   other:    an absolute target: zone id, then y (10..15), x (16..21),
//             z (22..27) and heading (28..30) in server coordinates.
struct ZoneLineTarget {
    bool reference = false;
    std::int32_t zone_point = -1; // reference: server zone point number
    std::uint32_t zone_id = 0;    // absolute: target zone id (0 = no zone; the client ignores it)
    float server_y = 0.0f, server_x = 0.0f, server_z = 0.0f, heading = 0.0f; // absolute only
};

// The water types: water, slime, freezing water, W2, W3.
inline bool is_water_type(std::uint32_t type) { return type == 5 || type == 6 || type == 8 || type == 9 || type == 10; }

struct RegionInfo {
    std::uint32_t region = 0;   // 1-based region number; 0 = outside every region
    std::uint32_t flags = 0;    // client region flags (type byte + bits above)
    std::string tag;            // the last tag applied, empty when untagged
    std::optional<ZoneLineTarget> zone_line;

    RegionType type() const { return static_cast<RegionType>(flags & 0xff); }
    bool is_water() const { return is_water_type(flags & 0xff); }
    bool is_lava() const { return (flags & 0xff) == 7; }
    bool is_pvp() const { return flags & kRegionPvP; }
    bool is_slippery() const { return flags & kRegionSlippery; }
    bool is_zone_line() const { return (flags & kRegionTeleport) && zone_line.has_value(); }
};

// Applies one tag to region flags, exactly as ParseRegionNameFlags does.
std::uint32_t region_flags_from_tag(std::string_view tag, std::uint32_t flags = 0);

// Decodes a teleport tag; nullopt when the tag carries no teleport bit.
// "ATP<n>" (EQG) becomes a reference to zone point n, as the client rewrites it
// to "DRNTP00255%05d...".
std::optional<ZoneLineTarget> decode_zone_line(std::string_view tag);

// An EQG region volume (binary ZON): an oriented box. Angles are as stored
// and used as native 512-unit turns (truncated) about Z, -Y and X; extents are
// signed half-sizes (OpenEQ docs/EQGZ_NATIVE_REGIONS.md, EQGraphicsDX9.dll
// 0x100bc6e0 / 0x10021c50).
struct RegionBox {
    std::string name;
    glm::vec3 center{0.0f};
    glm::vec3 angles{0.0f};
    glm::vec3 extents{0.0f};
};

class ZoneRegions {
public:
    ZoneRegions() = default;

    // From a zone WLD: its 0x21 tree, the number of 0x22 regions and its 0x29 tags.
    // A zone without a tree yields an empty set.
    static ZoneRegions from_wld(const wld::WldFile& wld);
    // From EQG region boxes, in file order. A point is in the first box that
    // contains it, skipping "APV" boxes (the native query @0x10021410).
    static ZoneRegions from_boxes(const std::vector<RegionBox>& boxes);
    // From parts (tests and tools). Tags apply in order.
    static ZoneRegions build(std::vector<wld::BspNode> nodes, std::size_t region_count,
                             const std::vector<wld::RegionTag>& tags);

    bool empty() const { return nodes_.empty() && boxes_.empty(); }
    // An EQG zone's (from_boxes) regions.
    bool has_boxes() const { return !boxes_.empty(); }
    std::size_t region_count() const { return regions_.size(); }
    std::size_t node_count() const { return nodes_.size(); }

    // 1-based region containing the point, 0 when none. A point exactly on a
    // split plane is in no region (as the EQEmu water map).
    // EQG boxes (CTerrain::FindRegionContaining @0x10021540): with a prefix,
    // the first containing box in file order whose name's first three
    // characters equal it (strncmp 3); otherwise, or when none matches, the
    // first containing box whose name does not start with "APV". No eqgame
    // caller passes a prefix.
    std::uint32_t region_at(const glm::vec3& world_pos, std::string_view prefix = {}) const;
    // CSceneGraph::IsPointInWorld (vtable +0x4c, CTerrainBase::FindRegionForPoint
    // @0x100bc330): the BSP walk reaches a leaf with a region; front when
    // n . p + d >= 0. False when the walk falls off a missing child or ends
    // in a leaf without a region. True for a zone without a BSP tree (EQG
    // zones: their terrain system answers instead, not ported).
    bool in_world(const glm::vec3& world_pos) const;
    // EQG zones: the point is inside box `region` (1-based), whatever boxes come before it; false for a degenerate
    // box, a number out of range or a BSP zone. For the buried zone-line fallback (game/zone_lines.h).
    bool box_contains(std::uint32_t region, const glm::vec3& world_pos) const;
    // Region number, flags and zone-line target at a point.
    RegionInfo lookup(const glm::vec3& world_pos) const;
    // Info for a 1-based region (an empty RegionInfo for 0 or out of range).
    RegionInfo info(std::uint32_t region) const;
    // Every tagged region, for tools and tests.
    std::vector<RegionInfo> tagged() const;

    // The space a region holds, as boxes: each BSP leaf of a region `want` picks (by 1-based number) is the convex
    // cell its path of split planes cuts from the box lo..hi; its box is that cell's. EQG zones: each box's corners'
    // box. For the zone's exits (game/zone_exits.h), which need where the zone lines are.
    struct LeafCell {
        std::uint32_t region = 0;
        glm::vec3 lo{0.0f}, hi{0.0f};
    };
    std::vector<LeafCell> leaf_cells(const glm::vec3& lo, const glm::vec3& hi,
                                     const std::function<bool(std::uint32_t)>& want) const;

private:
    struct Entry {
        std::uint32_t flags = 0;
        std::int32_t tag = -1; // index into tags_
    };
    struct Box {
        glm::mat4 world_to_box{1.0f}; // maps the box to [-1, 1]^3
        bool skip = false;            // "APV" boxes are not selected without a prefix
        bool degenerate = false;      // no inverse: never selected
    };
    std::vector<wld::BspNode> nodes_;
    std::vector<Box> boxes_;      // EQG zones: region number = index + 1
    std::vector<Entry> regions_; // index = region number - 1
    std::vector<std::string> tags_;
};

} // namespace hailstone::assets
