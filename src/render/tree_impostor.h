#pragma once

// A far tree for the land round the zone (Hailstone's own; docs/graphics.md "Horizon hills", its trees): a zone's own
// tree drawn once into a picture, from two sides, on the CPU, and two crossed cards that carry it. Thousands of them
// stand on the land's woods (render::horizon_trees), drawn instanced at 4 triangles each, where the tree itself (tens
// of triangles, and cut-out cards that would flicker at a few pixels across) would cost a million.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include <glm/glm.hpp>

#include "assets/model.h"
#include "assets/texture.h"

namespace hailstone::render {

// A material's picture as its UVs read it (RGBA8, level 0, row 0 at v = 0; the cut-out in alpha), or null to leave
// that material out.
using ImpostorTextureLookup = std::function<const assets::Image*(const assets::Material&)>;

// The picture's side, per view, and how many samples each way a texel averages.
inline constexpr std::uint32_t kImpostorSize = 128;
inline constexpr std::uint32_t kImpostorSamples = 2;

struct TreeImpostor {
    // Two cards, crossed at the tree's middle: one across x (the tree seen along y) and one across y (seen along x),
    // each from the tree's lowest point to its highest. One material, cut out and two-sided, whose frame is
    // `texture_name`; normals straight up, so both cards take the light the land under them does.
    assets::Mesh mesh;
    // RGBA8, kImpostorSize x 2 wide and kImpostorSize high: the view along y on the left, along x on the right; the
    // alpha is the share of a texel the tree covers, and where it covers nothing the colour is its mean (so the
    // filtering does not darken its edge).
    assets::Image image;
    glm::vec3 lo{0.0f}, hi{0.0f}; // the tree's box, in its own space
};

// The two crossed cards over the box `lo`..`hi` (TreeImpostor::mesh), carrying the picture named `texture_name`.
assets::Mesh impostor_cards(glm::vec3 lo, glm::vec3 hi, const std::string& texture_name);

// nullopt when nothing of `tree` is drawn (no triangle of a material the lookup gives a picture for).
std::optional<TreeImpostor> tree_impostor(const assets::Mesh& tree, const ImpostorTextureLookup& texture,
                                          const std::string& texture_name);

} // namespace hailstone::render
