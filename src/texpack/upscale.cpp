#include "texpack/upscale.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>

namespace hailstone::texpack {

using assets::Image;

Planes to_planes(const Image& rgba, std::uint32_t channels) {
    Planes out(rgba.width, rgba.height, channels);
    const auto& px = rgba.mips[0];
    const std::size_t n = std::size_t(rgba.width) * rgba.height;
    for (std::size_t i = 0; i < n; ++i)
        for (std::uint32_t c = 0; c < channels; ++c)
            out.px[i * channels + c] = static_cast<float>(std::to_integer<std::uint8_t>(px[i * 4 + c])) / 255.0f;
    return out;
}

namespace {

std::byte quantize(float v) {
    // (as numpy's (clip(v, 0, 1) * 255 + 0.5).astype(uint8))
    return std::byte(static_cast<std::uint8_t>(std::floor(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f)));
}

} // namespace

Image to_image(const Planes& rgb, const Planes* alpha) {
    Image out;
    out.width = rgb.width;
    out.height = rgb.height;
    out.format = assets::PixelFormat::RGBA8;
    const std::size_t n = std::size_t(rgb.width) * rgb.height;
    std::vector<std::byte> px(n * 4);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::uint32_t c = 0; c < 3; ++c) px[i * 4 + c] = quantize(rgb.px[i * rgb.channels + std::min(c, rgb.channels - 1)]);
        px[i * 4 + 3] = alpha ? quantize(alpha->px[i]) : std::byte{255};
    }
    out.mips.push_back(std::move(px));
    return out;
}

Planes channel(const Planes& img, std::uint32_t c) {
    Planes out(img.width, img.height, 1);
    const std::size_t n = std::size_t(img.width) * img.height;
    for (std::size_t i = 0; i < n; ++i) out.px[i] = img.px[i * img.channels + c];
    return out;
}

Planes rgb_of(const Planes& img) {
    if (img.channels == 3) return img;
    Planes out(img.width, img.height, 3);
    const std::size_t n = std::size_t(img.width) * img.height;
    for (std::size_t i = 0; i < n; ++i)
        for (std::uint32_t c = 0; c < 3; ++c) out.px[i * 3 + c] = img.px[i * img.channels + std::min(c, img.channels - 1)];
    return out;
}

namespace {

// The source index of padded index i (-p .. n + p) for a side of n texels.
std::uint32_t edge_index(std::int64_t i, std::uint32_t n, Edge edge) {
    if (n == 1) return 0;
    if (edge == Edge::Wrap) {
        std::int64_t m = i % n;
        return static_cast<std::uint32_t>(m < 0 ? m + n : m);
    }
    const std::int64_t period = 2 * (static_cast<std::int64_t>(n) - 1);
    std::int64_t m = i % period;
    if (m < 0) m += period;
    if (m >= n) m = period - m;
    return static_cast<std::uint32_t>(m);
}

} // namespace

Planes pad(const Planes& img, std::uint32_t p, Edge edge) {
    Planes out(img.width + 2 * p, img.height + 2 * p, img.channels);
    for (std::uint32_t y = 0; y < out.height; ++y) {
        const std::uint32_t sy = edge_index(static_cast<std::int64_t>(y) - p, img.height, edge);
        for (std::uint32_t x = 0; x < out.width; ++x) {
            const std::uint32_t sx = edge_index(static_cast<std::int64_t>(x) - p, img.width, edge);
            for (std::uint32_t c = 0; c < img.channels; ++c) out.at(x, y, c) = img.at(sx, sy, c);
        }
    }
    return out;
}

Planes crop(const Planes& img, std::uint32_t x0, std::uint32_t y0, std::uint32_t w, std::uint32_t h) {
    Planes out(w, h, img.channels);
    for (std::uint32_t y = 0; y < h; ++y) {
        const float* src = &img.px[(std::size_t(y0 + y) * img.width + x0) * img.channels];
        std::copy(src, src + std::size_t(w) * img.channels, &out.px[std::size_t(y) * w * img.channels]);
    }
    return out;
}

