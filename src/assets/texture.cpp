#include "assets/texture.h"
#include "assets/png_read.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string_view>

#include "core/byte_io.h"

#define BCDEC_IMPLEMENTATION
#include <bcdec.h>

// Formats: Microsoft "DDS file layout" / "DDS_PIXELFORMAT" and "BMP file
// format" (BITMAPINFOHEADER, BITMAPV4/V5HEADER) documentation. BCn decoding
// follows the Direct3D "Opaque and 1-Bit Alpha Textures" (DXT1) and "Texture
// Block Compression in Direct3D 11" (BC1-BC5) pages; BC7 blocks are decoded by
// bcdec (github.com/iOrange/bcdec, MIT). The DXT1 punch-through
// rule matters for EQ: OpenEQ docs/DDS_BC1_ALPHA.md found authored
// transparency in FOURCC-only DXT1 files. See docs/formats/textures.md.

namespace hailstone::assets {

namespace {

// Refuse absurd headers before allocating.
constexpr std::uint32_t kMaxDimension = 16384;
// At most 8192 x 8192 texels (256 MiB as RGBA8), as png_read.cpp's kMaxPixels: a 16384 x 16384 BC1 file is 128 MiB
// but decodes to 1 GiB (to_rgba8). The game's largest texture is 1108 x 1108.
constexpr std::uint64_t kMaxTexels = std::uint64_t{8192} * 8192;

struct DecodeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(const std::string& what) { throw DecodeError(what); }

std::uint32_t fourcc(const char (&s)[5]) {
    return std::uint32_t(std::uint8_t(s[0])) | std::uint32_t(std::uint8_t(s[1])) << 8 |
           std::uint32_t(std::uint8_t(s[2])) << 16 | std::uint32_t(std::uint8_t(s[3])) << 24;
}

std::uint8_t byte_at(std::span<const std::byte> s, std::size_t i) { return std::to_integer<std::uint8_t>(s[i]); }

// Extracts the bits of `mask` from `v` and scales them to 0..255.
struct Channel {
    std::uint32_t mask = 0;
    int shift = 0;
    std::uint32_t max = 0;

    explicit Channel(std::uint32_t m = 0) : mask(m) {
        if (m == 0) return;
        shift = std::countr_zero(m);
        max = m >> shift;
    }
    bool present() const { return mask != 0; }
    std::uint8_t get(std::uint32_t v) const {
        std::uint64_t x = (v & mask) >> shift;
        return static_cast<std::uint8_t>((x * 255u + max / 2) / max);
    }
};

// Converts packed pixels (1-4 bytes each, little-endian) with channel masks.
// An absent alpha mask gives alpha 255; an absent colour mask gives 0, except
// that luminance formats copy the red mask into green and blue.
// Signed formats (D3DFMT_Q8W8V8U8, D3DFMT_CxV8U8 normal maps) have 8-bit
// two's-complement channels; they are stored biased by 128, the usual
// unsigned normal-map encoding. CxV8U8 derives its third component.
struct MaskFormat {
    Channel r, g, b, a;
    bool luminance = false;
    bool signed8 = false;
    bool derive_z = false;
    std::uint32_t bytes = 4;

    void convert(std::span<const std::byte> src, std::byte* dst) const {
        std::uint32_t v = 0;
        for (std::uint32_t i = 0; i < bytes; ++i) v |= std::uint32_t(byte_at(src, i)) << (8 * i);
        auto ch = [&](const Channel& c, std::uint8_t absent) {
            if (!c.present()) return absent;
            std::uint8_t x = c.get(v);
            return signed8 ? static_cast<std::uint8_t>(x ^ 0x80) : x;
        };
        std::uint8_t rr = ch(r, 0);
        std::uint8_t gg = luminance ? rr : ch(g, 0);
        std::uint8_t bb = luminance ? rr : ch(b, 0);
        if (derive_z) {
            double x = (rr - 128) / 127.0, y = (gg - 128) / 127.0;
            double z = std::sqrt(std::max(0.0, 1.0 - x * x - y * y));
            bb = static_cast<std::uint8_t>(std::lround(128 + 127 * z));
        }
        dst[0] = std::byte{rr};
        dst[1] = std::byte{gg};
        dst[2] = std::byte{bb};
        dst[3] = std::byte{ch(a, 255)};
    }
};

std::uint32_t level_dim(std::uint32_t d, std::size_t level) { return std::max<std::uint32_t>(1, d >> level); }

std::size_t full_chain_length(std::uint32_t w, std::uint32_t h) {
    return static_cast<std::size_t>(std::bit_width(std::max(w, h)));
}

// ---- BCn ---------------------------------------------------------------

using Rgba = std::array<std::uint8_t, 4>;

Rgba expand565(std::uint16_t c) {
    std::uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return {static_cast<std::uint8_t>(r << 3 | r >> 2), static_cast<std::uint8_t>(g << 2 | g >> 4),
            static_cast<std::uint8_t>(b << 3 | b >> 2), 255};
}

std::uint8_t mix(std::uint32_t a, std::uint32_t wa, std::uint32_t b, std::uint32_t wb) {
    return static_cast<std::uint8_t>((a * wa + b * wb) / (wa + wb));
}

// Decodes an 8-byte colour block into 16 texels. BC1 uses the three-colour
// plus transparent mode when colour0 <= colour1; BC2/BC3 colour blocks are
// always four-colour.
void decode_color_block(const std::byte* p, bool bc1, std::array<Rgba, 16>& out) {
    std::uint16_t c0 = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[0]) |
                                                  std::to_integer<std::uint8_t>(p[1]) << 8);
    std::uint16_t c1 = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[2]) |
                                                  std::to_integer<std::uint8_t>(p[3]) << 8);
    std::array<Rgba, 4> pal;
    pal[0] = expand565(c0);
    pal[1] = expand565(c1);
    if (!bc1 || c0 > c1) {
        for (int i = 0; i < 3; ++i) {
            pal[2][i] = mix(pal[0][i], 2, pal[1][i], 1);
            pal[3][i] = mix(pal[0][i], 1, pal[1][i], 2);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int i = 0; i < 3; ++i) pal[2][i] = mix(pal[0][i], 1, pal[1][i], 1);
        pal[2][3] = 255;
        pal[3] = {0, 0, 0, 0};
    }
    std::uint32_t idx = 0;
    for (int i = 0; i < 4; ++i) idx |= std::uint32_t(std::to_integer<std::uint8_t>(p[4 + i])) << (8 * i);
    for (int t = 0; t < 16; ++t) out[t] = pal[(idx >> (2 * t)) & 3];
}

void decode_bc2_alpha(const std::byte* p, std::array<Rgba, 16>& out) {
    for (int t = 0; t < 16; ++t) {
        std::uint8_t b = std::to_integer<std::uint8_t>(p[t / 2]);
        std::uint8_t a4 = (t & 1) ? b >> 4 : b & 15;
        out[t][3] = static_cast<std::uint8_t>(a4 * 17);
    }
}

