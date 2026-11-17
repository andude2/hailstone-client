#include "app/world_atlas.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <span>
#include <unordered_map>
#include <utility>

#include "assets/texture.h"
#include "assets/zone.h"
#include "core/atomic_file.h"
#include "core/game_dir.h"
#include "core/log.h"
#include "game/collision.h"
#include "platform/paths.h"
#include "render/zone_renderer.h"

namespace hailstone::app {

static_assert(game::kAtlasCell == render::kHorizonCell, "the atlas's squares are the land's");

namespace {

std::string server_folder(std::string_view server) {
    return std::string(server.empty() ? std::string_view("default") : server);
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return std::nullopt;
    return read_text_file(path);
}

std::span<const std::byte> as_bytes(const std::string& s) { return std::as_bytes(std::span(s.data(), s.size())); }

bool write_bytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    return write_file_atomic(path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

// The record's layer over the squares from (x0, y0), w x h of them, moved up by `dz`.
std::vector<float> crop(const game::AtlasZone& z, const std::vector<float>& layer, int x0, int y0, int w, int h, float dz) {
    std::vector<float> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] =
                layer[static_cast<std::size_t>(y + y0) * static_cast<std::size_t>(z.nx) + static_cast<std::size_t>(x + x0)] + dz;
    return out;
}

std::vector<std::uint32_t> crop_color(const game::AtlasZone& z, int x0, int y0, int w, int h) {
    std::vector<std::uint32_t> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] =
                z.color[static_cast<std::size_t>(y + y0) * static_cast<std::size_t>(z.nx) + static_cast<std::size_t>(x + x0)];
    return out;
}

std::vector<std::uint8_t> crop_trees(const game::AtlasZone& z, int x0, int y0, int w, int h) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] =
                z.trees[static_cast<std::size_t>(y + y0) * static_cast<std::size_t>(z.nx) + static_cast<std::size_t>(x + x0)];
    return out;
}

float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float linear_to_srgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }

// A texture's average colour (in linear light, weighted by its alpha, back to sRGB; R in the low byte, alpha 255), 0
// when the zone has no such file or it does not decode.
std::uint32_t texture_average(const assets::Zone& zone, const std::string& name) {
    const auto data = zone.read_file(name);
    if (!data) return 0;
    assets::TextureOptions options;
    options.wld_bmp_rows = true;
    const auto image = assets::decode_texture(*data, options);
    if (!image || image->mips.empty()) return 0;
    const assets::Image rgba = assets::to_rgba8(*image);
    if (rgba.mips.empty()) return 0;
    const auto& level = rgba.mips.front();
    const std::size_t texels = std::min<std::size_t>(std::size_t{rgba.width} * rgba.height, level.size() / 4);
    const std::size_t step = std::max<std::size_t>(1, texels / 4096); // a few thousand texels are plenty
    glm::dvec3 sum(0.0);
    double weight = 0.0;
    for (std::size_t i = 0; i < texels; i += step) {
        const auto* p = &level[i * 4];
        const double a = static_cast<double>(std::to_integer<int>(p[3])) / 255.0;
        for (int k = 0; k < 3; ++k)
            sum[k] += a * srgb_to_linear(static_cast<float>(std::to_integer<int>(p[k])) / 255.0f);
        weight += a;
    }
    if (!(weight > 0.0)) return 0;
    std::uint32_t out = 0xff000000u;
    for (int k = 0; k < 3; ++k) {
        const float c = linear_to_srgb(static_cast<float>(sum[k] / weight));
        out |= static_cast<std::uint32_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f)) << (8 * k);
    }
    return out;
}

} // namespace

