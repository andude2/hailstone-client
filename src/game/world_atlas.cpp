#include "game/world_atlas.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <set>
#include <stdexcept>

#include "core/byte_io.h"
#include "game/zone_names.h"

namespace hailstone::game {

namespace {

constexpr std::uint32_t kVisitMagic = 0x31565441u; // "ATV1"
constexpr std::uint32_t kVisitVersion = 1;
constexpr std::uint32_t kZoneMagic = 0x315a5441u;  // "ATZ1"
// 2: with its ground's colour.
constexpr std::uint32_t kZoneVersion = 2;
constexpr std::size_t kMaxName = 64;
constexpr std::size_t kMaxTag = 256;
constexpr std::uint32_t kMaxPoints = 4096;
constexpr std::uint32_t kMaxLineCells = 65536;

void write_string(ByteWriter& w, std::string_view s, std::size_t max) {
    const std::size_t n = std::min(s.size(), max);
    w.le(static_cast<std::uint16_t>(n));
    w.bytes(std::as_bytes(std::span(s.data(), n)));
}

std::string read_string(ByteReader& r, std::size_t max) {
    const auto n = r.le<std::uint16_t>();
    if (n > max) throw std::out_of_range("atlas: string too long");
    const auto b = r.bytes(n);
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

void write_vec3(ByteWriter& w, const glm::vec3& v) {
    for (int i = 0; i < 3; ++i) w.f32(v[i]);
}

glm::vec3 read_vec3(ByteReader& r) {
    glm::vec3 v;
    for (int i = 0; i < 3; ++i) v[i] = r.f32();
    return v;
}

bool finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// The horizontal distance between two boxes (0 when they overlap).
float box_distance(const glm::vec3& alo, const glm::vec3& ahi, const glm::vec3& blo, const glm::vec3& bhi) {
    const float dx = std::max({blo.x - ahi.x, alo.x - bhi.x, 0.0f});
    const float dy = std::max({blo.y - ahi.y, alo.y - bhi.y, 0.0f});
    return std::hypot(dx, dy);
}

Seam moved(Seam s, const glm::vec3& offset) {
    s.face += offset[s.axis];
    s.centre += offset;
    s.lo += offset;
    s.hi += offset;
    return s;
}

} // namespace

std::vector<std::byte> write_atlas_visit(const AtlasVisit& v) {
    ByteWriter w;
    w.le(kVisitMagic);
    w.le(kVisitVersion);
    write_string(w, v.zone, kMaxName);
    w.le(v.zone_id);
    w.le(static_cast<std::uint8_t>(v.outdoor ? 1 : 0));
    const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(v.points.size(), kMaxPoints));
    w.le(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        const auto& p = v.points[i];
        w.le(p.number);
        w.le(p.zone_id);
        w.f32(p.server_x);
        w.f32(p.server_y);
        w.f32(p.server_z);
    }
    return w.take();
}

std::optional<AtlasVisit> read_atlas_visit(std::span<const std::byte> data) {
    try {
        ByteReader r(data);
        if (r.le<std::uint32_t>() != kVisitMagic || r.le<std::uint32_t>() != kVisitVersion) return std::nullopt;
        AtlasVisit v;
        v.zone = read_string(r, kMaxName);
        v.zone_id = r.le<std::uint32_t>();
        v.outdoor = r.le<std::uint8_t>() != 0;
        const auto n = r.le<std::uint32_t>();
        if (n > kMaxPoints || r.remaining() != static_cast<std::size_t>(n) * 20) return std::nullopt;
        for (std::uint32_t i = 0; i < n; ++i) {
            ZonePointTarget p;
            p.number = r.le<std::uint32_t>();
            p.zone_id = r.le<std::uint32_t>();
            p.server_x = r.f32();
            p.server_y = r.f32();
            p.server_z = r.f32();
            if (!std::isfinite(p.server_x) || !std::isfinite(p.server_y) || !std::isfinite(p.server_z))
                return std::nullopt;
            v.points.push_back(p);
        }
        return v;
    } catch (const std::out_of_range&) {
        return std::nullopt;
    }
}

std::optional<float> AtlasZone::ground_at(glm::vec2 p) const {
    if (!valid()) return std::nullopt;
    const float gx = (p.x - grid_lo.x) / cell, gy = (p.y - grid_lo.y) / cell;
    if (!(gx >= 0.0f && gy >= 0.0f && gx < static_cast<float>(nx) && gy < static_cast<float>(ny))) return std::nullopt;
    const float h = ground[static_cast<std::size_t>(static_cast<int>(gy)) * static_cast<std::size_t>(nx) +
                           static_cast<std::size_t>(static_cast<int>(gx))];
    if (!std::isfinite(h)) return std::nullopt;
    return h;
}

std::vector<std::byte> write_atlas_zone(const AtlasZone& z) {
    ByteWriter w;
    w.le(kZoneMagic);
    w.le(kZoneVersion);
    write_string(w, z.zone, kMaxName);
    w.le(z.key);
    write_vec3(w, z.lo);
    write_vec3(w, z.hi);
    w.f32(z.grid_lo.x);
    w.f32(z.grid_lo.y);
    w.f32(z.cell);
    w.f32(z.open_sky);
    const bool ok = z.valid();
    w.le(static_cast<std::uint32_t>(ok ? z.nx : 0));
    w.le(static_cast<std::uint32_t>(ok ? z.ny : 0));
    if (ok) {
        for (const auto* layer : {&z.ground, &z.top, &z.water})
            for (float h : *layer) w.f32(h);
        for (std::uint32_t c : z.color) w.le(c);
    }
    const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(z.lines.size(), kMaxLineCells));
    w.le(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        const auto& c = z.lines[i];
        w.le(c.region);
        write_vec3(w, c.lo);
        write_vec3(w, c.hi);
        w.le(static_cast<std::uint8_t>(c.reference ? 1 : 0));
        w.le(c.zone_point);
        w.le(c.zone_id);
        w.f32(c.server_x);
        w.f32(c.server_y);
        w.f32(c.server_z);
        write_string(w, c.tag, kMaxTag);
    }
    return w.take();
}