// A BC3 alpha block, also BC4's block and each half of BC5's; written to channel `c`.
void decode_bc3_alpha(const std::byte* p, std::array<Rgba, 16>& out, int c = 3) {
    std::uint32_t a0 = std::to_integer<std::uint8_t>(p[0]), a1 = std::to_integer<std::uint8_t>(p[1]);
    std::array<std::uint8_t, 8> pal{static_cast<std::uint8_t>(a0), static_cast<std::uint8_t>(a1)};
    if (a0 > a1) {
        for (std::uint32_t i = 1; i <= 6; ++i) pal[i + 1] = mix(a0, 7 - i, a1, i);
    } else {
        for (std::uint32_t i = 1; i <= 4; ++i) pal[i + 1] = mix(a0, 5 - i, a1, i);
        pal[6] = 0;
        pal[7] = 255;
    }
    std::uint64_t idx = 0;
    for (int i = 0; i < 6; ++i) idx |= std::uint64_t(std::to_integer<std::uint8_t>(p[2 + i])) << (8 * i);
    for (int t = 0; t < 16; ++t) out[t][c] = pal[(idx >> (3 * t)) & 7];
}

std::vector<std::byte> decode_bc_level(PixelFormat format, std::span<const std::byte> src, std::uint32_t w,
                                       std::uint32_t h) {
    std::size_t block_bytes = format == PixelFormat::BC1 || format == PixelFormat::BC4 ? 8 : 16;
    std::uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    std::vector<std::byte> out(std::size_t(w) * h * 4);
    std::array<Rgba, 16> texels;
    for (std::uint32_t by = 0; by < bh; ++by) {
        for (std::uint32_t bx = 0; bx < bw; ++bx) {
            const std::byte* p = src.data() + (std::size_t(by) * bw + bx) * block_bytes;
            switch (format) {
            case PixelFormat::BC1: decode_color_block(p, true, texels); break;
            case PixelFormat::BC2:
                decode_color_block(p + 8, false, texels);
                decode_bc2_alpha(p, texels);
                break;
            case PixelFormat::BC4:
                texels.fill({0, 0, 0, 255});
                decode_bc3_alpha(p, texels, 0);
                break;
            case PixelFormat::BC5:
                texels.fill({0, 0, 0, 255});
                decode_bc3_alpha(p, texels, 0);
                decode_bc3_alpha(p + 8, texels, 1);
                break;
            case PixelFormat::BC7:
                bcdec_bc7(p, texels.data(), 16);
                break;
            default:
                decode_color_block(p + 8, false, texels);
                decode_bc3_alpha(p, texels);
                break;
            }
            for (std::uint32_t t = 0; t < 16; ++t) {
                std::uint32_t x = bx * 4 + t % 4, y = by * 4 + t / 4;
                if (x >= w || y >= h) continue;
                std::byte* d = out.data() + (std::size_t(y) * w + x) * 4;
                for (int c = 0; c < 4; ++c) d[c] = std::byte{texels[t][c]};
            }
        }
    }
    return out;
}

// ---- DDS ---------------------------------------------------------------

constexpr std::uint32_t DDPF_ALPHAPIXELS = 0x1;
constexpr std::uint32_t DDPF_ALPHA = 0x2;
constexpr std::uint32_t DDPF_FOURCC = 0x4;
constexpr std::uint32_t DDPF_RGB = 0x40;
constexpr std::uint32_t DDPF_LUMINANCE = 0x20000;
constexpr std::uint32_t DDSCAPS2_CUBEMAP = 0x200;
constexpr std::uint32_t DDSCAPS2_VOLUME = 0x200000;

struct DdsHeader {
    std::uint32_t height = 0, width = 0, depth = 0, mip_count = 0;
    std::uint32_t pf_flags = 0, pf_fourcc = 0, pf_bits = 0;
    std::uint32_t r_mask = 0, g_mask = 0, b_mask = 0, a_mask = 0;
    std::uint32_t caps2 = 0;
    std::uint32_t dxgi_format = 0; // with FourCC DX10
};

DdsHeader read_dds_header(ByteReader& r) {
    r.seek(4);
    if (r.le<std::uint32_t>() != 124) fail("DDS header size is not 124");
    DdsHeader h;
    r.skip(4); // flags
    h.height = r.le<std::uint32_t>();
    h.width = r.le<std::uint32_t>();
    r.skip(4); // pitch or linear size
    h.depth = r.le<std::uint32_t>();
    h.mip_count = r.le<std::uint32_t>();
    r.skip(11 * 4);
    if (r.le<std::uint32_t>() != 32) fail("DDS pixel format size is not 32");
    h.pf_flags = r.le<std::uint32_t>();
    h.pf_fourcc = r.le<std::uint32_t>();
    h.pf_bits = r.le<std::uint32_t>();
    h.r_mask = r.le<std::uint32_t>();
    h.g_mask = r.le<std::uint32_t>();
    h.b_mask = r.le<std::uint32_t>();
    h.a_mask = r.le<std::uint32_t>();
    r.skip(4); // caps
    h.caps2 = r.le<std::uint32_t>();
    r.skip(12);
    if ((h.pf_flags & DDPF_FOURCC) && h.pf_fourcc == fourcc("DX10")) {
        h.dxgi_format = r.le<std::uint32_t>();
        r.skip(16); // dimension, misc flags, array size, misc flags 2
    }
    return h;
}

// DXGI_FORMAT values read from a DX10 header.
constexpr std::uint32_t DXGI_R8G8B8A8_UNORM = 28, DXGI_R8G8B8A8_UNORM_SRGB = 29;
constexpr std::uint32_t DXGI_BC1_UNORM = 71, DXGI_BC1_UNORM_SRGB = 72, DXGI_BC2_UNORM = 74, DXGI_BC2_UNORM_SRGB = 75;
constexpr std::uint32_t DXGI_BC3_UNORM = 77, DXGI_BC3_UNORM_SRGB = 78, DXGI_BC4_UNORM = 80, DXGI_BC5_UNORM = 83;
constexpr std::uint32_t DXGI_BC7_UNORM = 98, DXGI_BC7_UNORM_SRGB = 99;

// Legacy D3DFORMAT values that writers store in the FourCC field.
constexpr std::uint32_t D3DFMT_Q8W8V8U8 = 63;
constexpr std::uint32_t D3DFMT_CxV8U8 = 117;

std::string fourcc_name(std::uint32_t v) {
    if (v == D3DFMT_Q8W8V8U8) return "Q8W8V8U8";
    if (v == D3DFMT_CxV8U8) return "CxV8U8";
    if (v < 0x100) return "D3DFMT " + std::to_string(v);
    std::string s;
    for (int i = 0; i < 4; ++i) {
        char c = static_cast<char>((v >> (8 * i)) & 0xff);
        s += (c >= 0x20 && c < 0x7f) ? c : '?';
    }
    return s;
}

