#pragma once

// Server-placed legacy particle clouds (type-53 switches). Their effect number
// selects stage 2 of spells.eff, with sprites supplied by gequip.s3d.

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "game/doors.h"
#include "game/particles.h"
#include "render/particle_renderer.h"

namespace hailstone { class GameDir; }
namespace hailstone::assets { class PfsArchive; }

namespace hailstone::app {

class SwitchParticles {
public:
    using Effect = std::array<std::optional<assets::EmitterDefinition>, 3>;
    // Headless constructor: the same lifecycle and simulation without a renderer.
    explicit SwitchParticles(std::vector<Effect> effects);
    explicit SwitchParticles(const GameDir& game);
    SwitchParticles(render::VkContext& vk, const GameDir& game, const std::filesystem::path& shader_dir);

    bool start(const game::Door& door, std::int32_t effect, std::uint8_t level = 255);
    bool active(const game::Door& door) const;
    void stop(const game::Door& door);
    // A replacement OP_SpawnDoor list can remove emitters without a state change.
    void reconcile(std::span<const game::Door> doors);
    void clear();
    void update(float dt, const glm::vec3& viewer);
    void set_density(float density);
    void draw(const render::FrameInfo& frame, const glm::mat4& view_proj, const glm::mat4& view,
              float glow = 1.0f, const render::SceneLighting* modern = nullptr);
    const game::ParticleSystem& simulation() const { return particles_; }

private:
    void set_effects(std::vector<Effect> effects);
    struct Live {
        std::int32_t effect = 0;
        glm::vec3 position{0.0f};
    };
    std::vector<std::array<std::size_t, 3>> effects_;
    std::array<std::optional<Live>, 256> live_{};
    game::ParticleSystem particles_{std::vector<assets::EmitterDefinition>{assets::EmitterDefinition{}}};
    float density_ = 1.0f;
    std::shared_ptr<const assets::PfsArchive> archive_;
    std::unique_ptr<render::ParticleRenderer> renderer_;
    render::VkContext* vk_ = nullptr;
    const GameDir* game_ = nullptr;
    std::filesystem::path shader_dir_;
};

} // namespace hailstone::app
