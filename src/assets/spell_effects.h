#pragma once

// Particle emitter definitions (.edd) and the spell effect stages
// (spellsnew.eff). Sources: the original's particle code and
// OpenEQ docs/SPELL_EFFECT_FORMAT.md (field meanings where this build was
// not traced).
//
// EDD (EmitterDefinition_ReadEdd @0x10075ad0): "EDD\0", a 4-byte ASCII
// version; version "110" is read directly as (size - 8) / 0x1a0 records,
// record 0 being the file's own "None" sentinel. Other versions are converted
// field by field (atoi(version) - 101 switch @0x10077577) and a file without
// the magic is the legacy headerless 0xc4-byte format; neither occurs in the
// NMS install and both are rejected here.
//
// spellsnew.eff: 268-byte records, a 64-byte name then three stages (cast,
// projectile, impact) of 0x44 bytes: a wave id and four emitter slots
// {emitter index, minimum level, location, attach}
// (SpellEffect_SpawnStageEmitters @0x51aa20 reads +0x44 + stage 0x44 + 16k).

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace hailstone::assets {

inline constexpr std::size_t kEddRecordSize = 0x1a0;

// One 0x1a0-byte EDD 110 record. Offsets in brackets; defaults are
// EmitterDefinition_SetDefaults @0x100712d0's.
struct EmitterDefinition {
    std::string name;    // [0x00] char[64]
    std::string texture; // [0x40] char[32]; SpellEffects\, EnvEmitterEffects\, ActorEffects\ (searched in that order)
    std::uint32_t bone_orientation = 0;  // [0x60] take the orientation from the attached bone
    std::uint32_t no_zwrite = 0;         // [0x64] 1: ZWRITEENABLE off while drawing
    std::uint32_t additive = 0;          // [0x68] 1: DESTBLEND ONE (restored to INVSRCALPHA)
    std::uint32_t actor_scales_size = 0; // [0x6c]
    std::uint32_t billboard = 0;         // [0x70] billboard mode (0, 1, 2 seen)
    std::uint32_t follow_attachment = 0; // [0x74] existing particles follow the attachment
    float duration = 0.0f;               // [0x78] s; negative: abs forced; positive: the caller's duration wins
    float lifetime = -1.0f;              // [0x7c] s; <= 0 uses the duration
    std::uint32_t initial_count = 1;     // [0x80]
    std::uint32_t per_tick = 0;          // [0x84] particles per emission tick
    float ticks_per_second = 0.0f;       // [0x88]
    float delay = 0.0f;                  // [0x8c] emission delay, s
    float fade_in = 0.0f, fade_out = 0.0f, grow = 0.0f, shrink = 0.0f; // [0x90..0x9c] s
    float reduce_distance = 80.0f;       // [0xa0] count-reduction distance
    float max_opacity = 1.0f;            // [0xa4]
    std::uint32_t shape = 0;             // [0xa8] 0 point .. 9 random ring
    glm::vec3 shape_size{0.0f};          // [0xac..0xb4]
    glm::vec3 offset{0.0f};              // [0xb8..0xc0] local offsets
    float angle1 = 0.0f, angle2 = 0.0f;  // [0xc4, 0xc8] orientation, 512 per turn
    float width = 1.0f;                  // [0xcc] quad width (start)
    float depth_offset = 0.0f;           // [0xd0] view-space depth offset
    glm::uvec3 color_start{255};         // [0xd4..0xdc]
    glm::uvec3 color_end{255};           // [0xe0..0xe8]
    // [0xec..0x124] velocity endpoints (min, max) and acceleration of the
    // axial, first transverse, second transverse, radial and orbital motion.
    std::array<glm::vec3, 5> motion{};
    float gravity = 0.0f;                // [0x128] world downward acceleration
    float wind_x = 0.0f;                 // [0x12c] world X acceleration
    std::uint32_t frames = 16;           // [0x130] flipbook frame count
    float frames_per_second = 16.0f;     // [0x134]
    float spin = 0.0f;                   // [0x138] spin endpoint
    std::uint32_t unknown_13c = 0, unknown_140 = 0; // [0x13c, 0x140] not traced
    std::uint32_t capacity = 0;          // [0x144] fixed particle capacity (replaces the computed one)
    std::array<std::uint32_t, 9> globals{}; // [0x148..0x168] copied from DLL globals, not traced
    float scale1 = 1.0f, scale2 = 1.0f;  // [0x16c, 0x170]
    std::uint32_t random_rotation = 0;   // [0x174]
    std::uint32_t unknown_178 = 0;       // [0x178] not traced
    float height_start = 1.0f, height_end = 1.0f, width_end = 1.0f; // [0x17c, 0x180, 0x184]
    float spin2 = 0.0f;                  // [0x188] spin endpoint 2
    std::uint32_t same_random_size = 1;  // [0x18c]
    float shrink_x = 0.0f, shrink_y = 0.0f; // [0x190, 0x194] extra shrink ramps
    std::uint32_t negative_radial = 0;   // [0x198]
    std::uint32_t actor_scales_offsets = 1; // [0x19c]
    // WLD sprites use separate bitmap files in authored order. Empty for an EDD texture's atlas flipbook.
    std::vector<std::string> texture_frames;
};

// Parses an EDD file. Throws std::runtime_error for a missing magic, a version
// other than "110" or a size that is not 8 + n x 0x1a0.
std::vector<EmitterDefinition> parse_edd(std::span<const std::byte> data);
// One 0x1a0-byte record.
EmitterDefinition parse_edd_record(std::span<const std::byte> record);

struct EmitterSlot {
    std::int32_t emitter = 0;   // EDD index; < 1 = none
    std::int32_t min_level = 0; // the caster's level must reach it
    std::int32_t location = 0;  // 0 the given point, 1 caster, 2 missile, 3 target
    std::int32_t attach = 0;    // 0 none, 1..11 bone points, 8 the actor without a bone, 12 point 1 scaled by height
};

struct SpellEffectStage {
    std::int32_t sound = 0;
    std::array<EmitterSlot, 4> slots{};
};

struct SpellEffect {
    std::string name;
    std::array<SpellEffectStage, 3> stages{}; // cast, projectile, impact
};

// spellsnew.eff, indexed by a spell's SpellAnim.
std::vector<SpellEffect> parse_spell_effects(std::span<const std::byte> data);

} // namespace hailstone::assets