std::string describe_dds(const DdsHeader& h) {
    std::string s = "DDS ";
    if (h.pf_flags & DDPF_FOURCC) {
        s += fourcc_name(h.pf_fourcc);
        if (h.pf_fourcc == fourcc("DX10")) s += " DXGI " + std::to_string(h.dxgi_format);
    } else {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s%u %08x/%08x/%08x/%08x",
                      (h.pf_flags & DDPF_LUMINANCE) ? "L" : (h.pf_flags & DDPF_RGB) ? "RGB" : "A", h.pf_bits,
                      h.r_mask, h.g_mask, h.b_mask, (h.pf_flags & (DDPF_ALPHAPIXELS | DDPF_ALPHA)) ? h.a_mask : 0u);
        s += buf;
    }
    if (h.mip_count > 1) s += " mipcount=" + std::to_string(h.mip_count);
    if (h.caps2 & DDSCAPS2_CUBEMAP) s += " cube";
    if (h.caps2 & DDSCAPS2_VOLUME) s += " volume";
    return s;
}

Image decode_dds(std::span<const std::byte> data) {
    ByteReader r(data);
    DdsHeader h = read_dds_header(r);
    if (h.width == 0 || h.height == 0 || h.width > kMaxDimension || h.height > kMaxDimension ||
        std::uint64_t{h.width} * h.height > kMaxTexels)
        fail("DDS dimensions out of range");

    Image img;
    img.width = h.width;
    img.height = h.height;
    std::optional<MaskFormat> mask;
    if (h.pf_flags & DDPF_FOURCC) {
        if (h.pf_fourcc == fourcc("DXT1")) img.format = PixelFormat::BC1;
        else if (h.pf_fourcc == fourcc("DXT2") || h.pf_fourcc == fourcc("DXT3")) img.format = PixelFormat::BC2;
        else if (h.pf_fourcc == fourcc("DXT4") || h.pf_fourcc == fourcc("DXT5")) img.format = PixelFormat::BC3;
        else if (h.pf_fourcc == fourcc("ATI1") || h.pf_fourcc == fourcc("BC4U")) img.format = PixelFormat::BC4;
        else if (h.pf_fourcc == fourcc("ATI2") || h.pf_fourcc == fourcc("BC5U")) img.format = PixelFormat::BC5;
        else if (h.pf_fourcc == fourcc("DX10")) {
            switch (h.dxgi_format) {
            case DXGI_R8G8B8A8_UNORM:
            case DXGI_R8G8B8A8_UNORM_SRGB:
                mask = MaskFormat{Channel(0xff), Channel(0xff00), Channel(0xff0000), Channel(0xff000000)};
                break;
            case DXGI_BC1_UNORM:
            case DXGI_BC1_UNORM_SRGB: img.format = PixelFormat::BC1; break;
            case DXGI_BC2_UNORM:
            case DXGI_BC2_UNORM_SRGB: img.format = PixelFormat::BC2; break;
            case DXGI_BC3_UNORM:
            case DXGI_BC3_UNORM_SRGB: img.format = PixelFormat::BC3; break;
            case DXGI_BC4_UNORM: img.format = PixelFormat::BC4; break;
            case DXGI_BC5_UNORM: img.format = PixelFormat::BC5; break;
            case DXGI_BC7_UNORM:
            case DXGI_BC7_UNORM_SRGB: img.format = PixelFormat::BC7; break;
            default: fail("unsupported DDS DXGI format " + std::to_string(h.dxgi_format));
            }
        }
        else if (h.pf_fourcc == D3DFMT_Q8W8V8U8) {
            MaskFormat m{Channel(0xff), Channel(0xff00), Channel(0xff0000), Channel(0xff000000)};
            m.signed8 = true;
            mask = m;
        } else if (h.pf_fourcc == D3DFMT_CxV8U8) {
            MaskFormat m{Channel(0xff), Channel(0xff00), Channel(0), Channel(0)};
            m.signed8 = m.derive_z = true;
            m.bytes = 2;
            mask = m;
        } else fail("unsupported DDS FourCC " + fourcc_name(h.pf_fourcc));
    } else if (h.pf_flags & (DDPF_RGB | DDPF_LUMINANCE | DDPF_ALPHA)) {
        if (h.pf_bits != 8 && h.pf_bits != 16 && h.pf_bits != 24 && h.pf_bits != 32)
            fail("unsupported DDS bit count " + std::to_string(h.pf_bits));
        bool has_alpha = (h.pf_flags & (DDPF_ALPHAPIXELS | DDPF_ALPHA)) != 0;
        MaskFormat m{Channel(h.r_mask), Channel(h.g_mask), Channel(h.b_mask), Channel(has_alpha ? h.a_mask : 0)};
        m.luminance = (h.pf_flags & DDPF_LUMINANCE) != 0;
        m.bytes = h.pf_bits / 8;
        if (!m.r.present() && !m.g.present() && !m.b.present() && !m.a.present()) fail("DDS has no channel masks");
        mask = m;
    } else {
        fail("unsupported DDS pixel format flags");
    }

    // Uncompressed levels are tightly packed at bit_count per pixel.
    auto source_size = [&](std::uint32_t w, std::uint32_t ht) {
        return mask ? std::size_t(w) * ht * mask->bytes : mip_size(img.format, w, ht);
    };
    std::size_t levels = std::clamp<std::size_t>(h.mip_count, 1, full_chain_length(h.width, h.height));
    // Cube maps and volumes: only the first face / slice is kept.
    std::size_t pos = r.position();
    for (std::size_t level = 0; level < levels; ++level) {
        std::uint32_t w = level_dim(h.width, level), ht = level_dim(h.height, level);
        std::size_t n = source_size(w, ht);
        if (n > data.size() - pos) {
            if (level == 0) fail("DDS pixel data truncated");
            break;
        }
        auto src = data.subspan(pos, n);
        pos += n;
        if (!mask) {
            img.mips.emplace_back(src.begin(), src.end());
            continue;
        }
        std::vector<std::byte> out(std::size_t(w) * ht * 4);
        for (std::size_t i = 0; i < std::size_t(w) * ht; ++i)
            mask->convert(src.subspan(i * mask->bytes, mask->bytes), out.data() + i * 4);
        img.mips.push_back(std::move(out));
        if (h.caps2 & DDSCAPS2_VOLUME) break; // later levels would mix slices
    }
    return img;
}

} // namespace