namespace {

double sinc(double x) {
    if (x == 0.0) return 1.0;
    x *= std::numbers::pi;
    return std::sin(x) / x;
}

double lanczos3(double x) { return -3.0 <= x && x < 3.0 ? sinc(x) * sinc(x / 3.0) : 0.0; }

// PIL's precompute_coeffs (Resample.c) for a side of `in` texels to `out`: each output texel's first source texel
// and its weights.
struct Coeffs {
    std::vector<std::uint32_t> first;
    std::vector<std::vector<double>> weights;
};
Coeffs coeffs(std::uint32_t in, std::uint32_t out) {
    const double scale = static_cast<double>(in) / out;
    const double filterscale = std::max(scale, 1.0);
    const double support = 3.0 * filterscale;
    const double ss = 1.0 / filterscale;
    Coeffs c;
    c.first.resize(out);
    c.weights.resize(out);
    for (std::uint32_t xx = 0; xx < out; ++xx) {
        const double center = (xx + 0.5) * scale;
        // ((int) truncates toward zero, then clamps: as PIL)
        auto xmin = static_cast<std::int64_t>(center - support + 0.5);
        if (xmin < 0) xmin = 0;
        auto xmax = static_cast<std::int64_t>(center + support + 0.5);
        if (xmax > in) xmax = in;
        std::vector<double> w(static_cast<std::size_t>(std::max<std::int64_t>(0, xmax - xmin)));
        double sum = 0.0;
        for (std::size_t x = 0; x < w.size(); ++x) {
            w[x] = lanczos3((static_cast<double>(x) + static_cast<double>(xmin) - center + 0.5) * ss);
            sum += w[x];
        }
        if (sum != 0.0)
            for (double& v : w) v /= sum;
        c.first[xx] = static_cast<std::uint32_t>(xmin);
        c.weights[xx] = std::move(w);
    }
    return c;
}

} // namespace

Planes resize_lanczos(const Planes& img, std::uint32_t w, std::uint32_t h) {
    Planes cur = img;
    if (w != cur.width) {
        const Coeffs k = coeffs(cur.width, w);
        Planes out(w, cur.height, cur.channels);
        for (std::uint32_t y = 0; y < cur.height; ++y)
            for (std::uint32_t x = 0; x < w; ++x)
                for (std::uint32_t c = 0; c < cur.channels; ++c) {
                    double s = 0.0;
                    for (std::size_t i = 0; i < k.weights[x].size(); ++i)
                        s += cur.at(k.first[x] + static_cast<std::uint32_t>(i), y, c) * k.weights[x][i];
                    out.at(x, y, c) = static_cast<float>(s);
                }
        cur = std::move(out);
    }
    if (h != cur.height) {
        const Coeffs k = coeffs(cur.height, h);
        Planes out(cur.width, h, cur.channels);
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < cur.width; ++x)
                for (std::uint32_t c = 0; c < cur.channels; ++c) {
                    double s = 0.0;
                    for (std::size_t i = 0; i < k.weights[y].size(); ++i)
                        s += cur.at(x, k.first[y] + static_cast<std::uint32_t>(i), c) * k.weights[y][i];
                    out.at(x, y, c) = static_cast<float>(s);
                }
        cur = std::move(out);
    }
    for (float& v : cur.px) v = std::clamp(v, 0.0f, 1.0f);
    return cur;
}