std::vector<std::uint32_t> measure_ground_color(const assets::Zone& zone, glm::vec2 lo, float cell, int nx, int ny,
                                                const std::vector<float>& ground, const std::atomic<bool>* cancelled) {
    const auto n = static_cast<std::size_t>(std::max(nx, 0)) * static_cast<std::size_t>(std::max(ny, 0));
    std::vector<std::uint32_t> color(n, 0);
    if (n == 0 || ground.size() != n) return color;
    std::vector<float> best(n, kAtlasColorReach);
    std::map<std::string, std::uint32_t> averages;
    const auto liquid = render::classic_liquid_triangles(zone);
    for (std::size_t mi = 0; mi < zone.terrain.size(); ++mi) {
        if (cancelled && *cancelled) return color;
        const assets::Mesh& mesh = zone.terrain[mi];
        for (const assets::MeshPart& part : mesh.parts) {
            if (part.material >= mesh.materials.size()) continue;
            const assets::Material& m = mesh.materials[part.material];
            // The ground's own textures: not water, not what is not drawn or is cut out (leaves) or glows.
            using assets::BlendMode, assets::EqgShader;
            if (m.frames.empty() || m.blend == BlendMode::Invisible || m.blend == BlendMode::Masked ||
                m.blend == BlendMode::Additive || m.eqg_shader == EqgShader::Water || m.eqg_shader == EqgShader::WaterFall)
                continue;
            auto [it, fresh] = averages.try_emplace(m.frames.front(), 0u);
            if (fresh) it->second = texture_average(zone, m.frames.front());
            const std::uint32_t average = it->second;
            if (average == 0) continue;
            for (std::uint32_t i = part.first_index; i + 2 < part.first_index + part.index_count && i + 2 < mesh.indices.size();
                 i += 3) {
                if (mi < liquid.size() && i / 3 < liquid[mi].size() && liquid[mi][i / 3] != 0) continue;
                const std::uint32_t va = mesh.indices[i], vb = mesh.indices[i + 1], vc = mesh.indices[i + 2];
                if (va >= mesh.vertices.size() || vb >= mesh.vertices.size() || vc >= mesh.vertices.size()) continue;
                const glm::vec3 c[3] = {mesh.vertices[va].position, mesh.vertices[vb].position, mesh.vertices[vc].position};
                const glm::vec2 a(c[0]), b(c[1]), d(c[2]);
                const float area = (b.x - a.x) * (d.y - a.y) - (d.x - a.x) * (b.y - a.y);
                if (std::abs(area) < 1.0f) continue;
                const glm::vec2 tlo = glm::min(a, glm::min(b, d)), thi = glm::max(a, glm::max(b, d));
                const int x0 = std::max(0, static_cast<int>(std::floor((tlo.x - lo.x) / cell)));
                const int y0 = std::max(0, static_cast<int>(std::floor((tlo.y - lo.y) / cell)));
                const int x1 = std::min(nx - 1, static_cast<int>(std::floor((thi.x - lo.x) / cell)));
                const int y1 = std::min(ny - 1, static_cast<int>(std::floor((thi.y - lo.y) / cell)));
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x) {
                        const auto at = static_cast<std::size_t>(y) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(x);
                        if (!std::isfinite(ground[at])) continue;
                        const glm::vec2 p = lo + (glm::vec2(static_cast<float>(x), static_cast<float>(y)) + 0.5f) * cell;
                        const float w1 = ((p.x - a.x) * (d.y - a.y) - (d.x - a.x) * (p.y - a.y)) / area;
                        const float w2 = ((b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y)) / area;
                        const float w0 = 1.0f - w1 - w2;
                        if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                        const float off = std::abs(w0 * c[0].z + w1 * c[1].z + w2 * c[2].z - ground[at]);
                        if (off < best[at]) {
                            best[at] = off;
                            color[at] = average;
                        }
                    }
            }
        }
    }
    return color;
}

std::filesystem::path atlas_visit_path(std::string_view server, std::string_view zone) {
    return platform::cache_dir() / "atlas" / server_folder(server) / (std::string(zone) + ".visit");
}

std::filesystem::path atlas_zone_path(std::string_view zone) {
    return platform::cache_dir() / "atlas" / "zones" / (std::string(zone) + ".bin");
}

std::uint64_t atlas_zone_key(const GameDir& game, std::string_view zone) {
    for (const char* ext : {".s3d", ".eqg"})
        if (auto p = game.find(std::string(zone) + ext)) {
            std::error_code ec;
            const auto n = std::filesystem::file_size(*p, ec);
            if (!ec) return static_cast<std::uint64_t>(n);
        }
    return 0;
}