glm::vec3 octahedral_direction(glm::vec2 uv) {
    glm::vec2 f = uv * 2.0f - 1.0f;
    glm::vec3 d(f.x, f.y, 1.0f - std::abs(f.x) - std::abs(f.y));
    if (d.z < 0.0f) {
        const float x = d.x, y = d.y;
        d.x = (1.0f - std::abs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
        d.y = (1.0f - std::abs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
    }
    return glm::normalize(d);
}

glm::vec2 octahedral_uv(glm::vec3 d) {
    d /= std::abs(d.x) + std::abs(d.y) + std::abs(d.z);
    glm::vec2 p(d.x, d.y);
    if (d.z < 0.0f)
        p = glm::vec2((1.0f - std::abs(d.y)) * (d.x >= 0.0f ? 1.0f : -1.0f),
                      (1.0f - std::abs(d.x)) * (d.y >= 0.0f ? 1.0f : -1.0f));
    return p * 0.5f + 0.5f;
}

std::optional<Image> decode_cube_as_octahedral(std::span<const std::byte> data, std::uint32_t size) {
    if (data.size() < 128 || size == 0 || !(data[0] == std::byte{'D'} && data[1] == std::byte{'D'} &&
                                            data[2] == std::byte{'S'} && data[3] == std::byte{' '}))
        return std::nullopt;
    DdsHeader h;
    try {
        ByteReader r(data);
        h = read_dds_header(r);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!(h.caps2 & DDSCAPS2_CUBEMAP)) return std::nullopt;
    const std::size_t header = 128 + ((h.pf_flags & DDPF_FOURCC) && h.pf_fourcc == fourcc("DX10") ? 20 : 0);
    if (data.size() <= header || (data.size() - header) % 6 != 0) return std::nullopt;
    const std::size_t face_bytes = (data.size() - header) / 6;
    // Each face decoded on its own: the header (cube bit cleared) over that face's levels.
    std::array<Image, 6> faces;
    for (std::size_t f = 0; f < 6; ++f) {
        std::vector<std::byte> one(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(header));
        one.insert(one.end(), data.begin() + static_cast<std::ptrdiff_t>(header + f * face_bytes),
                   data.begin() + static_cast<std::ptrdiff_t>(header + (f + 1) * face_bytes));
        std::uint32_t caps2 = h.caps2 & ~DDSCAPS2_CUBEMAP;
        for (int k = 0; k < 4; ++k) one[112 + static_cast<std::size_t>(k)] = static_cast<std::byte>(caps2 >> (8 * k)); // dwCaps2
        try {
            faces[f] = to_rgba8(decode_dds(one));
        } catch (const std::exception&) {
            return std::nullopt;
        }
        if (faces[f].width != faces[0].width || faces[f].height != faces[0].height || faces[f].mips.empty())
            return std::nullopt;
    }
    const std::uint32_t fw = faces[0].width, fh = faces[0].height;
    auto texel = [&](const Image& img, float u, float v) {
        // Bilinear within the face, clamped at its edges.
        float x = std::clamp(u * static_cast<float>(fw) - 0.5f, 0.0f, static_cast<float>(fw - 1));
        float y = std::clamp(v * static_cast<float>(fh) - 0.5f, 0.0f, static_cast<float>(fh - 1));
        auto x0 = static_cast<std::uint32_t>(x), y0 = static_cast<std::uint32_t>(y);
        std::uint32_t x1 = std::min(x0 + 1, fw - 1), y1 = std::min(y0 + 1, fh - 1);
        float ax = x - static_cast<float>(x0), ay = y - static_cast<float>(y0);
        auto at = [&](std::uint32_t tx, std::uint32_t ty) {
            const std::byte* p = &img.mips[0][(std::size_t(ty) * fw + tx) * 4];
            return glm::vec4(std::to_integer<int>(p[0]), std::to_integer<int>(p[1]), std::to_integer<int>(p[2]),
                             std::to_integer<int>(p[3]));
        };
        return glm::mix(glm::mix(at(x0, y0), at(x1, y0), ax), glm::mix(at(x0, y1), at(x1, y1), ax), ay);
    };
    Image out;
    out.width = out.height = size;
    out.mips.emplace_back(std::size_t(size) * size * 4);
    for (std::uint32_t y = 0; y < size; ++y)
        for (std::uint32_t x = 0; x < size; ++x) {
            glm::vec3 d = octahedral_direction(
                glm::vec2((static_cast<float>(x) + 0.5f) / static_cast<float>(size),
                          (static_cast<float>(y) + 0.5f) / static_cast<float>(size)));
            // The cube maps are made with +Y up (env_noswap_day's +Y face is the sky, -Y the water): the world's
            // (x, y, z up) looks up (x, z, y). Then Direct3D's face selection and face coordinates (OpenGL's table).
            d = glm::vec3(d.x, d.z, d.y);
            glm::vec3 a = glm::abs(d);
            std::size_t face;
            float u, v, m;
            if (a.x >= a.y && a.x >= a.z) {
                m = a.x;
                face = d.x > 0.0f ? 0 : 1;
                u = d.x > 0.0f ? -d.z : d.z;
                v = -d.y;
            } else if (a.y >= a.z) {
                m = a.y;
                face = d.y > 0.0f ? 2 : 3;
                u = d.x;
                v = d.y > 0.0f ? d.z : -d.z;
            } else {
                m = a.z;
                face = d.z > 0.0f ? 4 : 5;
                u = d.z > 0.0f ? d.x : -d.x;
                v = -d.y;
            }
            glm::vec4 c = texel(faces[face], (u / m + 1.0f) * 0.5f, (v / m + 1.0f) * 0.5f);
            std::byte* p = &out.mips[0][(std::size_t(y) * size + x) * 4];
            for (int k = 0; k < 4; ++k)
                p[k] = static_cast<std::byte>(std::clamp(static_cast<int>(std::lround(c[k])), 0, 255));
        }
    return out;
}

namespace {

// ---- BMP ---------------------------------------------------------------

constexpr std::uint32_t BI_RGB = 0, BI_RLE8 = 1, BI_RLE4 = 2, BI_BITFIELDS = 3, BI_ALPHABITFIELDS = 6;

struct BmpHeader {
    std::uint32_t data_offset = 0, header_size = 0, compression = 0, colors_used = 0;
    std::int32_t width = 0, height = 0;
    std::uint16_t bits = 0;
    std::uint32_t masks[4] = {0, 0, 0, 0};
    bool has_alpha_mask = false;
};

BmpHeader read_bmp_header(ByteReader& r) {
    BmpHeader h;
    r.seek(10);
    h.data_offset = r.le<std::uint32_t>();
    h.header_size = r.le<std::uint32_t>();
    if (h.header_size == 12) { // BITMAPCOREHEADER
        h.width = r.le<std::int16_t>();
        h.height = r.le<std::int16_t>();
        r.skip(2);
        h.bits = r.le<std::uint16_t>();
        return h;
    }
    if (h.header_size < 40) fail("unsupported BMP header size " + std::to_string(h.header_size));
    h.width = r.le<std::int32_t>();
    h.height = r.le<std::int32_t>();
    r.skip(2); // planes
    h.bits = r.le<std::uint16_t>();
    h.compression = r.le<std::uint32_t>();
    r.skip(12); // image size, resolution
    h.colors_used = r.le<std::uint32_t>();
    r.skip(4);
    if (h.compression == BI_BITFIELDS || h.compression == BI_ALPHABITFIELDS) {
        // Masks live inside V2+ headers, or directly after a 40-byte header.
        int count = (h.compression == BI_ALPHABITFIELDS || h.header_size >= 56) ? 4 : 3;
        for (int i = 0; i < count; ++i) h.masks[i] = r.le<std::uint32_t>();
        h.has_alpha_mask = count == 4;
    }
    return h;
}

std::string describe_bmp(const BmpHeader& h) {
    std::string s = "BMP " + std::to_string(h.bits) + "-bit";
    if (h.compression == BI_RLE8) s += " RLE8";
    else if (h.compression == BI_RLE4) s += " RLE4";
    else if (h.compression == BI_BITFIELDS || h.compression == BI_ALPHABITFIELDS) s += " bitfields";
    else if (h.compression != BI_RGB) s += " compression " + std::to_string(h.compression);
    if (h.header_size != 40) s += " hdr" + std::to_string(h.header_size);
    if (h.height < 0) s += " top-down";
    return s;
}

// Decodes RLE8/RLE4, calling store(row, x, palette index) for each pixel written, rows bottom-up as stored. The
// caller fills the pixels never written with index 0.
template <typename Store>
void decode_rle(ByteReader& r, std::uint32_t w, std::uint32_t h, bool rle4, Store&& store) {
    std::uint32_t x = 0, y = 0;
    auto put = [&](std::uint8_t v) {
        if (x < w && y < h) store(y, x, v);
        ++x;
    };
    while (y < h) {
        std::uint8_t count = r.le<std::uint8_t>();
        std::uint8_t value = r.le<std::uint8_t>();
        if (count > 0) {
            for (std::uint32_t i = 0; i < count; ++i)
                put(rle4 ? static_cast<std::uint8_t>((i & 1) ? value & 15 : value >> 4) : value);
        } else if (value == 0) { // end of line
            x = 0;
            ++y;
        } else if (value == 1) { // end of bitmap
            break;
        } else if (value == 2) { // delta
            x += r.le<std::uint8_t>();
            y += r.le<std::uint8_t>();
        } else { // absolute run of `value` pixels, padded to 16 bits
            std::size_t nbytes = rle4 ? (value + 1u) / 2 : value;
            auto s = r.bytes(nbytes);
            for (std::uint32_t i = 0; i < value; ++i) {
                std::uint8_t b = byte_at(s, rle4 ? i / 2 : i);
                put(rle4 ? static_cast<std::uint8_t>((i & 1) ? b & 15 : b >> 4) : b);
            }
            if (nbytes & 1) r.skip(1);
        }
    }
}

Image decode_bmp(std::span<const std::byte> data, const TextureOptions& options) {
    ByteReader r(data);
    BmpHeader h = read_bmp_header(r);
    if (h.width <= 0 || h.height == 0 || h.height == INT32_MIN) fail("BMP dimensions out of range");
    bool top_down = h.height < 0;
    std::uint32_t w = static_cast<std::uint32_t>(h.width);
    std::uint32_t ht = static_cast<std::uint32_t>(top_down ? -std::int64_t(h.height) : h.height);
    if (w > kMaxDimension || ht > kMaxDimension || std::uint64_t{w} * ht > kMaxTexels) fail("BMP dimensions out of range");

    bool palettised = h.bits == 1 || h.bits == 4 || h.bits == 8;
    bool rle = h.compression == BI_RLE8 || h.compression == BI_RLE4;
    if (rle && (top_down || h.bits != (h.compression == BI_RLE8 ? 8 : 4))) fail("invalid BMP RLE header");
    if (!palettised && h.bits != 16 && h.bits != 24 && h.bits != 32)
        fail("unsupported BMP bit count " + std::to_string(h.bits));
    if (!rle && h.compression != BI_RGB &&
        !((h.compression == BI_BITFIELDS || h.compression == BI_ALPHABITFIELDS) && (h.bits == 16 || h.bits == 32)))
        fail("unsupported BMP compression " + std::to_string(h.compression));

    // Palette: BGRx entries (BGR for the core header). Its reserved byte is
    // not alpha.
    std::vector<Rgba> palette;
    if (palettised) {
        std::size_t entries = h.colors_used ? h.colors_used : (std::size_t(1) << h.bits);
        if (entries > 256) fail("BMP palette too large");
        entries = std::min<std::size_t>(entries, std::size_t(1) << h.bits); // colors_used may exceed what the depth can index
        std::size_t entry_bytes = h.header_size == 12 ? 3 : 4;
        r.seek(14 + std::size_t(h.header_size));
        // Some writers declare more entries than fit before the pixels.
        if (h.data_offset >= r.position())
            entries = std::min(entries, (h.data_offset - r.position()) / entry_bytes);
        palette.resize(std::size_t(1) << h.bits, Rgba{0, 0, 0, 255});
        for (std::size_t i = 0; i < entries; ++i) {
            auto e = r.bytes(entry_bytes);
            palette[i] = {byte_at(e, 2), byte_at(e, 1), byte_at(e, 0), 255};
        }
        if (options.palette_index0_transparent) palette[0][3] = 0;
    }

    MaskFormat mask;
    if (!palettised) {
        if (h.compression == BI_RGB) {
            if (h.bits == 16) mask = {Channel(0x7c00), Channel(0x03e0), Channel(0x001f), Channel(0)};
            else mask = {Channel(0xff0000), Channel(0x00ff00), Channel(0x0000ff), Channel(0)};
        } else {
            mask = {Channel(h.masks[0]), Channel(h.masks[1]), Channel(h.masks[2]),
                    Channel(h.has_alpha_mask ? h.masks[3] : 0)};
        }
        mask.bytes = h.bits / 8u;
    }

    // Check the data can cover the image before allocating it.
    r.seek(h.data_offset);
    std::size_t stride = ((std::size_t(w) * h.bits + 31) / 32) * 4;
    std::size_t last_row = (std::size_t(w) * h.bits + 7) / 8; // the last row needs no padding
    if (rle) {
        // Each escape is at least two bytes; a delta skips at most 255 rows.
        if (ht > (r.remaining() / 2 + 1) * 255) fail("BMP pixel data truncated");
        // A run writes at most 255 pixels for its two bytes. Only deltas and an early end of bitmap leave more
        // unwritten, and no encoder writes a whole image so: a size far past what its runs could fill is a forged
        // header (no RLE bitmap is in the game's files, docs/formats/textures.md).
        if (std::uint64_t{w} * ht > (std::uint64_t{r.remaining()} + 512) * 128) fail("BMP RLE data too short for its size");
    } else if (r.remaining() < stride * (ht - 1) + last_row) {
        fail("BMP pixel data truncated");
    }

    Image img;
    img.width = w;
    img.height = ht;
    std::vector<std::byte> out(std::size_t(w) * ht * 4);

    auto store_index = [&](std::size_t y, std::uint32_t x, std::uint8_t i) {
        std::byte* d = out.data() + (y * w + x) * 4;
        for (int c = 0; c < 4; ++c) d[c] = std::byte{palette[i][c]};
    };

    if (rle) {
        for (std::uint32_t y = 0; y < ht; ++y)
            for (std::uint32_t x = 0; x < w; ++x) store_index(y, x, 0);
        decode_rle(r, w, ht, h.compression == BI_RLE4,
                   [&](std::uint32_t y, std::uint32_t x, std::uint8_t i) { store_index(ht - 1 - y, x, i); });
    } else {
        for (std::uint32_t row = 0; row < ht; ++row) {
            auto src = data.subspan(h.data_offset + row * stride, row + 1 == ht ? last_row : stride);
            std::size_t y = top_down ? row : ht - 1 - row;
            for (std::uint32_t x = 0; x < w; ++x) {
                if (palettised) {
                    std::uint32_t bit = x * h.bits;
                    std::uint8_t b = byte_at(src, bit / 8);
                    std::uint8_t i = static_cast<std::uint8_t>((b >> (8 - h.bits - bit % 8)) & ((1u << h.bits) - 1));
                    store_index(y, x, i);
                } else {
                    mask.convert(src.subspan(std::size_t(x) * mask.bytes, mask.bytes), out.data() + (y * w + x) * 4);
                }
            }
        }
    }
    img.mips.push_back(std::move(out));
    return img;
}

// ---- TGA ---------------------------------------------------------------
// Truevision TGA 2.0 specification. TGA has no magic, so a header is only
// accepted when every field is plausible; it is tried after DDS and BMP.

struct TgaHeader {
    std::uint8_t id_length = 0, cmap_type = 0, image_type = 0, cmap_bits = 0, bits = 0, descriptor = 0;
    std::uint16_t cmap_first = 0, cmap_length = 0, width = 0, height = 0;
};

std::optional<TgaHeader> read_tga_header(std::span<const std::byte> data) {
    if (data.size() < 18) return std::nullopt;
    ByteReader r(data);
    TgaHeader h;
    h.id_length = r.le<std::uint8_t>();
    h.cmap_type = r.le<std::uint8_t>();
    h.image_type = r.le<std::uint8_t>();
    h.cmap_first = r.le<std::uint16_t>();
    h.cmap_length = r.le<std::uint16_t>();
    h.cmap_bits = r.le<std::uint8_t>();
    r.skip(4); // origin
    h.width = r.le<std::uint16_t>();
    h.height = r.le<std::uint16_t>();
    h.bits = r.le<std::uint8_t>();
    h.descriptor = r.le<std::uint8_t>();
    bool mapped = h.image_type == 1 || h.image_type == 9;
    bool truecolor = h.image_type == 2 || h.image_type == 10;
    bool gray = h.image_type == 3 || h.image_type == 11;
    if (!mapped && !truecolor && !gray) return std::nullopt;
    if (h.cmap_type > 1 || (mapped && h.cmap_type != 1) || h.width == 0 || h.height == 0) return std::nullopt;
    if (h.descriptor & 0xc0) return std::nullopt; // interleaving: obsolete
    if (mapped && h.bits != 8 && h.bits != 16) return std::nullopt;
    if (truecolor && h.bits != 15 && h.bits != 16 && h.bits != 24 && h.bits != 32) return std::nullopt;
    if (gray && h.bits != 8 && h.bits != 16) return std::nullopt;
    if (h.cmap_type == 1 && h.cmap_bits != 15 && h.cmap_bits != 16 && h.cmap_bits != 24 && h.cmap_bits != 32)
        return std::nullopt;
    return h;
}

std::string describe_tga(const TgaHeader& h) {
    std::string s = "TGA type " + std::to_string(h.image_type) + " " + std::to_string(h.bits) + "-bit";
    if (h.descriptor & 0x20) s += " top-down";
    return s;
}

// Converts one stored truecolour/colour-map value (BGR[A], or 16-bit
// A1R5G5B5) to RGBA.
Rgba tga_color(std::uint32_t v, std::uint32_t bits, bool use_alpha) {
    if (bits == 15 || bits == 16) {
        auto x5 = [](std::uint32_t c) { return static_cast<std::uint8_t>((c << 3) | (c >> 2)); };
        return {x5((v >> 10) & 31), x5((v >> 5) & 31), x5(v & 31),
                static_cast<std::uint8_t>(bits == 16 && use_alpha ? ((v >> 15) ? 255 : 0) : 255)};
    }
    return {static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v),
            static_cast<std::uint8_t>(bits == 32 && use_alpha ? v >> 24 : 255)};
}

Image decode_tga(std::span<const std::byte> data, const TgaHeader& h) {
    if (h.width > kMaxDimension || h.height > kMaxDimension || std::uint64_t{h.width} * h.height > kMaxTexels)
        fail("TGA dimensions out of range");
    ByteReader r(data);
    r.seek(18 + std::size_t(h.id_length));
    bool use_alpha = (h.descriptor & 15) != 0;
    std::vector<Rgba> palette;
    if (h.cmap_type == 1) {
        std::uint32_t entry_bytes = (h.cmap_bits + 7u) / 8;
        for (std::uint32_t i = 0; i < h.cmap_length; ++i) {
            std::uint32_t v = 0;
            for (std::uint32_t b = 0; b < entry_bytes; ++b) v |= std::uint32_t(r.le<std::uint8_t>()) << (8 * b);
            palette.push_back(tga_color(v, h.cmap_bits, use_alpha));
        }
    }
    std::uint32_t pixel_bytes = (h.bits + 7u) / 8;
    auto read_value = [&] {
        std::uint32_t v = 0;
        for (std::uint32_t b = 0; b < pixel_bytes; ++b) v |= std::uint32_t(r.le<std::uint8_t>()) << (8 * b);
        return v;
    };
    auto to_rgba = [&](std::uint32_t v) -> Rgba {
        switch (h.image_type) {
        case 1:
        case 9: {
            std::size_t i = v - h.cmap_first;
            if (v < h.cmap_first || i >= palette.size()) fail("TGA colour index out of range");
            return palette[i];
        }
        case 3:
        case 11: {
            auto l = static_cast<std::uint8_t>(h.bits == 16 ? v & 0xff : v);
            return {l, l, l, static_cast<std::uint8_t>(h.bits == 16 && use_alpha ? v >> 8 : 255)};
        }
        default: return tga_color(v, h.bits, use_alpha);
        }
    };

    std::size_t count = std::size_t(h.width) * h.height;
    // An RLE packet of 1 + pixel_bytes bytes yields at most 128 pixels.
    if (count > (r.remaining() / (1 + pixel_bytes) + 1) * 128) fail("TGA pixel data truncated");
    std::vector<Rgba> stream;
    stream.reserve(count);
    if (h.image_type >= 9) {
        while (stream.size() < count) {
            std::uint8_t packet = r.le<std::uint8_t>();
            std::size_t n = std::min<std::size_t>((packet & 0x7fu) + 1u, count - stream.size());
            if (packet & 0x80) {
                Rgba c = to_rgba(read_value());
                stream.insert(stream.end(), n, c);
            } else {
                for (std::size_t i = 0; i < n; ++i) stream.push_back(to_rgba(read_value()));
            }
        }
    } else {
        if (r.remaining() < count * pixel_bytes) fail("TGA pixel data truncated");
        for (std::size_t i = 0; i < count; ++i) stream.push_back(to_rgba(read_value()));
    }

    Image img;
    img.width = h.width;
    img.height = h.height;
    std::vector<std::byte> out(count * 4);
    bool top_down = (h.descriptor & 0x20) != 0, right_to_left = (h.descriptor & 0x10) != 0;
    for (std::uint32_t row = 0; row < h.height; ++row) {
        std::size_t y = top_down ? row : h.height - 1u - row;
        for (std::uint32_t col = 0; col < h.width; ++col) {
            std::size_t x = right_to_left ? h.width - 1u - col : col;
            const Rgba& c = stream[std::size_t(row) * h.width + col];
            for (int k = 0; k < 4; ++k) out[(y * h.width + x) * 4 + k] = std::byte{c[k]};
        }
    }
    img.mips.push_back(std::move(out));
    return img;
}

bool starts_with(std::span<const std::byte> data, std::string_view magic) {
    if (data.size() < magic.size()) return false;
    for (std::size_t i = 0; i < magic.size(); ++i)
        if (byte_at(data, i) != static_cast<std::uint8_t>(magic[i])) return false;
    return true;
}

} // namespace

std::optional<Image> decode_texture(std::span<const std::byte> data, const TextureOptions& options,
                                    std::string* error) {
    try {
        if (starts_with(data, "DDS ")) return decode_dds(data);
        if (is_png(data)) return decode_png(data, error); // replacement textures only (assets/texture_overrides.h)
        if (starts_with(data, "BM")) {
            Image img = decode_bmp(data, options);
            if (options.wld_bmp_rows && img.format == PixelFormat::RGBA8) {
                std::size_t row = std::size_t(img.width) * 4;
                for (auto& level : img.mips)
                    if (level.size() >= row * img.height)
                        for (std::uint32_t y = 0; y < img.height / 2; ++y)
                            std::swap_ranges(level.begin() + static_cast<std::ptrdiff_t>(y * row),
                                             level.begin() + static_cast<std::ptrdiff_t>((y + 1) * row),
                                             level.begin() + static_cast<std::ptrdiff_t>((img.height - 1 - y) * row));
            }
            return img;
        }
        if (auto tga = read_tga_header(data)) return decode_tga(data, *tga);
        fail("unrecognised texture format");
    } catch (const std::exception& e) {
        // DecodeError, or ByteReader's out_of_range on truncated data.
        if (error) *error = e.what();
    }
    return std::nullopt;
}

std::string describe_texture(std::span<const std::byte> data) {
    try {
        if (starts_with(data, "DDS ")) {
            ByteReader r(data);
            return describe_dds(read_dds_header(r));
        }
        if (starts_with(data, "BM")) {
            ByteReader r(data);
            return describe_bmp(read_bmp_header(r));
        }
        if (auto tga = read_tga_header(data)) return describe_tga(*tga);
        if (is_png(data)) return "PNG";
    } catch (const std::exception&) {
        return "truncated header";
    }
    return "unknown";
}

Image to_rgba8(const Image& image) {
    if (image.format == PixelFormat::RGBA8) return image;
    Image out;
    out.width = image.width;
    out.height = image.height;
    out.format = PixelFormat::RGBA8;
    for (std::size_t level = 0; level < image.mips.size(); ++level) {
        std::uint32_t w = level_dim(image.width, level), h = level_dim(image.height, level);
        const auto& src = image.mips[level];
        if (src.size() < mip_size(image.format, w, h)) break; // malformed Image; keep what is valid
        out.mips.push_back(decode_bc_level(image.format, src, w, h));
    }
    return out;
}

namespace {

float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

// A table over linear values (fine enough that neighbouring entries round to the same or adjacent bytes).
constexpr int kEncodeSteps = 8192;

std::byte linear_to_srgb_byte(float c) {
    static const std::array<std::uint8_t, kEncodeSteps + 1> kTable = [] {
        std::array<std::uint8_t, kEncodeSteps + 1> t{};
        for (int i = 0; i <= kEncodeSteps; ++i) {
            float v = float(i) / kEncodeSteps;
            float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
            t[i] = static_cast<std::uint8_t>(std::lround(s * 255.0f));
        }
        return t;
    }();
    return std::byte{kTable[static_cast<std::size_t>(std::clamp(c, 0.0f, 1.0f) * kEncodeSteps + 0.5f)]};
}

using Texels = std::vector<std::array<float, 4>>; // linear rgb, alpha

// Gives fully transparent texels the average colour of their nearest opaque neighbours (a few texels out); the rest
// keep the colour they had.
void grow_color(Texels& t, std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> known(t.size());
    for (std::size_t i = 0; i < t.size(); ++i) known[i] = t[i][3] > 0.0f;
    for (int pass = 0; pass < 8; ++pass) {
        std::vector<std::uint8_t> next = known;
        bool changed = false;
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                std::size_t i = std::size_t(y) * w + x;
                if (known[i]) continue;
                float sum[3] = {};
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        // Wrapped: world textures tile.
                        std::uint32_t nx = (x + w + dx) % w, ny = (y + h + dy) % h;
                        std::size_t j = std::size_t(ny) * w + nx;
                        if (!known[j]) continue;
                        for (int k = 0; k < 3; ++k) sum[k] += t[j][k];
                        ++n;
                    }
                if (n == 0) continue;
                for (int k = 0; k < 3; ++k) t[i][k] = sum[k] / float(n);
                next[i] = 1;
                changed = true;
            }
        known = std::move(next);
        if (!changed) break;
    }
}

} // namespace

