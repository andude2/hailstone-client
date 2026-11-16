#include "assets/wld_particles.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <numbers>

namespace hailstone::assets {

std::optional<ParticleTexture> particle_texture(const wld::WldFile& wld, wld::Ref ref) {
    const auto* blit = wld.get<wld::ParticleSprite>(ref);
    const auto* sprite = blit ? wld.get<wld::BitmapInfoRef>(blit->sprite) : nullptr;
    const auto* info = sprite ? wld.get<wld::BitmapInfo>(sprite->info) : nullptr;
    if (!info || info->frames.empty()) return std::nullopt;
    const auto* first = wld.get<wld::BitmapName>(info->frames.front());
    if (!first || first->files.empty() || first->files.front().empty()) return std::nullopt;
    ParticleTexture t;
    for (const auto frame : info->frames) {
        const auto* bmp = wld.get<wld::BitmapName>(frame);
        const bool whole = bmp && !bmp->files.empty() &&
                           std::none_of(bmp->files.begin(), bmp->files.end(), [](const std::string& f) { return f.empty(); });
        if (!whole) {
            // A damaged later frame: the first bitmap alone, still, as before the sequences were read (a zone's cloud
            // must not vanish), rather than a sequence silently shortened.
            t.files.assign(1, first->files.front());
            break;
        }
        t.files.insert(t.files.end(), bmp->files.begin(), bmp->files.end());
    }
    t.file = t.files.front();
    t.frames = static_cast<std::uint32_t>(t.files.size());
    t.timing_ms = info->frame_delay_ms;
    t.material = blit->material;
    return t;
}

std::optional<std::uint32_t> particle_material_alias(std::uint32_t handle) {
    // The default installer writes 0x80000017 -> 0x011b0507; the setter keeps
    // the low 31 bits as the index (< 42) into the state cache's table.
    if (handle == 0x80000017u) return 0x011b0507u;
    if (handle == 0x80000018u) return 0x01080507u; // blood's alpha material (default installer 0x1009abe1)
    if (handle == 0x80000019u) return 0x00000587u; // masked sprite (default installer 0x1009aa40)
    return std::nullopt;
}

float particle_tan(float angle) {
    // _ftol truncates; the index is masked to the first quarter turn, so 128
    // (a right angle) wraps to 0 rather than reaching the pole. A NaN, an infinity or a value past int32 is not cast
    // (undefined in C++); _ftol's integer indefinite masks to index 0, so it gives tan(0).
    if (!(std::fabs(angle) < 2147483648.0f)) return 0.0f;
    auto i = static_cast<std::int32_t>(angle) & 0x7f;
    return static_cast<float>(std::tan(static_cast<double>(i) * 2.0 * std::numbers::pi / 512.0));
}

EmitterDefinition particle_cloud_definition(const wld::ParticleCloud& c, const ParticleTexture& tex,
                                            std::string_view name) {
    // Starts from EmitterDefinition_SetDefaults' values (0x100712d0, which the
    // bulk reader calls on the descriptor first), then the converter's writes
    // in its order. Comments give the descriptor offsets.
    EmitterDefinition d;
    d.name = std::string(name);
    d.texture = tex.file;
    d.texture_frames = tex.files;
    d.unknown_13c = c.selector; // +0x13c
    d.unknown_140 = c.bits;     // +0x140
    if (c.bits & 0xc000) d.follow_attachment = 1;                              // +0x74
    if ((c.bits & 0x10000) || d.follow_attachment == 1) d.actor_scales_size = 1; // +0x6c
    d.bone_orientation = 1; // +0x60: the owner node's basis
    d.capacity = c.capacity; // +0x144
    d.gravity = c.extra[0];  // +0x128
    // +0x148..+0x168: the record's remaining nine floats (body +0x18..+0x20,
    // then the reader's copy of the flag-0x01 triples); not read by the update
    // as far as traced.
    d.globals[0] = std::bit_cast<std::uint32_t>(c.extra[1]);
    d.globals[1] = std::bit_cast<std::uint32_t>(c.extra[2]);
    d.globals[2] = std::bit_cast<std::uint32_t>(c.extra[3]);
    for (int k = 0; k < 3; ++k) {
        d.globals[3 + k] = std::bit_cast<std::uint32_t>(c.ranges[0][k]);
        d.globals[6 + k] = std::bit_cast<std::uint32_t>(c.ranges[1][k]);
    }
    // Velocity along the cloud's direction (modes 0, 4 and 5): transverse 1,
    // transverse 2 and axial take direction x, y and z times the speed, each
    // as a fixed (min = max) value.
    auto directed = [&] {
        d.motion[1].x = d.motion[1].y = c.direction.x * c.speed;
        d.motion[2].x = d.motion[2].y = c.direction.y * c.speed;
        d.motion[0].x = d.motion[0].y = c.direction.z * c.speed;
    };
    switch (c.mode) {
    case 0: // box surface, half the flag-0x01 ranges' extent
        d.shape = 6;
        d.shape_size = glm::abs(c.ranges[1] - c.ranges[0]) * 0.5f;
        directed();
        break;
    case 1: // sphere surface, radial speed a quarter of the speed up to the speed
        d.shape = 5;
        d.shape_size.x = c.radius;
        d.motion[3].x = c.speed * 0.25f;
        d.motion[3].y = c.speed;
        break;
    case 2: // ring, fixed radial speed
        d.shape = 9;
        d.shape_size.x = c.radius;
        d.motion[3].x = d.motion[3].y = c.speed;
        break;
    case 3: { // a cone: a point (or a 0.1 disk with bit 0x80) spraying along one axis
        d.shape = 4;
        d.shape_size.x = (c.bits & 0x80) ? 0.1f : 0.0f;
        // The axis: the default +Z of the node, or a cardinal direction turned
        // to by a quarter or half turn in +0xc4 or +0xc8 (exact compares, in
        // this order; another direction keeps +Z).
        if (c.direction.x == 1.0f) d.angle1 = 128.0f;
        else if (c.direction.z == -1.0f) d.angle1 = 256.0f;
        else if (c.direction.x == -1.0f) d.angle1 = -128.0f;
        else if (c.direction.y == 1.0f) d.angle2 = -128.0f;
        else if (c.direction.y == -1.0f) d.angle2 = 128.0f;
        d.motion[0].x = d.motion[0].y = c.speed;
        d.motion[3].x = 0.0f;
        d.motion[3].y = c.speed * particle_tan(c.angle); // spread: speed x tan(angle)
        break;
    }
    case 4: // disk of the radius
        d.shape = 4;
        d.shape_size.x = c.radius;
        directed();
        break;
    case 5: // ring of the radius
        d.shape = 9;
        d.shape_size.x = c.radius;
        directed();
        break;
    default: break; // the switch has no case: SetDefaults' point
    }
    // +0x78: -(ms x 0.001), or -9999 for 0 (indefinite: CreateEmitter takes
    // the absolute value of a negative duration).
    d.duration = c.duration_ms == 0 ? -9999.0f : -(static_cast<float>(c.duration_ms) * 0.001f);
    d.lifetime = static_cast<float>(c.lifetime_ms) * 0.001f; // +0x7c
    // Bit 0x100: fade out over the whole life at full opacity; without it no
    // fade and half opacity (+0x170 and +0xa4 take the same value).
    if (c.bits & 0x100) {
        d.fade_out = d.lifetime; // +0x94
        d.scale2 = d.max_opacity = 1.0f;
    } else {
        d.scale2 = d.max_opacity = 0.5f;
    }
    d.per_tick = 1; // +0x84 (+0x80, the initial count, stays SetDefaults' 1)
    d.ticks_per_second = c.interval_ms ? 1000.0f / static_cast<float>(c.interval_ms) : 0.0f; // +0x88
    d.scale1 = c.size; // +0x16c: the update scales the quad by it (0x10074aaf)
    d.color_start = d.color_end = glm::uvec3((c.color >> 16) & 0xff, (c.color >> 8) & 0xff, c.color & 0xff);
    // The texture: frame rate 1000 / its timing (a timing below 2 counts as
    // 100), at least one frame. WLD bitmap sequences remain separate files; the renderer selects one full texture.
    d.frames_per_second = 1000.0f / static_cast<float>(tex.timing_ms < 2 ? 100u : tex.timing_ms); // +0x134
    d.frames = std::max(tex.frames, 1u); // +0x130
    // +0x68: a material with bit 0x1000000 picks the blend from bit 20
    // (DESTBLEND ONE when set); negative words go through the alias table.
    std::optional<std::uint32_t> material = tex.material;
    if (tex.material & 0x80000000u) material = particle_material_alias(tex.material);
    if (material && (*material & 0x1000000u)) d.additive = (*material >> 20) & 1u;
    d.no_zwrite = 1; // +0x64
    // A "TFIRE2" name (strnicmp, 6 characters) gets a fixed small fast fire.
    auto prefix = name.substr(0, 6);
    if (prefix.size() == 6 && std::equal(prefix.begin(), prefix.end(), "TFIRE2", [](char a, char b) {
            return std::toupper(static_cast<unsigned char>(a)) == b;
        })) {
        d.per_tick = 1;
        d.lifetime = 0.08f;
        d.capacity = 8;
        d.shape_size.x = 0.05f;
        d.ticks_per_second = 33.333332f;
    }
    return d;
}

} // namespace hailstone::assets