void bleed(Planes& rgb, const Planes& alpha) {
    const std::uint32_t w = rgb.width, h = rgb.height, ch = rgb.channels;
    const std::size_t n = std::size_t(w) * h;
    std::vector<char> known(n);
    std::size_t count = 0;
    for (std::size_t i = 0; i < n; ++i) count += (known[i] = alpha.px[i] > 0.0f) != 0;
    if (count == n || count == 0) return;
    constexpr int kDirs[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
    std::vector<char> grow(n);
    std::vector<float> acc(n * ch);
    for (int round = 0; round < 64 && count < n; ++round) {
        std::fill(grow.begin(), grow.end(), char(0));
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::size_t i = std::size_t(y) * w + x;
                if (known[i]) continue;
                float sum[4] = {};
                int k = 0;
                for (const auto& d : kDirs) {
                    // (np.roll: the texel d away, wrapping at the edges)
                    const auto sy = static_cast<std::uint32_t>((static_cast<std::int64_t>(y) - d[0] + h) % h);
                    const auto sx = static_cast<std::uint32_t>((static_cast<std::int64_t>(x) - d[1] + w) % w);
                    const std::size_t j = std::size_t(sy) * w + sx;
                    if (!known[j]) continue;
                    ++k;
                    for (std::uint32_t c = 0; c < ch; ++c) sum[c] += rgb.px[j * ch + c];
                }
                if (!k) continue;
                grow[i] = 1;
                for (std::uint32_t c = 0; c < ch; ++c) acc[i * ch + c] = sum[c] / static_cast<float>(k);
            }
        for (std::size_t i = 0; i < n; ++i)
            if (grow[i]) {
                for (std::uint32_t c = 0; c < ch; ++c) rgb.px[i * ch + c] = acc[i * ch + c];
                known[i] = 1;
                ++count;
            }
    }
    if (count == n) return;
    double mean[4] = {};
    std::size_t m = 0;
    for (std::size_t i = 0; i < n; ++i)
        if (known[i]) {
            ++m;
            for (std::uint32_t c = 0; c < ch; ++c) mean[c] += rgb.px[i * ch + c];
        }
    for (std::size_t i = 0; i < n; ++i)
        if (!known[i])
            for (std::uint32_t c = 0; c < ch; ++c) rgb.px[i * ch + c] = static_cast<float>(mean[c] / static_cast<double>(m));
}

Planes run_tiled(Model& model, const Planes& img, std::uint32_t tile, std::uint32_t overlap) {
    const std::uint32_t s = model.scale();
    const std::uint32_t h = img.height, w = img.width;
    Planes out(w * s, h * s, 3);
    std::vector<float> weight(std::size_t(w) * s * h * s, 0.0f);
    const std::uint32_t step = tile - 2 * overlap;
    auto starts = [&](std::uint32_t n) {
        std::vector<std::uint32_t> v;
        if (n <= tile) return std::vector<std::uint32_t>{0};
        for (std::uint32_t i = 0; i < std::max<std::uint32_t>(n - 2 * overlap, 1); i += step) v.push_back(i);
        return v;
    };
    for (std::uint32_t y0 : starts(h))
        for (std::uint32_t x0 : starts(w)) {
            const std::uint32_t y1 = std::min(y0 + tile, h), x1 = std::min(x0 + tile, w);
            const std::uint32_t y0c = y1 > tile ? y1 - tile : 0, x0c = x1 > tile ? x1 - tile : 0;
            Planes r = model.run(crop(img, x0c, y0c, x1 - x0c, y1 - y0c));
            const std::uint32_t th = r.height, tw = r.width;
            // Feathered weights so tile seams blend.
            const float cap = static_cast<float>(overlap * s);
            for (std::uint32_t y = 0; y < th && y0c * s + y < out.height; ++y) {
                const float wy = std::min(static_cast<float>(std::min(y + 1, th - y)), cap);
                for (std::uint32_t x = 0; x < tw && x0c * s + x < out.width; ++x) {
                    const float wt = wy * std::min(static_cast<float>(std::min(x + 1, tw - x)), cap);
                    const std::uint32_t ox = x0c * s + x, oy = y0c * s + y;
                    for (std::uint32_t c = 0; c < 3; ++c)
                        out.at(ox, oy, c) += std::clamp(r.at(x, y, std::min(c, r.channels - 1)), 0.0f, 1.0f) * wt;
                    weight[std::size_t(oy) * out.width + ox] += wt;
                }
            }
        }
    for (std::size_t i = 0; i < weight.size(); ++i)
        for (std::uint32_t c = 0; c < 3; ++c) out.px[i * 3 + c] /= std::max(weight[i], 1e-6f);
    return out;
}

std::optional<Image>& Outputs::of(FileKind kind) {
    switch (kind) {
    case FileKind::Color: return color;
    case FileKind::Masked: return masked;
    case FileKind::Keyed: return keyed;
    case FileKind::Normal: return normal;
    case FileKind::Rough: return rough;
    }
    return color;
}