bool remove_color_key(Image& image) {
    if (image.format != PixelFormat::RGBA8 || image.mips.empty()) return false;
    const std::uint32_t w = image.width, h = image.height;
    auto& px = image.mips[0];
    const std::size_t n = std::size_t(w) * h;
    if (n == 0 || px.size() < n * 4) return false;
    auto at = [&](std::size_t i, int k) { return std::to_integer<std::uint8_t>(px[i * 4 + k]); };

    // The key: the median colour under alpha 0, found under at least 90% of those texels (a few units apart: DXT
    // colour endpoints vary).
    std::array<std::vector<std::uint8_t>, 3> clear;
    for (std::size_t i = 0; i < n; ++i)
        if (at(i, 3) == 0)
            for (int k = 0; k < 3; ++k) clear[k].push_back(at(i, k));
    const std::size_t n0 = clear[0].size();
    if (n0 < 16 || n0 * 64 < n) return false;
    std::array<int, 3> key{};
    for (int k = 0; k < 3; ++k) {
        auto mid = clear[k].begin() + static_cast<std::ptrdiff_t>(n0 / 2);
        std::nth_element(clear[k].begin(), mid, clear[k].end());
        key[k] = *mid;
    }
    std::size_t near = 0;
    for (std::size_t i = 0; i < n; ++i)
        if (at(i, 3) == 0) {
            bool close = true;
            for (int k = 0; k < 3; ++k) close &= std::abs(int(at(i, k)) - key[k]) <= 24;
            near += close;
        }
    if (near * 10 < n0 * 9) return false;
    // A painted background is a saturated colour (cyan, magenta). Black, white or grey under alpha 0 is as likely a
    // DXT1 cut-out's black or a picture's own shade, and a tangent-space normal map's flat (128, 128, 255) under its
    // gloss alpha is no key either.
    if (*std::max_element(key.begin(), key.end()) - *std::min_element(key.begin(), key.end()) < 64) return false;
    if (std::abs(key[0] - 128) <= 32 && std::abs(key[1] - 128) <= 32) return false;

    // The edge's own colour: that of the mostly opaque texels (key taken out), grown a few texels outwards.
    std::vector<std::array<float, 3>> own(n), edge(n);
    std::vector<std::uint8_t> known(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float a = at(i, 3) / 255.0f;
        for (int k = 0; k < 3; ++k) {
            float c = at(i, k);
            if (a > 0.0f) c = std::clamp((c - (1.0f - a) * float(key[k])) / a, 0.0f, 255.0f);
            own[i][k] = c;
        }
        edge[i] = own[i];
        known[i] = a >= 0.75f;
    }
    for (int pass = 0; pass < 8; ++pass) {
        std::vector<std::uint8_t> next = known;
        bool changed = false;
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::size_t i = std::size_t(y) * w + x;
                if (known[i]) continue;
                float sum[3] = {};
                int count = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        // Wrapped: world textures tile.
                        const std::size_t j = std::size_t((y + h + dy) % h) * w + (x + w + dx) % w;
                        if (!known[j]) continue;
                        for (int k = 0; k < 3; ++k) sum[k] += edge[j][k];
                        ++count;
                    }
                if (count == 0) continue;
                for (int k = 0; k < 3; ++k) edge[i][k] = sum[k] / float(count);
                next[i] = 1;
                changed = true;
            }
        known = std::move(next);
        if (!changed) break;
    }

    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t a8 = at(i, 3);
        if (a8 == 255) continue;
        std::array<float, 3> c = edge[i];
        if (a8 != 0) {
            // Mostly clear: the neighbours' colour; mostly opaque: its own, smoothly between.
            float s = std::clamp((a8 / 255.0f - 0.5f) / 0.5f, 0.0f, 1.0f);
            s = s * s * (3.0f - 2.0f * s);
            for (int k = 0; k < 3; ++k) c[k] = known[i] ? edge[i][k] + (own[i][k] - edge[i][k]) * s : own[i][k];
        } else if (!known[i]) {
            continue; // too far from any opaque texel to matter
        }
        for (int k = 0; k < 3; ++k) px[i * 4 + k] = std::byte(static_cast<std::uint8_t>(std::lround(c[k])));
    }
    return true;
}