std::optional<AtlasZone> read_atlas_zone(std::span<const std::byte> data) {
    try {
        ByteReader r(data);
        if (r.le<std::uint32_t>() != kZoneMagic || r.le<std::uint32_t>() != kZoneVersion) return std::nullopt;
        AtlasZone z;
        z.zone = read_string(r, kMaxName);
        z.key = r.le<std::uint64_t>();
        z.lo = read_vec3(r);
        z.hi = read_vec3(r);
        z.grid_lo.x = r.f32();
        z.grid_lo.y = r.f32();
        z.cell = r.f32();
        z.open_sky = r.f32();
        const auto nx = r.le<std::uint32_t>(), ny = r.le<std::uint32_t>();
        if (!finite(z.lo) || !finite(z.hi) || !std::isfinite(z.grid_lo.x) || !std::isfinite(z.grid_lo.y) ||
            !(z.cell >= 1.0f && z.cell <= 4096.0f) || !(z.open_sky >= 0.0f && z.open_sky <= 1.0f) || nx > static_cast<std::uint32_t>(kAtlasMaxSide) ||
            ny > static_cast<std::uint32_t>(kAtlasMaxSide) || (nx == 0) != (ny == 0))
            return std::nullopt;
        const std::size_t n = static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny);
        if (r.remaining() < n * 16 + 4) return std::nullopt;
        z.nx = static_cast<int>(nx);
        z.ny = static_cast<int>(ny);
        for (auto* layer : {&z.ground, &z.top, &z.water}) {
            layer->resize(n);
            for (float& h : *layer) {
                h = r.f32();
                if (std::isinf(h)) h = std::numeric_limits<float>::quiet_NaN();
            }
        }
        z.color.resize(n);
        for (std::uint32_t& c : z.color) c = r.le<std::uint32_t>();
        const auto cells = r.le<std::uint32_t>();
        if (cells > kMaxLineCells) return std::nullopt;
        for (std::uint32_t i = 0; i < cells; ++i) {
            ZoneLineCell c;
            c.region = r.le<std::uint32_t>();
            c.lo = read_vec3(r);
            c.hi = read_vec3(r);
            c.reference = r.le<std::uint8_t>() != 0;
            c.zone_point = r.le<std::int32_t>();
            c.zone_id = r.le<std::uint32_t>();
            c.server_x = r.f32();
            c.server_y = r.f32();
            c.server_z = r.f32();
            c.tag = read_string(r, kMaxTag);
            if (!finite(c.lo) || !finite(c.hi) || !std::isfinite(c.server_x) || !std::isfinite(c.server_y) ||
                !std::isfinite(c.server_z))
                return std::nullopt;
            z.lines.push_back(std::move(c));
        }
        if (r.remaining() != 0) return std::nullopt;
        return z;
    } catch (const std::out_of_range&) {
        return std::nullopt;
    }
}