void save_atlas_visit(std::string_view server, const game::AtlasVisit& visit) {
    if (visit.zone.empty()) return;
    const auto path = atlas_visit_path(server, visit.zone);
    const auto bytes = game::write_atlas_visit(visit);
    if (auto old = read_file(path))
        if (old->size() == bytes.size() && std::equal(bytes.begin(), bytes.end(), as_bytes(*old).begin())) return;
    if (write_bytes(path, bytes))
        log::info("world atlas: {} visited ({}, {} zone points)", visit.zone, visit.outdoor ? "outdoor" : "indoor",
                  visit.points.size());
}

std::optional<game::AtlasVisit> load_atlas_visit(std::string_view server, std::string_view zone) {
    const auto text = read_file(atlas_visit_path(server, zone));
    if (!text) return std::nullopt;
    auto v = game::read_atlas_visit(as_bytes(*text));
    if (!v || v->zone != zone) return std::nullopt;
    return v;
}

std::optional<game::AtlasZone> load_atlas_zone(const GameDir& game, std::string_view zone) {
    const auto text = read_file(atlas_zone_path(zone));
    if (!text) return std::nullopt;
    auto z = game::read_atlas_zone(as_bytes(*text));
    if (!z || z->zone != zone) return std::nullopt;
    const std::uint64_t key = atlas_zone_key(game, zone);
    if (key == 0 || key != z->key) return std::nullopt;
    return z;
}