void make_opaque(Image& image) {
    if (image.format != PixelFormat::RGBA8) return;
    for (auto& level : image.mips)
        for (std::size_t i = 3; i < level.size(); i += 4) level[i] = std::byte{0xff};
}

bool is_opaque(const Image& image) {
    if (image.format != PixelFormat::RGBA8 || image.mips.empty()) return false;
    const auto& level = image.mips[0];
    if (level.size() < std::size_t(image.width) * image.height * 4) return false;
    for (std::size_t i = 3; i < level.size(); i += 4)
        if (level[i] != std::byte{0xff}) return false;
    return true;
}

void build_color_mips(Image& image, float coverage_ref, bool grow_transparent) {
    if (image.format != PixelFormat::RGBA8 || image.mips.empty() || image.width == 0 || image.height == 0) return;
    std::uint32_t w = image.width, h = image.height;
    const auto& base = image.mips[0];
    if (base.size() < std::size_t(w) * h * 4) return;

    static const std::array<float, 256> kLinear = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) t[i] = srgb_to_linear(float(i) / 255.0f);
        return t;
    }();
    Texels cur(std::size_t(w) * h);
    bool has_clear = false, has_solid = false, binary = true;
    for (std::size_t i = 0; i < cur.size(); ++i) {
        for (int k = 0; k < 3; ++k) cur[i][k] = kLinear[std::to_integer<std::uint8_t>(base[i * 4 + k])];
        std::uint8_t a = std::to_integer<std::uint8_t>(base[i * 4 + 3]);
        cur[i][3] = float(a) / 255.0f;
        has_clear |= a == 0;
        has_solid |= a == 255;
        binary &= a == 0 || a == 255;
    }
    const bool cutout = binary && has_clear && has_solid;
    float coverage = 0.0f;
    if (cutout) {
        for (const auto& t : cur) coverage += t[3] >= coverage_ref;
        coverage /= float(cur.size());
    }
    if (has_clear && grow_transparent) {
        grow_color(cur, w, h);
        // Level 0 keeps its alpha; only the colour under the transparent texels changes.
        auto& out = image.mips[0];
        for (std::size_t i = 0; i < cur.size(); ++i)
            if (cur[i][3] == 0.0f)
                for (int k = 0; k < 3; ++k) out[i * 4 + k] = linear_to_srgb_byte(cur[i][k]);
    }

    image.mips.resize(1);
    while (w > 1 || h > 1) {
        std::uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
        Texels next(std::size_t(nw) * nh);
        for (std::uint32_t y = 0; y < nh; ++y)
            for (std::uint32_t x = 0; x < nw; ++x) {
                float rgb[3] = {}, plain[3] = {}, alpha = 0.0f;
                int n = 0;
                for (std::uint32_t sy = y * 2; sy < std::min(h, y * 2 + 2); ++sy)
                    for (std::uint32_t sx = x * 2; sx < std::min(w, x * 2 + 2); ++sx) {
                        const auto& t = cur[std::size_t(sy) * w + sx];
                        for (int k = 0; k < 3; ++k) {
                            rgb[k] += t[k] * t[3];
                            plain[k] += t[k];
                        }
                        alpha += t[3];
                        ++n;
                    }
                auto& o = next[std::size_t(y) * nw + x];
                for (int k = 0; k < 3; ++k) o[k] = alpha > 1e-6f ? rgb[k] / alpha : plain[k] / float(n);
                o[3] = alpha / float(n);
            }
        // Cut-outs: scale the level's alpha so the share at or above the reference matches level 0's.
        float scale = 1.0f;
        if (cutout && coverage > 0.0f) {
            std::vector<float> alphas(next.size());
            for (std::size_t i = 0; i < next.size(); ++i) alphas[i] = next[i][3];
            std::size_t keep = std::min(alphas.size() - 1,
                                        static_cast<std::size_t>(std::lround((1.0f - coverage) * float(alphas.size()))));
            std::nth_element(alphas.begin(), alphas.begin() + static_cast<std::ptrdiff_t>(keep), alphas.end());
            float threshold = alphas[keep];
            if (threshold > 1e-3f) scale = coverage_ref / threshold;
        }
        std::vector<std::byte> level(next.size() * 4);
        for (std::size_t i = 0; i < next.size(); ++i) {
            for (int k = 0; k < 3; ++k) level[i * 4 + k] = linear_to_srgb_byte(next[i][k]);
            level[i * 4 + 3] = std::byte{static_cast<std::uint8_t>(
                std::lround(std::clamp(next[i][3] * scale, 0.0f, 1.0f) * 255.0f))};
        }
        image.mips.push_back(std::move(level));
        cur = std::move(next);
        w = nw;
        h = nh;
    }
}

std::size_t mip_size(PixelFormat format, std::uint32_t width, std::uint32_t height) {
    std::size_t w = std::max<std::uint32_t>(width, 1), h = std::max<std::uint32_t>(height, 1);
    switch (format) {
    case PixelFormat::RGBA8: return w * h * 4;
    case PixelFormat::BC1:
    case PixelFormat::BC4: return ((w + 3) / 4) * ((h + 3) / 4) * 8;
    case PixelFormat::BC2:
    case PixelFormat::BC3:
    case PixelFormat::BC5:
    case PixelFormat::BC7: return ((w + 3) / 4) * ((h + 3) / 4) * 16;
    }
    return 0;
}

} // namespace hailstone::assets