std::vector<AtlasEdge> atlas_edges(const AtlasZone& zone, const AtlasVisit& visit,
                                   const std::function<const AtlasZone*(const std::string&)>& other) {
    std::vector<AtlasEdge> out;
    for (const auto& g : collect_neighbour_lines(zone.lines, visit.points, visit.zone_id)) {
        AtlasEdge e;
        e.to_id = g.zone_id;
        e.to = zone_short_name(g.zone_id);
        if (e.to.empty() || e.to == zone.zone) continue;
        e.seam = seam_of(g, zone.lo, zone.hi);
        e.offset = offset_from_targets(g, e.seam);
        e.agree = std::max(e.offset.spread.x, e.offset.spread.y) <= kMaxTargetSpread;
        if (!e.agree)
            if (auto each = atlas_line_offset(zone.lines, visit.points, g.zone_id)) {
                e.offset = *each;
                e.agree = true;
                e.cut = false;
                out.push_back(std::move(e));
                continue;
            }
        const AtlasZone* there = other ? other(e.to) : nullptr;
        if (e.agree && there) {
            std::vector<LineCell> cells;
            for (const auto& c : there->lines) cells.push_back({c.lo, c.hi});
            const glm::vec3 arrival = e.seam.centre - e.offset.offset;
            const auto back = find_return_line(cells, e.seam, arrival);
            const Floors here = [&](const glm::vec3& p) {
                std::vector<float> f;
                if (auto h = zone.ground_at(glm::vec2(p))) f.push_back(*h);
                return f;
            };
            const Floors far = [&](const glm::vec3& p) {
                std::vector<float> f;
                if (auto h = there->ground_at(glm::vec2(p))) f.push_back(*h);
                return f;
            };
            e.offset = refine_offset(e.offset, e.seam, g, back, seam_points(e.seam), here, far);
        }
        out.push_back(std::move(e));
    }
    return out;
}

std::optional<NeighbourOffset> atlas_line_offset(const std::vector<ZoneLineCell>& cells,
                                                 const std::vector<ZonePointTarget>& points, std::uint32_t to_id) {
    // Each region's box and target.
    struct Region {
        glm::vec3 lo{std::numeric_limits<float>::max()}, hi{-std::numeric_limits<float>::max()};
        std::optional<LineTarget> target;
    };
    std::map<std::uint32_t, Region> regions;
    for (const auto& c : cells) {
        std::optional<LineTarget> target;
        if (c.reference) {
            auto it = std::find_if(points.begin(), points.end(), [&](const ZonePointTarget& p) {
                return c.zone_point >= 0 && p.number == static_cast<std::uint32_t>(c.zone_point);
            });
            if (it == points.end() || it->zone_id != to_id) continue;
            target = line_target_from_server(it->server_x, it->server_y, it->server_z);
        } else {
            if (c.zone_id != to_id) continue;
            target = line_target_from_server(c.server_x, c.server_y, c.server_z);
        }
        Region& r = regions[c.region];
        r.lo = glm::min(r.lo, c.lo);
        r.hi = glm::max(r.hi, c.hi);
        r.target = target;
    }
    std::vector<glm::vec3> estimates;
    for (const auto& [id, r] : regions) {
        if (!r.target) continue;
        const glm::vec3 middle = 0.5f * (r.lo + r.hi);
        glm::vec3 e(0.0f);
        for (int i = 0; i < 2; ++i) e[i] = r.target->keep[i] ? 0.0f : middle[i] - r.target->position[i];
        e.z = r.target->keep.z ? 0.0f : r.lo.z - r.target->position.z;
        estimates.push_back(e);
    }
    // The estimate the most others agree with (on x and y), and those that do.
    std::vector<glm::vec3> best;
    for (const auto& c : estimates) {
        std::vector<glm::vec3> with;
        for (const auto& e : estimates)
            if (std::abs(e.x - c.x) <= kMaxTargetSpread && std::abs(e.y - c.y) <= kMaxTargetSpread) with.push_back(e);
        if (with.size() > best.size()) best = std::move(with);
    }
    if (best.size() < 2 || best.size() * 2 <= estimates.size()) return std::nullopt;
    NeighbourOffset o;
    for (int i = 0; i < 3; ++i) {
        std::vector<float> v;
        for (const auto& e : best) v.push_back(e[i]);
        std::sort(v.begin(), v.end());
        const std::size_t m = v.size() / 2;
        o.offset[i] = v.size() % 2 ? v[m] : 0.5f * (v[m - 1] + v[m]);
        o.spread[i] = v.back() - v.front();
    }
    o.estimates = best.size();
    o.detail = std::format("{} of {} lines one by one agree (spread {:.1f} {:.1f})", best.size(), estimates.size(),
                           o.spread.x, o.spread.y);
    return o;
}

