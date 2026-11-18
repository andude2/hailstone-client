#include "game/zone_exits.h"

#include <algorithm>
#include <format>
#include <map>

#include "assets/regions.h"
#include "game/coords.h"

namespace hailstone::game {

LineTarget line_target_from_server(float server_x, float server_y, float server_z) {
    // World x is the server's y and world y the server's x (game/coords.h).
    LineTarget t;
    t.position = server_to_world(glm::vec3(server_x, server_y, server_z));
    t.keep = glm::bvec3(keeps(server_y), keeps(server_x), keeps(server_z));
    for (int i = 0; i < 3; ++i)
        if (t.keep[i]) t.position[i] = 0.0f;
    return t;
}

std::vector<ZoneLineCell> zone_line_cells(const assets::ZoneRegions& regions, const glm::vec3& zone_lo,
                                          const glm::vec3& zone_hi) {
    std::map<std::uint32_t, ZoneLineCell> by_region; // the region's target, the cells' template
    for (std::uint32_t r = 1; r <= regions.region_count(); ++r) {
        const auto info = regions.info(r);
        if (!info.is_zone_line()) continue;
        const auto& z = *info.zone_line;
        ZoneLineCell c;
        c.region = r;
        c.reference = z.reference;
        c.zone_point = z.zone_point;
        c.zone_id = z.zone_id;
        c.server_x = z.server_x;
        c.server_y = z.server_y;
        c.server_z = z.server_z;
        c.tag = info.tag;
        by_region.emplace(r, std::move(c));
    }
    std::vector<ZoneLineCell> out;
    if (by_region.empty()) return out;
    const glm::vec3 margin(200.0f, 200.0f, 200.0f);
    for (const auto& leaf : regions.leaf_cells(zone_lo - margin, zone_hi + margin,
                                               [&](std::uint32_t r) { return by_region.contains(r); })) {
        auto it = by_region.find(leaf.region);
        if (it == by_region.end()) continue;
        ZoneLineCell c = it->second;
        c.lo = leaf.lo;
        c.hi = leaf.hi;
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<ZoneExitLines> collect_zone_exits(const assets::ZoneRegions& regions, const glm::vec3& zone_lo,
                                              const glm::vec3& zone_hi, const std::vector<ZonePointTarget>& points,
                                              std::uint32_t current_zone) {
    return collect_zone_exits(zone_line_cells(regions, zone_lo, zone_hi), points, current_zone);
}

std::vector<ZoneExitLines> collect_zone_exits(const std::vector<ZoneLineCell>& line_cells,
                                              const std::vector<ZonePointTarget>& points, std::uint32_t current_zone) {
    // Each zone-line region's destination: (zone, a key for its target so each zone point counts once).
    struct Dest {
        std::uint32_t zone = 0;
        std::string key;
        LineTarget target;
    };
    std::map<std::uint32_t, Dest> by_region;
    for (const auto& z : line_cells) {
        if (by_region.contains(z.region)) continue;
        Dest d;
        if (z.reference) {
            auto it = std::find_if(points.begin(), points.end(), [&](const ZonePointTarget& p) {
                return z.zone_point >= 0 && p.number == static_cast<std::uint32_t>(z.zone_point);
            });
            if (it == points.end()) continue;
            d.zone = it->zone_id;
            d.key = std::format("#{}", it->number);
            d.target = line_target_from_server(it->server_x, it->server_y, it->server_z);
        } else {
            d.zone = z.zone_id;
            d.key = z.tag;
            d.target = line_target_from_server(z.server_x, z.server_y, z.server_z);
        }
        if (d.zone == 0 || d.zone == current_zone) continue;
        by_region.emplace(z.region, std::move(d));
    }
    std::vector<ZoneExitLines> out;
    if (by_region.empty()) return out;
    std::map<std::uint32_t, std::size_t> group; // zone -> index in out
    std::map<std::uint32_t, std::vector<std::string>> keys;
    for (const auto& [r, d] : by_region) {
        auto [it, fresh] = group.try_emplace(d.zone, out.size());
        if (fresh) {
            out.push_back({});
            out.back().zone_id = d.zone;
        }
        auto& k = keys[d.zone];
        if (std::find(k.begin(), k.end(), d.key) == k.end()) {
            k.push_back(d.key);
            out[it->second].targets.push_back(d.target);
        }
    }
    for (const auto& c : line_cells) {
        auto it = by_region.find(c.region);
        if (it == by_region.end()) continue;
        out[group.at(it->second.zone)].cells.push_back({c.lo, c.hi});
    }
    std::erase_if(out, [](const ZoneExitLines& l) { return l.cells.empty(); });
    for (auto& l : out) {
        l.lo = l.cells.front().lo;
        l.hi = l.cells.front().hi;
        for (const auto& c : l.cells) {
            l.lo = glm::min(l.lo, c.lo);
            l.hi = glm::max(l.hi, c.hi);
        }
    }
    return out;
}

} // namespace hailstone::game