namespace {

// The model over `rgb` with its edges padded, at the model's scale, cropped back to the image.
Planes model_padded(Model& model, const Planes& rgb, Edge edge) {
    const std::uint32_t p = std::min({kEdgePad, rgb.width, rgb.height});
    Planes up = run_tiled(model, pad(rgb, p, edge));
    const std::uint32_t s = model.scale();
    return crop(up, p * s, p * s, rgb.width * s, rgb.height * s);
}

} // namespace

Outputs upscale_texture(const TextureInfo& info, const Sources& sources, const Models& models,
                        const UpscaleSettings& settings, const std::vector<FileKind>& wanted) {
    auto want = [&](FileKind k) { return std::find(wanted.begin(), wanted.end(), k) != wanted.end(); };
    const Image& src = sources.color;
    // Refused past kMaxUpscaleTexels, at the pack's size or the colour model's (which pads and scales first): the
    // float planes take 16 bytes a texel, and 8192 x 8192 at 8x would ask for some 50 GB.
    auto too_large = [](std::uint64_t w, std::uint64_t h) { return w * h > kMaxUpscaleTexels; };
    const std::uint64_t model_scale = uses_models(info) && models.color ? models.color->scale() : 1;
    if (too_large(std::uint64_t{src.width} * settings.scale, std::uint64_t{src.height} * settings.scale) ||
        too_large((std::uint64_t{src.width} + 2 * kEdgePad) * model_scale,
                  (std::uint64_t{src.height} + 2 * kEdgePad) * model_scale))
        throw std::runtime_error("too large to upscale: " + std::to_string(src.width) + "x" +
                                 std::to_string(src.height) + " at " + std::to_string(settings.scale) + "x");
    const std::uint32_t W = src.width * settings.scale, H = src.height * settings.scale;
    const Edge edge = reflects_edges(info) ? Edge::Reflect : Edge::Wrap;
    const bool plain_ml = uses_models(info) && models.color;

    // Only a colour key (the masked variant's palette index 0, the keyed variant's key) hides nothing worth keeping.
    // A DDS's alpha 0 keeps the picture an opaque material draws (the hero's forge plate generic_plate_blue_c.dds
    // holds all but its cloth under alpha 0), so its colour goes to the model as stored.
    auto colour = [&](const Image& img, AlphaKind kind, bool keyed, Planes* rgb_out) {
        Planes rgba = to_planes(img, 4);
        Planes rgb = rgb_of(rgba);
        Planes alpha = channel(rgba, 3);
        if (keyed && kind != AlphaKind::None) bleed(rgb, alpha);
        if (plain_ml) {
            rgb = model_padded(*models.color, rgb, edge);
            if (rgb.width != W || rgb.height != H) rgb = resize_lanczos(rgb, W, H);
        } else {
            rgb = resize_lanczos(rgb, W, H);
        }
        Planes a(W, H, 1, 1.0f);
        if (kind != AlphaKind::None) {
            a = resize_lanczos(alpha, W, H);
            if (kind == AlphaKind::Cutout)
                for (float& v : a.px) v = v >= 0.5f ? 1.0f : 0.0f;
        }
        Image out = to_image(rgb, &a);
        if (rgb_out) *rgb_out = std::move(rgb);
        return out;
    };

    Outputs out;
    Planes rgb;
    // (the colour is always made: the maps are made from it)
    Image color = colour(src, info.alpha, false, &rgb);
    if (want(FileKind::Color)) out.color = std::move(color);
    if (want(FileKind::Masked) && sources.masked) {
        const AlphaKind kind = info.alpha == AlphaKind::Blend ? AlphaKind::Blend : AlphaKind::Cutout;
        out.masked = colour(*sources.masked, kind, true, nullptr);
    }
    // The colour key already taken out (decode_sources): its clear texels hold the leaves' colour.
    if (want(FileKind::Keyed) && sources.keyed) out.keyed = colour(*sources.keyed, info.alpha, true, nullptr);
    if (settings.maps && plain_ml) {
        if (want(FileKind::Normal) && models.normal) out.normal = to_image(model_padded(*models.normal, rgb, edge));
        if (want(FileKind::Rough) && models.rough) {
            Planes r = channel(model_padded(*models.rough, rgb, edge), 0);
            out.rough = to_image(r);
        }
    }
    return out;
}

} // namespace hailstone::texpack
