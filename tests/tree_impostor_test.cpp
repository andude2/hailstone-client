// render/tree_impostor.h: a zone's tree drawn into a picture for the land's far trees.
#include <doctest/doctest.h>

#include <cstddef>
#include <vector>

#include "render/tree_impostor.h"

using namespace hailstone;

namespace {

// A picture of one colour; the left half's alpha 0 when `holed`.
assets::Image plain(std::uint8_t r, std::uint8_t g, std::uint8_t b, bool holed = false) {
    assets::Image image;
    image.width = image.height = 8;
    std::vector<std::byte> px;
    for (std::uint32_t y = 0; y < 8; ++y)
        for (std::uint32_t x = 0; x < 8; ++x)
            for (std::uint8_t c : {r, g, b, static_cast<std::uint8_t>(holed && x < 4 ? 0 : 255)})
                px.push_back(static_cast<std::byte>(c));
    image.mips.push_back(std::move(px));
    return image;
}

// A card across x (x -10..10, y 0, z 0..40) and a post across y (y -2..2 at x 0) with `blend`, their UVs over the
// whole picture.
assets::Mesh card(assets::BlendMode blend) {
    assets::Mesh mesh;
    assets::Material m;
    m.name = "leaf";
    m.frames = {"leaf.bmp"};
    m.blend = blend;
    mesh.materials.push_back(m);
    auto v = [&](glm::vec3 p, glm::vec2 uv) {
        assets::Vertex out;
        out.position = p;
        out.uv = uv;
        mesh.vertices.push_back(out);
    };
    v({-10, 0, 0}, {0, 1});
    v({10, 0, 0}, {1, 1});
    v({10, 0, 40}, {1, 0});
    v({-10, 0, 40}, {0, 0});
    // A thin post across y, so the box has depth.
    v({0, -2, 0}, {0, 1});
    v({0, 2, 0}, {1, 1});
    v({0, 2, 40}, {1, 0});
    v({0, -2, 40}, {0, 0});
    mesh.indices = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    assets::MeshPart part;
    part.index_count = 12;
    mesh.parts.push_back(part);
    return mesh;
}

std::uint8_t alpha(const assets::Image& image, std::uint32_t x, std::uint32_t y) {
    return static_cast<std::uint8_t>(image.mips[0][(static_cast<std::size_t>(y) * image.width + x) * 4 + 3]);
}

} // namespace

TEST_CASE("tree impostor: the tree's picture from two sides, on two crossed cards") {
    const assets::Image green = plain(20, 200, 40);
    const auto out = render::tree_impostor(card(assets::BlendMode::Opaque),
                                           [&](const assets::Material&) { return &green; }, "hs_land_tree_0.bmp");
    REQUIRE(out);
    CHECK(out->image.width == render::kImpostorSize * 2);
    CHECK(out->image.height == render::kImpostorSize);
    REQUIRE(out->image.mips.size() == 1);
    CHECK(out->lo == glm::vec3(-10, -2, 0));
    CHECK(out->hi == glm::vec3(10, 2, 40));
    // Seen along y the card fills the left half; seen along x (the right half) only the post's thin edge-on width.
    const std::uint32_t s = render::kImpostorSize;
    CHECK(alpha(out->image, s / 4, s / 2) == 255);
    std::size_t covered_right = 0;
    for (std::uint32_t x = s; x < 2 * s; ++x) covered_right += alpha(out->image, x, s / 2) > 0 ? 1 : 0;
    CHECK(covered_right == s); // the post spans y -2..2: the whole width
    // Its colour where covered, and its mean where not.
    const auto& px = out->image.mips[0];
    CHECK(static_cast<int>(px[(static_cast<std::size_t>(s / 2) * out->image.width + s / 4) * 4 + 1]) > 150);
    // Two cards of one cut-out, two-sided material named for the picture.
    CHECK(out->mesh.vertices.size() == 8);
    CHECK(out->mesh.indices.size() == 12);
    REQUIRE(out->mesh.materials.size() == 1);
    CHECK(out->mesh.materials[0].blend == assets::BlendMode::Masked);
    CHECK(out->mesh.materials[0].two_sided);
    CHECK(out->mesh.materials[0].frames == std::vector<std::string>{"hs_land_tree_0.bmp"});
    for (const auto& v : out->mesh.vertices) CHECK(v.normal == glm::vec3(0, 0, 1));
}

TEST_CASE("tree impostor: a cut-out's transparent texels are left out; nothing drawn gives none") {
    const assets::Image holed = plain(20, 200, 40, true);
    const auto out = render::tree_impostor(card(assets::BlendMode::Masked),
                                           [&](const assets::Material&) { return &holed; }, "t.bmp");
    REQUIRE(out);
    const std::uint32_t s = render::kImpostorSize;
    // The picture's left half (u < 0.5) is cut out: the card's left side (x < 0) shows nothing.
    CHECK(alpha(out->image, s / 8, s / 2) == 0);
    CHECK(alpha(out->image, 5 * s / 8, s / 2) == 255);
    // Where nothing is, the colour is the mean (green), not black.
    CHECK(static_cast<int>(out->image.mips[0][(static_cast<std::size_t>(s / 2) * out->image.width + s / 8) * 4 + 1]) > 150);
    CHECK_FALSE(render::tree_impostor(card(assets::BlendMode::Opaque),
                                      [](const assets::Material&) -> const assets::Image* { return nullptr; }, "t.bmp"));
    CHECK_FALSE(render::tree_impostor(assets::Mesh{}, [&](const assets::Material&) { return &holed; }, "t.bmp"));
}
