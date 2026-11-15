#pragma once

// A texture's pack images from its sources (docs/texstudio.md): the colour upscaled by an image model, its normal and
// roughness maps made by two more from that colour, as tools/texpack/upscale.py made them. The models are behind
// Model, so the rules here (padding, tiles, the transparent texels, the alpha, what is only Lanczos-scaled) are the
// same whatever runs them: ncnn on the GPU (texpack/ncnn_model.h), or a test's stand-in.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "assets/texture.h"
#include "texpack/texpack.h"

namespace hailstone::texpack {

// An image of float channels 0..1, rows top first, each texel's channels together.
struct Planes {
    std::uint32_t width = 0, height = 0, channels = 0;
    std::vector<float> px;

    Planes() = default;
    Planes(std::uint32_t w, std::uint32_t h, std::uint32_t c, float fill = 0.0f)
        : width(w), height(h), channels(c), px(std::size_t(w) * h * c, fill) {}
    float& at(std::uint32_t x, std::uint32_t y, std::uint32_t c) { return px[(std::size_t(y) * width + x) * channels + c]; }
    float at(std::uint32_t x, std::uint32_t y, std::uint32_t c) const {
        return px[(std::size_t(y) * width + x) * channels + c];
    }
};

// An image model: RGB in, RGB `scale()` times as large out (any size in; the output clamped by the caller).
class Model {
public:
    virtual ~Model() = default;
    virtual std::uint32_t scale() const = 0;
    virtual Planes run(const Planes& rgb) = 0;
};

// RGB(A) of an RGBA8 image, and back (alpha 1 where `rgba` has none).
Planes to_planes(const assets::Image& rgba, std::uint32_t channels);
assets::Image to_image(const Planes& rgb, const Planes* alpha = nullptr);

// One channel of `img`, or a copy of the first three.
Planes channel(const Planes& img, std::uint32_t c);
Planes rgb_of(const Planes& img);

enum class Edge : std::uint8_t { Wrap, Reflect };
// `p` texels added round each edge: wrapped (a tiling surface) or reflected without repeating the edge texel
// (numpy's "reflect"; a character skin).
Planes pad(const Planes& img, std::uint32_t p, Edge edge);
Planes crop(const Planes& img, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h);
// PIL's Lanczos (a = 3, the support widened by the scale when shrinking), each channel as a float image, then
// clamped to 0..1.
Planes resize_lanczos(const Planes& img, std::uint32_t w, std::uint32_t h);
// The colour of transparent texels (alpha 0) grown in from their opaque neighbours, so a model never sees the key
// colour: 64 rounds of the 8 neighbours' mean, wrapping at the edges as np.roll does; any texel still unreached takes
// the mean of the known ones.
void bleed(Planes& rgb, const Planes& alpha);

// The model over `img` in overlapping tiles of `tile` source texels (`overlap` on each side), feathered where they
// meet; each tile's output clamped to 0..1.
inline constexpr std::uint32_t kTile = 384;
inline constexpr std::uint32_t kTileOverlap = 16;
Planes run_tiled(Model& model, const Planes& img, std::uint32_t tile = kTile, std::uint32_t overlap = kTileOverlap);

// Texels of the source added round each edge before a model, cropped after, so a tiling texture shows no seam.
inline constexpr std::uint32_t kEdgePad = 16;

struct Models {
    Model* color = nullptr;  // e.g. 4x-PBRify_UpscalerSPANV4: any scale
    Model* normal = nullptr; // 1x-PBRify_NormalV3; null: no normal maps
    Model* rough = nullptr;  // 1x-PBRify_RoughnessV2; null: no roughness maps
};

struct UpscaleSettings {
    std::uint32_t scale = 2; // the pack's size over the game's; the colour model's output is Lanczos-scaled to it
    bool maps = true;
};

// The pack images (RGBA8, level 0, upright) of one texture, those of `wanted` that apply.
struct Outputs {
    std::optional<assets::Image> color, masked, keyed, normal, rough;
    std::optional<assets::Image>& of(FileKind kind);
};
// The most texels upscale_texture makes (the pack's image, and the colour model's output before it is scaled to
// that): 8192 x 8192, far past any game texture at the largest scale (pack_job clamps it to 8). Past it, it throws.
inline constexpr std::uint64_t kMaxUpscaleTexels = std::uint64_t{8192} * 8192;
Outputs upscale_texture(const TextureInfo& info, const Sources& sources, const Models& models,
                        const UpscaleSettings& settings, const std::vector<FileKind>& wanted);

} // namespace hailstone::texpack
