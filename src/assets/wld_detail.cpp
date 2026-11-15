// Classic WLD detail textures (assets/wld_detail.h).

#include "assets/wld_detail.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "core/byte_io.h"
#include "core/msvc_atoi.h"

namespace hailstone::assets::wld {

namespace {

// Little-endian fields read through ByteReader (whatever the host's byte order); `at` is within the 54 bytes checked.
std::uint32_t le32(std::span<const std::byte> d, std::size_t at) {
    ByteReader r(d);
    r.seek(at);
    return r.le<std::uint32_t>();
}

std::uint16_t le16(std::span<const std::byte> d, std::size_t at) {
    ByteReader r(d);
    r.seek(at);
    return r.le<std::uint16_t>();
}

// One palette entry, "<n>, <scale>, <value>, <file>": the loader cuts the string at its first three commas (strcspn),
// takes atoi of the second and third fields, and the file two characters after the third comma. A string without
// three commas would run past its end in the original; here it gives no file.
DetailLayer palette_layer(const std::string& entry) {
    DetailLayer layer;
    std::size_t c1 = entry.find(',');
    if (c1 == std::string::npos) return layer;
    std::size_t c2 = entry.find(',', c1 + 1);
    if (c2 == std::string::npos) return layer;
    std::size_t c3 = entry.find(',', c2 + 1);
    if (c3 == std::string::npos) return layer;
    const auto scale = core::msvc_atoi(std::string_view(entry).substr(c1 + 1, c2 - c1 - 1));
    layer.uv_scale = palette_uv_scale(static_cast<float>(scale));
    if (c3 + 2 <= entry.size()) layer.texture = entry.substr(c3 + 2);
    return layer;
}

} // namespace

void apply_detail_names(const std::vector<std::string>& names, Material& m) {
    if (names.size() == 2) {
        const std::string& second = names[1];
        if (second.find("_LAYER") != std::string::npos) return; // a character layer, no detail
        std::size_t at = second.find("_DETAIL");
        if (at == std::string::npos) return;
        DetailLayer layer;
        layer.texture = second.substr(0, at);
        // atof of the text after "_DETAIL_".
        layer.uv_scale = at + 8 <= second.size() ? static_cast<float>(std::atof(second.c_str() + at + 8)) : 0.0f;
        m.detail_layers = {std::move(layer)};
        m.detail_palette.clear();
    } else if (names.size() > 2 && names.size() <= 16) {
        m.detail_palette = names[1];
        m.detail_layers.clear();
        for (std::size_t i = 2; i < names.size(); ++i) m.detail_layers.push_back(palette_layer(names[i]));
    }
}

std::optional<PaletteMap> decode_palette_map(std::span<const std::byte> bmp) {
    if (bmp.size() < 54 || bmp[0] != std::byte{'B'} || bmp[1] != std::byte{'M'}) return std::nullopt;
    std::uint32_t offset = le32(bmp, 10), header = le32(bmp, 14);
    if (header < 40) return std::nullopt;
    auto width = static_cast<std::int32_t>(le32(bmp, 18)), height = static_cast<std::int32_t>(le32(bmp, 22));
    std::uint16_t bits = le16(bmp, 28);
    std::uint32_t compression = le32(bmp, 30);
    // INT32_MIN is refused before it is negated (its absolute value does not fit).
    if (bits != 8 || compression != 0 || width <= 0 || height == 0 || height == INT32_MIN || width > 4096 ||
        height > 4096 || height < -4096)
        return std::nullopt;
    // A negative height stores the rows top-down: the last row stored is the bottom one, which is v = 0.
    const bool top_down = height < 0;
    PaletteMap map;
    map.width = static_cast<std::uint32_t>(width);
    map.height = static_cast<std::uint32_t>(top_down ? -height : height);
    std::size_t stride = (map.width + 3u) & ~std::size_t{3};
    if (offset > bmp.size() || bmp.size() - offset < stride * (map.height - 1) + map.width) return std::nullopt;
    map.pixels.resize(std::size_t(map.width) * map.height);
    for (std::uint32_t y = 0; y < map.height; ++y) {
        const std::uint32_t stored = top_down ? map.height - 1 - y : y;
        std::memcpy(map.pixels.data() + std::size_t(y) * map.width, bmp.data() + offset + stored * stride, map.width);
    }
    return map;
}

int palette_entry(const PaletteMap& map, glm::vec2 uv, std::size_t entries) {
    if (map.width == 0 || map.height == 0) return -1;
    // Stored as a 64-bit integer; NaN and values out of range store the integer indefinite, whose low 32 bits are 0.
    auto texel = [](float t, std::uint32_t n) {
        double x = std::nearbyint(static_cast<double>(t) * n);
        auto r = std::isfinite(x) && std::abs(x) < 9.2e18 ? static_cast<std::uint32_t>(static_cast<std::int64_t>(x))
                                                           : 0u;
        return r % n;
    };
    std::uint32_t x = texel(uv.x, map.width), y = texel(uv.y, map.height);
    auto value = static_cast<std::int8_t>(map.pixels[std::size_t(y) * map.width + x]);
    auto entry = static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
    return entry < entries ? static_cast<int>(entry) : -1;
}

std::vector<DetailBatch> detail_batches(const Mesh& mesh, std::uint32_t material, const PaletteMap* map) {
    std::vector<DetailBatch> out;
    if (material >= mesh.materials.size()) return out;
    const auto& m = mesh.materials[material];
    if (m.detail_layers.empty()) return out;
    // The material's triangles, in mesh order.
    std::vector<std::array<std::uint32_t, 3>> tris;
    for (const auto& part : mesh.parts) {
        if (part.material != material) continue;
        for (std::uint32_t i = 0; i + 2 < part.index_count; i += 3) {
            const std::uint32_t* t = &mesh.indices[part.first_index + i];
            if (t[0] < mesh.vertices.size() && t[1] < mesh.vertices.size() && t[2] < mesh.vertices.size())
                tris.push_back({t[0], t[1], t[2]});
        }
    }
    if (tris.empty()) return out;
    auto grow = [&](glm::vec4& box, const std::array<std::uint32_t, 3>& t) {
        for (std::uint32_t v : t) {
            const glm::vec3& p = mesh.vertices[v].position;
            box.x = std::min(box.x, p.x);
            box.y = std::max(box.y, p.x);
            box.z = std::min(box.z, p.y);
            box.w = std::max(box.w, p.y);
        }
    };
    constexpr float kBig = 3.4028235e38f;
    const glm::vec4 empty(kBig, -kBig, kBig, -kBig);
    if (!map) {
        DetailBatch b;
        b.box = empty;
        for (const auto& t : tris) {
            grow(b.box, t);
            b.indices.insert(b.indices.end(), t.begin(), t.end());
        }
        out.push_back(std::move(b));
        return out;
    }
    const std::size_t layers = m.detail_layers.size();
    std::vector<DetailBatch> batches; // oldest first
    for (auto it = tris.rbegin(); it != tris.rend(); ++it) {
        const auto& t = *it;
        int e[3];
        for (int k = 0; k < 3; ++k) e[k] = palette_entry(*map, mesh.vertices[t[k]].uv, layers);
        glm::vec4 tri = empty;
        grow(tri, t);
        for (int k = 0; k < 3; ++k) {
            if (e[k] < 0 || (k > 0 && e[k] == e[0]) || (k == 2 && e[2] == e[1])) continue;
            DetailBatch* into = nullptr;
            for (auto b = batches.rbegin(); b != batches.rend(); ++b) {
                if (b->layer != std::size_t(e[k])) continue;
                glm::vec4 u(std::min(b->box.x, tri.x), std::max(b->box.y, tri.y), std::min(b->box.z, tri.z),
                            std::max(b->box.w, tri.w));
                if (u.y - u.x < 150.0f && u.w - u.z < 150.0f) {
                    b->box = u;
                    into = &*b;
                    break;
                }
            }
            if (!into) {
                into = &batches.emplace_back();
                into->layer = std::size_t(e[k]);
                into->box = tri;
            }
            into->indices.insert(into->indices.end(), t.begin(), t.end());
        }
    }
    out.assign(std::make_move_iterator(batches.rbegin()), std::make_move_iterator(batches.rend()));
    return out;
}

} // namespace hailstone::assets::wld
