#include "render/zone_fog.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "render/horizon.h"

namespace hailstone::render {

namespace {

// Channel `shift` of the field bilinearly at `p` (0..1), clamped at the grid's edge as the sampler is.
float sample(const ZoneFogField& f, glm::vec2 p, int shift) {
    const glm::vec2 u = (p - f.lo) / f.cell - 0.5f;
    const float fx = std::floor(u.x), fy = std::floor(u.y);
    const float tx = u.x - fx, ty = u.y - fy;
    auto at = [&](float x, float y) {
        const int ix = std::clamp(static_cast<int>(x), 0, f.nx - 1), iy = std::clamp(static_cast<int>(y), 0, f.ny - 1);
        const std::uint32_t t =
            f.texels[static_cast<std::size_t>(iy) * static_cast<std::size_t>(f.nx) + static_cast<std::size_t>(ix)];
        return static_cast<float>((t >> shift) & 0xffu) / 255.0f;
    };
    const float a = at(fx, fy) + (at(fx + 1.0f, fy) - at(fx, fy)) * tx;
    const float b = at(fx, fy + 1.0f) + (at(fx + 1.0f, fy + 1.0f) - at(fx, fy + 1.0f)) * tx;
    return a + (b - a) * ty;
}

} // namespace

float ZoneFogField::distance_at(glm::vec2 p) const {
    if (!valid()) return 0.0f;
    const glm::vec2 hi = lo + glm::vec2(static_cast<float>(nx), static_cast<float>(ny)) * cell;
    const float extra = glm::length(glm::max(lo - p, glm::vec2(0.0f)) + glm::max(p - hi, glm::vec2(0.0f)));
    return sample(*this, p, 0) * 255.0f * kZoneFogDistanceStep + extra;
}

float ZoneFogField::floor_at(glm::vec2 p) const {
    if (!valid()) return 0.0f;
    return floor_lo + sample(*this, p, 8) * floor_range;
}

ZoneFogField zone_fog_field(const HorizonField& field) {
    ZoneFogField out;
    if (!field.valid() || field.dist.size() != static_cast<std::size_t>(field.nx) * static_cast<std::size_t>(field.ny))
        return out;
    const std::size_t cells = field.dist.size();
    const int nx = field.nx, ny = field.ny;
    // The fog's floor: the zone's lowest ground by its edge, carried out from the nearest covered square (its lowest
    // terrain, or its water's surface over that), then smoothed. Not the edge's highest terrain (HorizonField::edge):
    // a city wall or a cliff's top there lifted the bank hundreds of units over the open ground and into the sky.
    std::vector<float> low(cells, std::numeric_limits<float>::quiet_NaN());
    std::vector<int> source(cells, -1);
    std::vector<float> reach(cells, std::numeric_limits<float>::infinity());
    for (std::size_t i = 0; i < cells; ++i)
        if (field.covered(i)) {
            float z = std::isfinite(field.zmin[i]) ? field.zmin[i] : field.water[i];
            if (field.wet[i] && std::isfinite(field.water[i])) z = std::isfinite(z) ? std::max(z, field.water[i]) : field.water[i];
            if (!std::isfinite(z)) continue;
            low[i] = z;
            source[i] = static_cast<int>(i);
            reach[i] = 0.0f;
        }
    auto at = [&](int x, int y) { return static_cast<std::size_t>(y) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(x); };
    auto relax = [&](int x, int y, int px, int py) {
        if (px < 0 || py < 0 || px >= nx || py >= ny) return;
        const std::size_t i = at(x, y);
        const int src = source[at(px, py)];
        if (src < 0 || field.covered(i)) return;
        const float d = std::hypot(static_cast<float>(x - src % nx), static_cast<float>(y - src / nx));
        if (d < reach[i]) {
            reach[i] = d;
            source[i] = src;
        }
    };
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x) {
            relax(x, y, x - 1, y);
            relax(x, y, x - 1, y - 1);
            relax(x, y, x, y - 1);
            relax(x, y, x + 1, y - 1);
        }
    for (int y = ny - 1; y >= 0; --y)
        for (int x = nx - 1; x >= 0; --x) {
            relax(x, y, x + 1, y);
            relax(x, y, x + 1, y + 1);
            relax(x, y, x, y + 1);
            relax(x, y, x - 1, y + 1);
        }
    std::vector<float> floor(cells, std::numeric_limits<float>::quiet_NaN());
    for (std::size_t i = 0; i < cells; ++i)
        if (source[i] >= 0) floor[i] = low[static_cast<std::size_t>(source[i])];
    for (int pass = 0; pass < 2; ++pass)
        for (int axis = 0; axis < 2; ++axis) {
            const std::vector<float> was = floor;
            constexpr int kRadius = 2;
            for (int y = 0; y < ny; ++y)
                for (int x = 0; x < nx; ++x) {
                    const std::size_t i = at(x, y);
                    if (!field.open[i]) continue;
                    float sum = 0.0f, n = 0.0f;
                    for (int k = -kRadius; k <= kRadius; ++k) {
                        const int px = axis == 0 ? x + k : x, py = axis == 0 ? y : y + k;
                        if (px < 0 || py < 0 || px >= nx || py >= ny || !std::isfinite(was[at(px, py)])) continue;
                        sum += was[at(px, py)];
                        n += 1.0f;
                    }
                    if (n > 0.0f) floor[i] = sum / n;
                }
        }
    // Never over the edge the land starts from.
    for (std::size_t i = 0; i < cells; ++i)
        if (field.open[i] && std::isfinite(field.edge[i]) && !(floor[i] <= field.edge[i])) floor[i] = field.edge[i];
    // Its range over the open squares (a pocket keeps the fog out, so its floor does not count).
    float lo = std::numeric_limits<float>::infinity(), hi = -lo;
    for (std::size_t i = 0; i < cells; ++i)
        if (field.open[i] && std::isfinite(floor[i])) {
            lo = std::min(lo, floor[i]);
            hi = std::max(hi, floor[i]);
        }
    if (!(lo <= hi)) return out; // no open square: the zone's terrain fills its grid
    out.lo = field.lo;
    out.cell = field.cell;
    out.nx = field.nx;
    out.ny = field.ny;
    out.floor_lo = lo;
    out.floor_range = std::max(hi - lo, 1.0f);
    out.texels.resize(cells);
    for (std::size_t i = 0; i < cells; ++i) {
        // The zone's own squares keep their lowest ground as the floor, as the open squares beside them carry it.
        float d = 0.0f, h = std::isfinite(floor[i]) ? floor[i] : lo;
        if (field.open[i]) d = field.dist[i] * field.cell;
        const auto r = static_cast<std::uint32_t>(std::lround(std::clamp(d / kZoneFogDistanceStep, 0.0f, 255.0f)));
        const auto g = static_cast<std::uint32_t>(std::lround(std::clamp((h - lo) / out.floor_range, 0.0f, 1.0f) * 255.0f));
        out.texels[i] = r | (g << 8) | 0xff000000u;
    }
    return out;
}

