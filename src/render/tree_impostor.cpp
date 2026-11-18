#include "render/tree_impostor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace hailstone::render {

namespace {

struct Sample {
    float depth = std::numeric_limits<float>::max();
    glm::vec3 rgb{0.0f};
    bool hit = false;
};

// The texel under `uv` (wrapping, nearest), RGBA 0..255; false where the picture cannot be read.
bool texel(const assets::Image& image, glm::vec2 uv, std::array<std::uint8_t, 4>& out) {
    if (image.format != assets::PixelFormat::RGBA8 || image.mips.empty() || image.width == 0 || image.height == 0)
        return false;
    const auto& level = image.mips.front();
    if (level.size() < static_cast<std::size_t>(image.width) * image.height * 4) return false;
    const float u = uv.x - std::floor(uv.x), v = uv.y - std::floor(uv.y);
    if (!std::isfinite(u) || !std::isfinite(v)) return false;
    const auto x = std::min(static_cast<std::uint32_t>(u * static_cast<float>(image.width)), image.width - 1);
    const auto y = std::min(static_cast<std::uint32_t>(v * static_cast<float>(image.height)), image.height - 1);
    const std::size_t at = (static_cast<std::size_t>(y) * image.width + x) * 4;
    for (int k = 0; k < 4; ++k) out[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(level[at + static_cast<std::size_t>(k)]);
    return true;
}

} // namespace

assets::Mesh impostor_cards(glm::vec3 lo, glm::vec3 hi, const std::string& texture_name) {
    assets::Mesh mesh;
    // The two cards, crossed at the middle.
    mesh.name = texture_name;
    assets::Material m;
    m.name = texture_name;
    m.blend = assets::BlendMode::Masked;
    m.frames = {texture_name};
    m.two_sided = true;
    mesh.materials.push_back(m);
    const glm::vec3 mid = 0.5f * (lo + hi);
    auto vertex = [&](glm::vec3 p, glm::vec2 uv) {
        assets::Vertex v;
        v.position = p;
        v.normal = glm::vec3(0.0f, 0.0f, 1.0f);
        v.uv = uv;
        mesh.vertices.push_back(v);
    };
    vertex({lo.x, mid.y, hi.z}, {0.0f, 0.0f});
    vertex({hi.x, mid.y, hi.z}, {0.5f, 0.0f});
    vertex({hi.x, mid.y, lo.z}, {0.5f, 1.0f});
    vertex({lo.x, mid.y, lo.z}, {0.0f, 1.0f});
    vertex({mid.x, hi.y, hi.z}, {0.5f, 0.0f});
    vertex({mid.x, lo.y, hi.z}, {1.0f, 0.0f});
    vertex({mid.x, lo.y, lo.z}, {1.0f, 1.0f});
    vertex({mid.x, hi.y, lo.z}, {0.5f, 1.0f});
    mesh.indices = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    assets::MeshPart part;
    part.first_index = 0;
    part.index_count = static_cast<std::uint32_t>(mesh.indices.size());
    part.material = 0;
    part.solid = false;
    mesh.parts.push_back(part);
    return mesh;
}

std::optional<TreeImpostor> tree_impostor(const assets::Mesh& tree, const ImpostorTextureLookup& texture,
                                          const std::string& texture_name) {
    // The triangles drawn, with their pictures.
    struct Tri {
        glm::vec3 p[3];
        glm::vec2 uv[3];
        const assets::Image* image;
        bool masked;
    };
    std::vector<Tri> tris;
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    for (const auto& part : tree.parts) {
        if (part.material >= tree.materials.size()) continue;
        const assets::Material& m = tree.materials[part.material];
        if (m.blend == assets::BlendMode::Invisible || m.blend == assets::BlendMode::Additive) continue;
        const assets::Image* image = texture ? texture(m) : nullptr;
        if (!image) continue;
        for (std::uint32_t i = 0; i + 2 < part.index_count; i += 3) {
            Tri t{};
            bool valid = true;
            for (int k = 0; k < 3; ++k) {
                const std::size_t at = static_cast<std::size_t>(part.first_index) + i + static_cast<std::uint32_t>(k);
                if (at >= tree.indices.size() || tree.indices[at] >= tree.vertices.size()) {
                    valid = false;
                    break;
                }
                const assets::Vertex& v = tree.vertices[tree.indices[at]];
                t.p[k] = v.position;
                t.uv[k] = v.uv;
                valid &= std::isfinite(v.position.x) && std::isfinite(v.position.y) && std::isfinite(v.position.z);
            }
            if (!valid) continue;
            t.image = image;
            t.masked = m.blend == assets::BlendMode::Masked;
            for (const glm::vec3& p : t.p) {
                lo = glm::min(lo, p);
                hi = glm::max(hi, p);
            }
            tris.push_back(t);
        }
    }
    if (tris.empty()) return std::nullopt;
    const glm::vec3 size = hi - lo;
    if (!(size.z > 0.0f) || !(size.x > 0.0f) || !(size.y > 0.0f)) return std::nullopt;

    constexpr std::uint32_t kS = kImpostorSize * kImpostorSamples;
    TreeImpostor out;
    out.lo = lo;
    out.hi = hi;
    out.image.width = kImpostorSize * 2;
    out.image.height = kImpostorSize;
    out.image.format = assets::PixelFormat::RGBA8;
    std::vector<std::byte> pixels(static_cast<std::size_t>(out.image.width) * out.image.height * 4);
    glm::dvec3 mean_sum(0.0);
    double mean_count = 0.0;
    std::vector<std::array<float, 4>> texels(static_cast<std::size_t>(out.image.width) * out.image.height);

    // view 0: seen from -y looking along +y (x to the right); view 1: seen from -x looking along +x (-y to the right).
    for (int view = 0; view < 2; ++view) {
        std::vector<Sample> samples(static_cast<std::size_t>(kS) * kS);
        auto project = [&](const glm::vec3& p) {
            const float across = view == 0 ? (p.x - lo.x) / size.x : (hi.y - p.y) / size.y;
            const float down = (hi.z - p.z) / size.z;
            const float depth = view == 0 ? p.y : p.x;
            return glm::vec3(across * static_cast<float>(kS), down * static_cast<float>(kS), depth);
        };
        const float dlo = view == 0 ? lo.y : lo.x, dsize = view == 0 ? size.y : size.x;
        for (const Tri& t : tris) {
            const glm::vec3 a = project(t.p[0]), b = project(t.p[1]), c = project(t.p[2]);
            const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (std::abs(area) < 1e-6f) continue; // edge on
            const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
            const int x1 = std::min(static_cast<int>(kS) - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
            const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
            const int y1 = std::min(static_cast<int>(kS) - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
                    const float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
                    const float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
                    const float w2 = 1.0f - w0 - w1;
                    if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                    const float depth = w0 * a.z + w1 * b.z + w2 * c.z;
                    Sample& s = samples[static_cast<std::size_t>(y) * kS + static_cast<std::size_t>(x)];
                    if (depth >= s.depth) continue;
                    std::array<std::uint8_t, 4> rgba{};
                    if (!texel(*t.image, w0 * t.uv[0] + w1 * t.uv[1] + w2 * t.uv[2], rgba)) continue;
                    if (t.masked && rgba[3] < 128) continue;
                    // The far side of the crown a little darker: some depth to a flat picture.
                    const float shade = 1.0f - 0.25f * std::clamp((depth - dlo) / dsize, 0.0f, 1.0f);
                    s.depth = depth;
                    s.rgb = glm::vec3(rgba[0], rgba[1], rgba[2]) * shade;
                    s.hit = true;
                }
        }
        // Each texel averages its samples.
        for (std::uint32_t y = 0; y < kImpostorSize; ++y)
            for (std::uint32_t x = 0; x < kImpostorSize; ++x) {
                glm::vec3 sum(0.0f);
                std::uint32_t hits = 0;
                for (std::uint32_t sy = 0; sy < kImpostorSamples; ++sy)
                    for (std::uint32_t sx = 0; sx < kImpostorSamples; ++sx) {
                        const Sample& s = samples[static_cast<std::size_t>(y * kImpostorSamples + sy) * kS +
                                                  x * kImpostorSamples + sx];
                        if (!s.hit) continue;
                        sum += s.rgb;
                        ++hits;
                    }
                auto& t = texels[static_cast<std::size_t>(y) * out.image.width + static_cast<std::size_t>(view) * kImpostorSize + x];
                if (hits == 0) {
                    t = {0.0f, 0.0f, 0.0f, 0.0f};
                    continue;
                }
                const glm::vec3 rgb = sum / static_cast<float>(hits);
                t = {rgb.r, rgb.g, rgb.b,
                     255.0f * static_cast<float>(hits) / static_cast<float>(kImpostorSamples * kImpostorSamples)};
                mean_sum += glm::dvec3(rgb);
                mean_count += 1.0;
            }
    }
    if (mean_count == 0.0) return std::nullopt;
    const glm::vec3 mean(mean_sum / mean_count);
    for (std::size_t i = 0; i < texels.size(); ++i) {
        const auto& t = texels[i];
        const glm::vec3 rgb = t[3] > 0.0f ? glm::vec3(t[0], t[1], t[2]) : mean;
        pixels[i * 4 + 0] = static_cast<std::byte>(std::lround(std::clamp(rgb.r, 0.0f, 255.0f)));
        pixels[i * 4 + 1] = static_cast<std::byte>(std::lround(std::clamp(rgb.g, 0.0f, 255.0f)));
        pixels[i * 4 + 2] = static_cast<std::byte>(std::lround(std::clamp(rgb.b, 0.0f, 255.0f)));
        pixels[i * 4 + 3] = static_cast<std::byte>(std::lround(std::clamp(t[3], 0.0f, 255.0f)));
    }
    out.image.mips.push_back(std::move(pixels));

    out.mesh = impostor_cards(lo, hi, texture_name);
    return out;
}

} // namespace hailstone::render