std::vector<float> measure_water(const assets::Zone& zone, glm::vec2 lo, float cell, int nx, int ny,
                                 const std::vector<float>& ground, const glm::vec3& offset,
                                 const std::atomic<bool>* cancelled) {
    std::vector<float> water(static_cast<std::size_t>(std::max(nx, 0)) * static_cast<std::size_t>(std::max(ny, 0)),
                             std::numeric_limits<float>::quiet_NaN());
    if (water.empty() || ground.size() != water.size()) return water;
    // Its water's surfaces, at each square's middle inside one (the highest).
    auto mark = [&](const glm::vec3& q) {
        const float fx = std::floor((q.x - lo.x) / cell), fy = std::floor((q.y - lo.y) / cell);
        if (!(fx >= 0.0f && fy >= 0.0f && fx < static_cast<float>(nx) && fy < static_cast<float>(ny))) return;
        float& at = water[static_cast<std::size_t>(fy) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(fx)];
        if (!std::isfinite(at) || q.z > at) at = q.z;
    };
    auto surface = [&](const assets::Mesh& mesh, std::size_t first) {
        glm::vec3 c[3];
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t v = mesh.indices[first + static_cast<std::size_t>(k)];
            if (v >= mesh.vertices.size()) return;
            c[k] = mesh.vertices[v].position + offset;
        }
        // Along its edges and at its middle (a canal's strip narrower than a square), then at the middles of the
        // squares inside it.
        for (int k = 0; k < 3; ++k) {
            const glm::vec3 from = c[k], to = c[(k + 1) % 3];
            const int steps = std::max(1, static_cast<int>(std::ceil(glm::length(glm::vec2(to - from)) / (0.5f * cell))));
            for (int i = 0; i <= steps; ++i) mark(glm::mix(from, to, static_cast<float>(i) / static_cast<float>(steps)));
        }
        mark((c[0] + c[1] + c[2]) / 3.0f);
        const glm::vec2 a(c[0]), b(c[1]), d(c[2]);
        const float area = (b.x - a.x) * (d.y - a.y) - (d.x - a.x) * (b.y - a.y);
        if (std::abs(area) < 1.0f) return;
        const glm::vec2 tlo = glm::min(a, glm::min(b, d)), thi = glm::max(a, glm::max(b, d));
        const int x0 = std::max(0, static_cast<int>(std::floor((tlo.x - lo.x) / cell)));
        const int y0 = std::max(0, static_cast<int>(std::floor((tlo.y - lo.y) / cell)));
        const int x1 = std::min(nx - 1, static_cast<int>(std::floor((thi.x - lo.x) / cell)));
        const int y1 = std::min(ny - 1, static_cast<int>(std::floor((thi.y - lo.y) / cell)));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const glm::vec2 p = lo + (glm::vec2(static_cast<float>(x), static_cast<float>(y)) + 0.5f) * cell;
                const float w1 = ((p.x - a.x) * (d.y - a.y) - (d.x - a.x) * (p.y - a.y)) / area;
                const float w2 = ((b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y)) / area;
                const float w0 = 1.0f - w1 - w2;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                mark(glm::vec3(p, w0 * c[0].z + w1 * c[1].z + w2 * c[2].z));
            }
    };
    // The classic regions' water surfaces.
    const auto liquid = render::classic_liquid_triangles(zone);
    for (std::size_t m = 0; m < zone.terrain.size() && m < liquid.size(); ++m) {
        const assets::Mesh& mesh = zone.terrain[m];
        for (std::size_t t = 0; t < liquid[m].size(); ++t)
            if (liquid[m][t] == render::kLiquidWater && 3 * t + 2 < mesh.indices.size()) surface(mesh, 3 * t);
    }
    // An EQG zone's: its terrain's RegionWater surfaces, level ones only (one draped over a fall is no body of water).
    for (const assets::Mesh& mesh : zone.terrain)
        for (const assets::MeshPart& part : mesh.parts) {
            if (part.material >= mesh.materials.size() || mesh.materials[part.material].eqg_shader != assets::EqgShader::Water)
                continue;
            for (std::uint32_t i = part.first_index; i + 2 < part.first_index + part.index_count && i + 2 < mesh.indices.size();
                 i += 3) {
                const std::uint32_t va = mesh.indices[i], vb = mesh.indices[i + 1], vc = mesh.indices[i + 2];
                if (va >= mesh.vertices.size() || vb >= mesh.vertices.size() || vc >= mesh.vertices.size()) continue;
                const glm::vec3 n = glm::cross(mesh.vertices[vb].position - mesh.vertices[va].position,
                                               mesh.vertices[vc].position - mesh.vertices[va].position);
                const float len = glm::length(n);
                if (!(len > 1e-4f) || std::abs(n.z) < 0.6f * len) continue;
                surface(mesh, i);
            }
        }
    // And where a water region stands over the ground (a harbour whose surface is not a liquid triangle the regions
    // mark): up from the ground while in water, the surface found to half a unit.
    if (!zone.regions.empty()) {
        std::vector<std::uint8_t> wet(zone.regions.region_count() + 1, 0);
        for (std::uint32_t r = 1; r < wet.size(); ++r) wet[r] = zone.regions.info(r).is_water() ? 1 : 0;
        auto in_water = [&](const glm::vec3& p) {
            const std::uint32_t r = zone.regions.region_at(p - offset);
            return r < wet.size() && wet[r];
        };
        constexpr float kStep = 8.0f, kDeepest = 1000.0f;
        for (int y = 0; y < ny; ++y) {
            if (cancelled && *cancelled) return water;
            for (int x = 0; x < nx; ++x) {
                const auto i = static_cast<std::size_t>(y) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(x);
                if (!std::isfinite(ground[i])) continue;
                const glm::vec2 p = lo + (glm::vec2(static_cast<float>(x), static_cast<float>(y)) + 0.5f) * cell;
                const float start = ground[i] + 1.0f;
                if (!in_water(glm::vec3(p, start))) continue;
                float wet_z = start, dry_z = start + kStep; // in the water, and the next sample up
                while (dry_z < start + kDeepest && dry_z < zone.bounds_max.z + offset.z && in_water(glm::vec3(p, dry_z))) {
                    wet_z = dry_z;
                    dry_z += kStep;
                }
                while (dry_z - wet_z > 0.5f) {
                    const float mid = 0.5f * (wet_z + dry_z);
                    (in_water(glm::vec3(p, mid)) ? wet_z : dry_z) = mid;
                }
                if (!std::isfinite(water[i]) || wet_z > water[i]) water[i] = wet_z;
            }
        }
    }
    return water;
}