namespace {

float smooth01(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

float layer(float over) { return 1.0f - smooth01((over - (kZoneFogTop - kZoneFogSoft)) / (2.0f * kZoneFogSoft)); }

} // namespace

float zone_fog_density(const ZoneFogField& field, glm::vec3 p) {
    if (!field.valid()) return 0.0f;
    const float ramp = smooth01(field.distance_at(glm::vec2(p)) / kZoneFogRamp);
    return kZoneFogDensity * ramp * layer(p.z - field.floor_at(glm::vec2(p)));
}

float zone_fog_density(const ZoneFogField& field, glm::vec3 p, glm::vec3 eye, float eye_past) {
    if (!field.valid()) return 0.0f;
    const float d = std::max(field.distance_at(glm::vec2(p)) - eye_past, 0.0f);
    float floor = field.floor_at(glm::vec2(p));
    floor += (std::min(floor, eye.z - 6.0f) - floor) * smooth01(eye_past / 64.0f);
    const float near = smooth01((glm::distance(p, eye) - kZoneFogClear) / (kZoneFogClearFade - kZoneFogClear));
    return kZoneFogDensity * smooth01(d / kZoneFogRamp) * near * layer(p.z - floor);
}

float zone_fog_transmittance(const ZoneFogField& field, glm::vec3 from, glm::vec3 to) {
    const float length = glm::distance(from, to);
    if (!field.valid() || length <= 0.0f) return 1.0f;
    const glm::vec3 dir = (to - from) / length;
    const float past = field.distance_at(glm::vec2(from));
    // Steps closer together near the camera (t = length x s^2), each sampled at its middle.
    const float n = static_cast<float>(kZoneFogSteps);
    float tau = 0.0f;
    for (int i = 0; i < kZoneFogSteps; ++i) {
        const float s0 = static_cast<float>(i) / n, s1 = static_cast<float>(i + 1) / n;
        const float sm = (s0 + s1) * 0.5f;
        tau += zone_fog_density(field, from + dir * (length * sm * sm), from, past) * length * (s1 * s1 - s0 * s0);
    }
    return std::exp(-tau);
}

} // namespace hailstone::render
