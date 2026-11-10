#pragma once

// Texture decoding for the formats found in EQ archives: DDS (DXT1/3/5 and
// uncompressed, including signed Q8W8V8U8/CxV8U8 normal maps; BC4/BC5/BC7 under
// a DX10 header for texture packs), BMP
// (palettised, RLE and 16/24/32-bit) and TGA (a few sky and model files);
// and PNG for replacement textures (assets/png_read.h).
// Files named .bmp often hold DDS data, so the format is detected from the
// contents, never from the name. See docs/formats/textures.md.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace hailstone::assets {

enum class PixelFormat : std::uint8_t {
    RGBA8, // 4 bytes per pixel, R G B A in memory order
    BC1,   // DXT1, 8 bytes per 4x4 block (1-bit punch-through alpha)
    BC2,   // DXT3
    BC3,   // DXT5
    BC4,   // one channel (red), 8 bytes per block; texture packs' roughness maps
    BC5,   // two channels (red, green), 16 bytes per block; texture packs' normal maps
    BC7,   // RGBA, 16 bytes per block; texture packs' colour
};

struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    PixelFormat format = PixelFormat::RGBA8;
    // Level 0 first; each level is tightly packed (block rows for BCn).
    std::vector<std::vector<std::byte>> mips;
};

struct TextureOptions {
    // Masked materials make palette index 0 of an 8-bit BMP transparent.
    bool palette_index0_transparent = false;
    // WLD meshes (zones, placed objects, sky layers, characters) address BMPs with bottom-origin rows: their rows are
    // flipped to match the UVs. DDS data already matches, even under a .bmp name.
    bool wld_bmp_rows = false;
    // The material reads the texture's alpha (an alpha test or a blend; render::MaterialState::alpha_unused false):
    // a built pack's variant with its colour key taken out (<stem>.k, assets/texture_overrides.h) is drawn when there
    // is one. The game's own file decodes the same either way.
    bool reads_alpha = false;
};

// Returns nullopt and sets *error when the data is not a supported texture.
std::optional<Image> decode_texture(std::span<const std::byte> data, const TextureOptions& options = {},
                                    std::string* error = nullptr);

// Short description of the stored format ("DDS DXT1", "BMP 8-bit RLE8", ...)
// read from the header alone; for tools and diagnostics.
std::string describe_texture(std::span<const std::byte> data);

// Decompresses a BCn image to RGBA8 (all mip levels): BC4 to (r, 0, 0, 255) and BC5 to (r, g, 0, 255), as the GPU
// samples them. RGBA8 images are returned unchanged.
Image to_rgba8(const Image& image);

// Replaces the mips of an RGBA8 colour texture (texels in sRGB) with a full chain built from level 0, for world
// textures: each level is box filtered in linear light with colour weighted by alpha, so the colour of transparent
// texels does not bleed into the edges; the colour of fully transparent level-0 texels is grown in from their opaque
// neighbours, for the same reason under bilinear filtering. A cut-out texture (alpha only 0 and 255, both present)
// keeps level 0's share of texels at or above `coverage_ref` on every level, so alpha-tested foliage does not thin
// out with distance. Other formats are left unchanged.
// `grow_transparent` false keeps level 0's colour under transparent texels as stored: a DDS's alpha channel is not a
// colour key, and an opaque material (EQG Opaque_* shaders, docs/graphics.md) draws that colour (the hero's
// forge plate generic_plate_blue_c.dds keeps most of its picture under alpha 0). A palette index 0 key (an 8-bit
// BMP decoded masked) has no colour worth keeping, and is grown over.
void build_color_mips(Image& image, float coverage_ref = 0.5f, bool grow_transparent = true);

// Takes a colour key out of level 0 of an RGBA8 image; true when there was one. A key is one saturated colour under
// nearly every fully transparent texel (potranquility's dstree312.dds, DXT5 leaves: cyan (0, 170, 156) under all of
// its alpha 0), the background the artist painted on, not a picture an opaque material could draw
// (generic_plate_blue_c.dds keeps a plate under alpha 0); black, grey and a normal map's flat blue are not taken for
// one. The key is also blended into the soft edge, partly transparent texels carrying a share (1 - alpha) of it,
// which drawn or bilinear filtered shows as a cyan fringe. Those texels get the edge's own colour: the opaque
// neighbours' where the texel is mostly clear, its own with the key's share taken out ((c - (1 - a) key) / a) where
// it is mostly opaque, smoothly between; the fully transparent ones get their opaque neighbours'. Other formats are
// left unchanged.
bool remove_color_key(Image& image);

// Makes every texel of an RGBA8 image opaque (all levels), for a material that never reads the texture's alpha (no
// alpha test, no blending): the colour stored under transparent texels is then drawn and filtered as the original
// draws it. Other formats are left unchanged.
void make_opaque(Image& image);

// Whether every texel of an RGBA8 image's level 0 is fully opaque. Such an image needs no coverage-aware mip
// chain: build_color_mips would average it like any other, so the GPU may build the chain itself (render/gpu.cpp
// UploadBatch::texture), which is what the original's Direct3D 9 did (its hardware mip generation).
bool is_opaque(const Image& image);

// A DDS cube map (DDSCAPS2_CUBEMAP, the six faces +X, -X, +Y, -Y, +Z, -Z one after another, each with its mips) laid
// out as one octahedral RGBA8 image `size` texels across: texel uv in 0..1 is the direction octahedral_direction(uv),
// and each texel the faces' bilinear colour there, as Direct3D's texCUBE picks a face. The game's cube maps stand
// with +Y up, so the world's direction (x, y, z up) is looked up as (x, z, y). nullopt when the data is no cube map.
std::optional<Image> decode_cube_as_octahedral(std::span<const std::byte> data, std::uint32_t size = 64);
// The direction (unit) an octahedral map's texel at uv (0..1) stands for: the upper hemisphere (z >= 0) is the
// diamond in the middle, the lower one folded into the corners (shaders/zone.frag octahedral_uv is the inverse).
glm::vec3 octahedral_direction(glm::vec2 uv);
// Where a direction lies in an octahedral map (0..1).
glm::vec2 octahedral_uv(glm::vec3 d);

// Byte size of one mip level of the given dimensions.
std::size_t mip_size(PixelFormat format, std::uint32_t width, std::uint32_t height);

} // namespace hailstone::assets