std::vector<std::uint8_t> measure_trees(const assets::Zone& zone, glm::vec2 lo, float cell, int nx, int ny) {
    const auto n = static_cast<std::size_t>(std::max(nx, 0)) * static_cast<std::size_t>(std::max(ny, 0));
    std::vector<std::uint8_t> trees(n, 0);
    if (n == 0 || !(cell > 0.0f)) return trees;
    std::unordered_map<std::string, float> heights; // per actor: its mesh's height if a plant, else 0
    for (const auto& p : zone.placements) {
        auto it = zone.objects.find(p.actor);
        if (it == zone.objects.end()) continue;
        auto [h, fresh] = heights.try_emplace(p.actor, 0.0f);
        if (fresh && render::is_plant(p.actor, it->second)) h->second = render::mesh_height(it->second);
        if (!(h->second * glm::length(glm::vec3(p.transform[2])) >= render::kLandTreeMinHeight)) continue;
        const glm::vec2 at = (glm::vec2(p.transform[3]) - lo) / cell;
        if (!(at.x >= 0.0f && at.y >= 0.0f && at.x < static_cast<float>(nx) && at.y < static_cast<float>(ny))) continue;
        std::uint8_t& t = trees[static_cast<std::size_t>(at.y) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(at.x)];
        if (t < 255) ++t;
    }
    return trees;
}

game::AtlasZone measure_atlas_zone(assets::Zone& zone, std::uint64_t key, const std::atomic<bool>* cancelled) {
    game::AtlasZone out;
    out.zone = zone.name;
    out.key = key;
    out.lo = zone.bounds_min;
    out.hi = zone.bounds_max;
    out.lines = game::zone_line_cells(zone.regions, zone.bounds_min, zone.bounds_max);
    const float cell = game::kAtlasCell;
    out.cell = cell;
    out.grid_lo = glm::floor(glm::vec2(zone.bounds_min) / cell) * cell;
    const int nx = std::min(game::kAtlasMaxSide, static_cast<int>(std::ceil((zone.bounds_max.x - out.grid_lo.x) / cell)));
    const int ny = std::min(game::kAtlasMaxSide, static_cast<int>(std::ceil((zone.bounds_max.y - out.grid_lo.y) / cell)));
    if (nx <= 0 || ny <= 0) return out;
    const auto n = static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    out.ground.assign(n, nan);
    out.top.assign(n, nan);
    out.water.assign(n, nan);
    out.color.assign(n, 0u);
    out.trees.assign(n, 0);

    // Its terrain alone: its placed objects (trees, houses) are not its ground.
    auto placements = std::move(zone.placements);
    zone.placements.clear();
    const game::CollisionMesh terrain = game::CollisionMesh::build(zone);
    zone.placements = std::move(placements);
    const float top = zone.bounds_max.z + 10.0f;
    constexpr int kSamples = 2;
    std::vector<float> hits;
    std::size_t with_ground = 0, open = 0;
    for (int y = 0; y < ny; ++y) {
        if (cancelled && *cancelled) return {};
        for (int x = 0; x < nx; ++x) {
            const glm::vec2 corner = out.grid_lo + glm::vec2(static_cast<float>(x), static_cast<float>(y)) * cell;
            hits.clear();
            for (int j = 0; j < kSamples; ++j)
                for (int i = 0; i < kSamples; ++i) {
                    const glm::vec2 p = corner + (glm::vec2(static_cast<float>(i), static_cast<float>(j)) + 0.5f) *
                                                     (cell / static_cast<float>(kSamples));
                    if (auto h = terrain.ground_below(glm::vec3(p, top))) hits.push_back(*h);
                }
            if (hits.size() < 2) continue;
            std::sort(hits.begin(), hits.end());
            const std::size_t m = hits.size() / 2;
            const auto i = static_cast<std::size_t>(y) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(x);
            out.ground[i] = hits.size() % 2 ? hits[m] : 0.5f * (hits[m - 1] + hits[m]);
            out.top[i] = hits.back();
            ++with_ground;
            const glm::vec3 at(corner + 0.5f * cell, out.ground[i] + 2.0f);
            const auto roof = terrain.roof_above(at);
            if (!roof || *roof > at.z + game::kOpenSkyHeight) ++open;
        }
    }

    // Its water.
    out.water = measure_water(zone, out.grid_lo, cell, nx, ny, out.ground, glm::vec3(0.0f), cancelled);
    if (cancelled && *cancelled) return {};
    out.color = measure_ground_color(zone, out.grid_lo, cell, nx, ny, out.ground, cancelled);
    if (cancelled && *cancelled) return {};
    out.trees = measure_trees(zone, out.grid_lo, cell, nx, ny);
    out.open_sky = with_ground ? static_cast<float>(open) / static_cast<float>(with_ground) : 0.0f;
    out.nx = nx;
    out.ny = ny;
    return out;
}

