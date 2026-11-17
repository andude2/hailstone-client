#include "assets/legacy_particles.h"

#include <cmath>
#include <stdexcept>
#include <utility>

#include "core/byte_io.h"

namespace hailstone::assets {

namespace {

constexpr std::size_t kStageSize = 0x3a0;
constexpr std::size_t kSwitchStage = 8 + 2 * kStageSize;
constexpr std::uint32_t kLongestSwitchMs = 43200000; // the switch's twelve-hour request
constexpr float kLargestField = 1000000.0f;

bool safe_name(const std::string& name) {
    if (name.empty() || name.size() >= 32 || name == "." || name == "..") return false;
    for (const char raw : name) {
        const auto c = static_cast<unsigned char>(raw);
        if (c < 32 || c > 126 || c == '/' || c == '\\' || c == ':') return false;
    }
    return true;
}

bool bounded(float value) { return std::isfinite(value) && std::fabs(value) <= kLargestField; }

bool valid(const LegacyParticleSlot& s) {
    return safe_name(s.sprite) && bounded(s.gravity) && bounded(s.direction.x) && bounded(s.direction.y) &&
           bounded(s.direction.z) && bounded(s.radius) && s.radius >= 0.0f && bounded(s.angle) &&
           s.lifetime_ms <= kLongestSwitchMs && bounded(s.velocity) && s.interval_ms <= kLongestSwitchMs &&
           bounded(s.scale) && s.scale >= 0.0f;
}

} // namespace

std::vector<LegacySwitchEffect> parse_legacy_switch_effects(std::span<const std::byte> data) {
    if (data.size() != kLegacyParticleEffectCount * kLegacyParticleEffectSize)
        throw std::runtime_error("spells.eff size is not 256 x 0xae8");
    std::vector<LegacySwitchEffect> out(kLegacyParticleEffectCount);
    for (std::size_t effect = 0; effect < out.size(); ++effect) {
        ByteReader r(data.subspan(effect * kLegacyParticleEffectSize + kSwitchStage, kStageSize));
        for (std::size_t slot = 0; slot < out[effect].size(); ++slot) {
            // DoParticleEffect 0x00521ca0, selector 1, calls the explicit-position
            // template. The other selectors depend on actors or spell points.
            r.seek(0x8c + slot * 4);
            if (r.le<std::int32_t>() != 1) continue;
            LegacyParticleSlot s;
            r.seek(slot * 32);
            s.sprite = r.fixed_string(32);
            r.seek(0x220 + slot * 4);
            s.color = r.le<std::uint32_t>();
            r.seek(0x22c + slot * 4);
            s.gravity = r.f32();
            r.seek(0x238 + slot * 12);
            const float x = r.f32();
            const float y = r.f32();
            s.direction = {x, y, r.f32()};
            r.seek(0x25c + slot * 4);
            s.radius = r.f32();
            r.seek(0x268 + slot * 4);
            s.angle = r.f32();
            r.seek(0x274 + slot * 4);
            s.lifetime_ms = r.le<std::uint32_t>();
            r.seek(0x280 + slot * 4);
            s.velocity = r.f32();
            r.seek(0x28c + slot * 4);
            s.interval_ms = r.le<std::uint32_t>();
            r.seek(0x298 + slot * 4);
            s.scale = r.f32();
            if (valid(s)) out[effect][slot] = std::move(s);
        }
    }
    return out;
}

std::optional<EmitterDefinition> legacy_switch_definition(const LegacyParticleSlot& s, const ParticleTexture& texture) {
    if (!valid(s) || texture.file.empty()) return std::nullopt;
    // The explicit-position template installed by 0x004ad960, with the nonzero
    // stage overrides from 0x0051a810. Its external descriptor is converted by
    // 0x10071640; it differs from a WLD cloud in gravity, tilt, radius and size.
    wld::ParticleCloud c;
    c.selector = 3;
    c.bits = 0x701;
    c.capacity = 2000;
    c.mode = 3;
    c.duration_ms = kLongestSwitchMs;
    c.lifetime_ms = s.lifetime_ms ? s.lifetime_ms : 2000;
    c.speed = s.velocity != 0.0f ? s.velocity : 20.0f;
    c.angle = s.angle != 0.0f ? s.angle : 20.0f;
    c.interval_ms = s.interval_ms ? s.interval_ms : 1;
    c.size = s.scale != 0.0f ? s.scale : 0.2f;
    c.color = (s.color & 0xffffffu) ? s.color : 0xffffffffu;
    // A neutral converter name avoids the WLD-only TFIRE2 override. The old
    // external descriptor has its own name-specific size handling below.
    EmitterDefinition d = particle_cloud_definition(c, texture, "LegacySwitch");
    d.name = s.sprite;
    d.gravity = s.gravity != 0.0f ? s.gravity : -1.0f;
    d.shape_size.x = s.radius;
    glm::vec3 direction = s.direction;
    if (direction == glm::vec3(0.0f)) direction = {-1.0f, 0.0f, 0.0f};
    d.angle1 = d.angle2 = 0.0f;
    const float tilt = (1.0f - c.angle / 512.0f) * 100.0f;
    if (direction.x != 0.0f) d.angle1 = tilt * direction.x;
    else if (direction.z != 0.0f) d.angle1 = direction.z == 1.0f ? 0.0f : 256.0f;
    else if (direction.y != 0.0f) d.angle2 = tilt * direction.y;
    // The particle simulator's interpolated sine/cosine casts these to int.
    // Two individually bounded input fields must not produce an unsafe tilt.
    if (!bounded(d.angle1) || !bounded(d.angle2)) return std::nullopt;
    d.scale1 *= 1.5f;
    // Unlike the WLD converter, these external-name tests are case-sensitive.
    // The smoke substring test comes last and restores its unscaled sprite.
    if (s.sprite.starts_with("TFIRE2")) {
        d.lifetime = 0.08f;
        d.capacity = 8;
        d.shape_size.x = 0.05f;
        d.ticks_per_second = 33.333332f;
    }
    if (s.sprite.find("I_TSMOKE") != std::string::npos) {
        d.ticks_per_second = 1000.0f / static_cast<float>(c.interval_ms);
        d.scale1 = c.size;
    }
    return d;
}

} // namespace hailstone::assets
