#pragma once

// The explicit-position particle slots used by classic switches (spells.eff,
// not spellsnew.eff). The old table has 256 records of 0xae8 bytes; each holds
// three 0x3a0-byte stages after its two-word header. Switches use stage 2.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "assets/spell_effects.h"
#include "assets/wld_particles.h"

namespace hailstone::assets {

inline constexpr std::size_t kLegacyParticleEffectCount = 256;
inline constexpr std::size_t kLegacyParticleEffectSize = 0xae8;

// Raw stage overrides: a zero value keeps the explicit-position template's
// default (a direction is replaced only when at least one component is nonzero).
struct LegacyParticleSlot {
    std::string sprite;
    float gravity = 0.0f;
    glm::vec3 direction{0.0f};
    float radius = 0.0f;
    float angle = 0.0f;
    std::uint32_t lifetime_ms = 0;
    float velocity = 0.0f;
    std::uint32_t interval_ms = 0; // the old field called Rate is an interval, not particles per second
    float scale = 0.0f;
    std::uint32_t color = 0;
};

using LegacySwitchEffect = std::array<std::optional<LegacyParticleSlot>, 3>;

// Keeps the original effect and slot ordinals. Only selector 1 (an explicit
// position) is supported; other selectors, malformed names and unsafe numeric
// values leave that slot empty. Throws for a file whose size is not exactly
// 256 records. No authored effect definitions are supplied by the client.
std::vector<LegacySwitchEffect> parse_legacy_switch_effects(std::span<const std::byte> data);

// Converts an admitted slot and its resolved global WLD sprite to the common
// particle descriptor. Returns nullopt for an invalid slot or missing texture.
std::optional<EmitterDefinition> legacy_switch_definition(const LegacyParticleSlot& slot,
                                                         const ParticleTexture& texture);

} // namespace hailstone::assets