AtlasLand atlas_land(const GameDir& game, std::string_view server, std::string_view zone,
                     const std::vector<render::HorizonCacheEntry>& measured) {
    AtlasLand out;
    std::map<std::string, std::optional<game::AtlasZone>> zones;
    std::map<std::string, std::optional<game::AtlasVisit>> visits;
    game::AtlasLookup lookup;
    lookup.zone = [&](const std::string& name) -> const game::AtlasZone* {
        auto [it, fresh] = zones.try_emplace(name);
        if (fresh) it->second = load_atlas_zone(game, name);
        return it->second ? &*it->second : nullptr;
    };
    lookup.visit = [&](const std::string& name) -> const game::AtlasVisit* {
        auto [it, fresh] = visits.try_emplace(name);
        if (fresh) it->second = load_atlas_visit(server, name);
        return it->second ? &*it->second : nullptr;
    };
    lookup.measured = [&](const std::string& name) -> std::optional<glm::vec3> {
        for (const auto& e : measured)
            if (e.neighbour == name) return e.offset;
        return std::nullopt;
    };
    const game::AtlasPlan plan = game::place_atlas(std::string(zone), lookup, kAtlasReach, kAtlasMaxHops, kAtlasMaxZones);
    for (const auto& note : plan.notes) log::info("world atlas: {}", note);
    out.missing = plan.missing;
    out.cuts = game::atlas_cuts(plan);
    auto cut = [&](const render::HorizonSource& s) {
        auto it = out.cuts.find(s.name);
        return it == out.cuts.end() ? s : render::cut_horizon_source(s, it->second);
    };
    for (const auto& e : measured) out.sources.push_back(std::make_shared<const render::HorizonSource>(cut(e.source)));
    if (!plan.zones.empty()) {
        const game::AtlasZone* here = lookup.zone(plan.zones.front().zone);
        const float margin = kAtlasReach + render::kHorizonSourceFade;
        const glm::vec2 want_lo = glm::vec2(here->lo) - margin, want_hi = glm::vec2(here->hi) + margin;
        for (const auto& p : plan.zones) {
            if (p.hop < 1) continue;
            if (std::any_of(measured.begin(), measured.end(), [&](const auto& e) { return e.neighbour == p.zone; }))
                continue; // the neighbour zones measured it themselves
            const game::AtlasZone* z = lookup.zone(p.zone);
            if (!z || !z->valid()) continue;
            // Only the squares within the land's reach of the current zone.
            const glm::vec2 lo = z->grid_lo + glm::vec2(p.offset);
            const int x0 = std::clamp(static_cast<int>(std::floor((want_lo.x - lo.x) / z->cell)), 0, z->nx);
            const int y0 = std::clamp(static_cast<int>(std::floor((want_lo.y - lo.y) / z->cell)), 0, z->ny);
            const int x1 = std::clamp(static_cast<int>(std::ceil((want_hi.x - lo.x) / z->cell)), 0, z->nx);
            const int y1 = std::clamp(static_cast<int>(std::ceil((want_hi.y - lo.y) / z->cell)), 0, z->ny);
            if (x1 <= x0 || y1 <= y0) continue;
            const int w = x1 - x0, h = y1 - y0;
            render::HorizonSource s = render::horizon_source(
                p.zone, lo + glm::vec2(static_cast<float>(x0), static_cast<float>(y0)) * z->cell, z->cell, w, h,
                crop(*z, z->ground, x0, y0, w, h, p.offset.z), crop(*z, z->top, x0, y0, w, h, p.offset.z),
                crop(*z, z->water, x0, y0, w, h, p.offset.z), crop_color(*z, x0, y0, w, h),
                crop_trees(*z, x0, y0, w, h));
            s = cut(s);
            s.draws_water = true;
            if (!s.valid()) continue;
            out.sources.push_back(std::make_shared<const render::HorizonSource>(std::move(s)));
            ++out.placed;
        }
    }
    render::sort_sources(out.sources);
    if (out.placed || !out.missing.empty())
        log::info("world atlas: {} zones of the atlas shape the land round {}{}", out.placed, zone,
                  out.missing.empty() ? "" : std::format(", {} to measure", out.missing.size()));
    return out;
}

} // namespace hailstone::app