AtlasPlan place_atlas(const std::string& current, const AtlasLookup& lookup, float reach, int max_hops,
                      std::size_t max_zones) {
    AtlasPlan plan;
    std::set<std::string> missing;
    auto want = [&](const std::string& name) {
        if (missing.insert(name).second) plan.missing.push_back(name);
    };
    const AtlasZone* here = lookup.zone ? lookup.zone(current) : nullptr;
    const AtlasVisit* here_visit = lookup.visit ? lookup.visit(current) : nullptr;
    if (!here_visit) {
        plan.notes.push_back(std::format("{}: not visited on this server", current));
        return plan;
    }
    if (!here) {
        want(current);
        plan.notes.push_back(std::format("{}: its record is not measured yet", current));
        return plan;
    }
    plan.zones.push_back({current, 0, glm::vec3(0.0f), {}, {}, true});
    std::map<std::string, std::size_t> placed{{current, 0}};
    for (std::size_t next = 0; next < plan.zones.size(); ++next) {
        const AtlasPlacement from = plan.zones[next];
        if (from.hop >= max_hops) continue;
        const AtlasZone* zone = lookup.zone(from.zone);
        const AtlasVisit* visit = lookup.visit(from.zone);
        if (!zone || !visit) continue;
        for (const AtlasEdge& e : atlas_edges(*zone, *visit, lookup.zone)) {
            const AtlasVisit* to_visit = lookup.visit(e.to);
            if (!to_visit || !to_visit->outdoor) continue; // not been there, or no sky: not in the atlas
            if (!e.agree) {
                plan.notes.push_back(std::format("{} -> {}: its zone points disagree, not followed", from.zone, e.to));
                continue;
            }
            const AtlasZone* to = lookup.zone(e.to);
            if (!to) {
                want(e.to);
                continue;
            }
            if (to->open_sky < kAtlasMinOpenSky) {
                plan.notes.push_back(std::format("{}: {:.0f}% of its ground under open sky, left out", e.to,
                                                 to->open_sky * 100.0f));
                continue;
            }
            glm::vec3 step = e.offset.offset;
            if (from.hop == 0 && lookup.measured)
                if (auto m = lookup.measured(e.to)) step = *m;
            const glm::vec3 offset = from.offset + step;
            if (auto it = placed.find(e.to); it != placed.end()) {
                const glm::vec3 had = plan.zones[it->second].offset;
                if (glm::distance(had, offset) > kAtlasDisagree)
                    plan.notes.push_back(std::format("{}: reached from {} at ({:.0f} {:.0f} {:.0f}), placed at ({:.0f} "
                                                     "{:.0f} {:.0f}) from {}",
                                                     e.to, from.zone, offset.x, offset.y, offset.z, had.x, had.y,
                                                     had.z, plan.zones[it->second].parent));
                continue;
            }
            const float away = box_distance(here->lo, here->hi, to->lo + offset, to->hi + offset);
            if (away > reach) continue;
            if (plan.zones.size() >= max_zones) {
                plan.notes.push_back(std::format("{}: past the atlas's {} zones", e.to, max_zones));
                continue;
            }
            placed.emplace(e.to, plan.zones.size());
            plan.zones.push_back({e.to, from.hop + 1, offset, from.zone, moved(e.seam, from.offset), e.cut});
            plan.notes.push_back(std::format("{}: hop {} from {}, offset ({:.1f} {:.1f} {:.1f}), {:.0f} units away; {}",
                                             e.to, from.hop + 1, from.zone, offset.x, offset.y, offset.z, away,
                                             e.offset.detail));
        }
    }
    return plan;
}

std::map<std::string, std::vector<glm::vec4>> atlas_cuts(const AtlasPlan& plan) {
    std::map<std::string, std::vector<glm::vec4>> out;
    const std::string current = plan.zones.empty() ? std::string() : plan.zones.front().zone;
    for (const auto& z : plan.zones) {
        if (z.hop < 1 || !z.cut) continue;
        const glm::vec4 past = seam_clip_plane(z.seam, 0.0f);
        out[z.zone].push_back(past);
        if (z.parent != current) out[z.parent].push_back(-past);
    }
    return out;
}

} // namespace hailstone::game
