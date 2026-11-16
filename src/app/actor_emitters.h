#pragma once

// The actors' own particle emitters (assets/model_points.h ModelEmitter): an
// EQG model's `<model>.prt`, a held EQG item's, and WLD skeleton particle
// clouds, e.g. a glowing weapon or a
// creature's flames. 0x1006b130 / 0x1006b290 make one through
// CParticleSystem vtable +0x20 (0x10070640, ActorEmittersNew.edd) for each
// always-on entry (trigger 0, variant below 1) whose model point exists, when
// the actor is made; it stays with the actor. Here the dressed mesh carries the
// entries (assets::MeshEmitter) and each drawn actor keeps a persistent emitter
// per entry on its point, placed every frame from the pose the actor renderer
// drew. Density and opacity: eqclient.ini [Defaults] ActorParticleDensity /
// ActorParticleOpacity. Of the animation-started entries (trigger 1; their
// animation is an index into eqgame's animation table at 0xaae3d8), those of
// STND (31) and IDLE (32) run while the actor stands: the rune circles'
// (#TPTrigger*, T05) blue lights. The others (footfalls, swings, casts) start
// each time their animation starts (or loops round) on the actor, after the
// entry's delay (+0x54), and emit while the animation plays, at most the
// entry's duration: the animation's clip is the table row's old-style code
// (new_to_old_anim, old_anim_clip). How the original times them is not
// traced; this is the client's own reading.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "game/particles.h"
#include "game/spawn_heights.h"
#include "render/particle_renderer.h"

namespace hailstone { class GameDir; }
namespace hailstone::assets { struct CharacterMesh; }
namespace hailstone::assets { struct ObjectAnimation; }

namespace hailstone::app {

class ActorEmitters {
public:
    ActorEmitters(render::VkContext& vk, const GameDir& game, const std::filesystem::path& shader_dir,
                  bool scene = true);
    explicit ActorEmitters(std::vector<assets::EmitterDefinition> definitions, game::HeightTables heights = {});
    const game::ParticleSystem& simulation() const { return particles_; }
    const game::ParticleSystem& cloud_simulation() const { return clouds_; }
    // The loaded character/equipment archives supply WLD cloud sprites (outside the EDD effect folders).
    // `any_thread`: render::ParticleRenderer::set_texture_reader.
    void set_texture_reader(render::ParticleRenderer::TextureReader reader, bool any_thread = false);
    glm::vec3 texture_color(const std::string& name, bool cloud = false) const;

    // An actor drawn this frame and the mesh it was drawn with.
    struct Actor {
        std::uint32_t id = 0;
        const assets::CharacterMesh* mesh = nullptr;
        bool standing = false; // in its stand or idle animation (render::ActorRenderer::standing)
        float alpha = 1.0f;
    };
    // A bone's world matrix on an actor as drawn (false: not drawn).
    using BoneWorld = std::function<bool(std::uint32_t id, int bone, glm::mat4& world)>;
    // Whether the actor drawn last was playing the clip `code`, and how far in (render::ActorRenderer::drawn_playing).
    using Playing = std::function<bool(std::uint32_t id, std::string_view code, float& time)>;

    // Starts the emitters of actors that came or changed their mesh, stops those of actors gone, and advances them
    // by dt seconds for a viewer at `viewer`; then loads the textures they need (call outside a frame).
    void update(float dt, std::span<const Actor> actors, const BoneWorld& bone_world, const glm::vec3& viewer,
                const Playing& playing = {});
    // `modern`: render::ParticleRenderer::draw's.
    void draw(const render::FrameInfo& frame, const glm::mat4& view_proj, const glm::mat4& view, float glow = 1.0f,
              const render::SceneLighting* modern = nullptr);
    void clear();
    std::size_t emitters() const { return particles_.emitters().size() + clouds_.emitters().size(); }
    // ActorParticleDensity (the Options window's combo; the effect manager's +0xac): the actors' emitters are made
    // again with it. eqclient_density() is the ini's, read at the start.
    void set_density(float density);
    float density() const { return settings_.density; }
    float eqclient_density() const { return eqclient_density_; }

private:
    struct Live {
        // The mesh drawn last frame, and which one it is: a mesh freed and another dressed at its address (the
        // spawn dressed again) has another serial (assets::MeshSerial), so its emitters start afresh.
        const assets::CharacterMesh* mesh = nullptr;
        std::uint64_t mesh_serial = 0;
        std::vector<std::uint32_t> keys;
        std::vector<std::uint32_t> cloud_keys;
        double elapsed = 0.0; // the attached item's own animation controller starts with this instance/mesh
        struct CloudPose {
            std::uint64_t ms = ~std::uint64_t{0};
            std::vector<glm::mat4> motion;
        };
        std::unordered_map<const assets::ObjectAnimation*, CloudPose> cloud_poses;
        bool restart_edd = false; // ActorParticleDensity changes leave the separate WLD list running
        float alpha = 1.0f;
        std::vector<std::uint32_t> standing_keys; // the STND / IDLE entries', while it stands
        // The other animation-started entries, by entry: the time into their animation last frame (negative: not
        // playing), a start waiting for the entry's delay (seconds left, negative none) and the emitter running.
        struct Triggered {
            float last = -1.0f, pending = -1.0f, length = 0.0f;
            std::uint32_t key = 0;
        };
        std::unordered_map<std::size_t, Triggered> triggered;
        bool seen = false;
    };
    game::ParticleSystem particles_;
    game::ParticleSystem clouds_{std::vector<assets::EmitterDefinition>{assets::EmitterDefinition{}}};
    // Own the definitions as long as their registered indexes can be used. Cleared with the zone/preview subject.
    std::unordered_map<const assets::EmitterDefinition*, std::size_t> cloud_indexes_;
    std::vector<std::shared_ptr<const assets::EmitterDefinition>> cloud_definitions_;
    game::HeightTables heights_;
    game::SpellParticleSettings settings_;
    float eqclient_density_ = 1.0f;
    std::unique_ptr<render::ParticleRenderer> renderer_;
    std::unique_ptr<render::ParticleRenderer> cloud_renderer_;
    render::ParticleRenderer::TextureReader texture_reader_;
    bool texture_reader_any_thread_ = false;
    render::VkContext* vk_ = nullptr;
    const GameDir* game_ = nullptr;
    std::filesystem::path shader_dir_;
    bool scene_ = true;
    std::unordered_map<std::uint32_t, Live> live_;
    std::uint32_t next_key_ = 1;
};

} // namespace hailstone::app
