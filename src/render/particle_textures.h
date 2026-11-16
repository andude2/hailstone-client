#pragma once

// What render::ParticleRenderer puts up for one particle texture, made on its loader thread rather than in the frame
// (a new spell effect's first cast read, decoded and averaged its textures on the main thread). Kept apart from Vulkan
// so it is tested.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "assets/texture.h"

namespace hailstone { class GameDir; }
namespace hailstone::assets { struct EmitterDefinition; }

namespace hailstone::render {

struct ParticleTextureFrame {
    std::string_view texture;
    glm::uvec2 grid{1};
    std::uint32_t atlas_frame = 0;
};

// EDD frames occupy cells of one texture; WLD sprites supply a separate whole bitmap per frame.
ParticleTextureFrame particle_texture_frame(const assets::EmitterDefinition& definition, std::uint32_t frame);

struct LoadedParticleTexture {
    std::optional<assets::Image> image; // as decoded; nullopt when no folder has it or it does not decode
    glm::vec3 color{1.0f};              // ParticleRenderer::texture_color
};

// The texture `name` (lower case): `zone_bytes` (the zone's archives' copy) when given, else the first of the game
// folder's SpellEffects/, EnvEmitterEffects/ and ActorEffects/ that has the file; the first source that has it is the
// one used, decoded or not. Its colour is the mean of rgb x alpha, scaled so the brightest channel is 1 (white for
// black or none).
LoadedParticleTexture load_particle_texture(const std::string& name, std::optional<std::vector<std::byte>> zone_bytes,
                                            const GameDir& game);

} // namespace hailstone::render
