#include "app/actor_emitters.h"

#include <algorithm>
#include <cstdlib>
#include <optional>

#include "assets/character.h"
#include "core/game_dir.h"
#include "core/ini_file.h"
#include "core/log.h"
#include "game/mechanics/anim_select.h"

namespace hailstone::app {

namespace {
// eqgame's animation table (0xaae3d8): the standing and idle animations.
constexpr std::int32_t kStnd = 31, kIdle = 32;

// The clip an entry's animation (a row of that table) plays: the row's old-style code.
std::string_view triggered_clip(std::int32_t animation) {
    if (animation < 1 || animation > 0xb6 - 0x4f) return {};
    namespace m = game::mechanics;
    return m::old_anim_clip(m::new_to_old_anim(static_cast<std::uint8_t>(0x4f + animation)));
}
} // namespace

ActorEmitters::ActorEmitters(render::VkContext& vk, const GameDir& game, const std::filesystem::path& shader_dir) {
    try {
        if (auto data = game.read("actoremittersnew.edd")) particles_ = game::ParticleSystem(assets::parse_edd(*data));
    } catch (const std::exception& e) {
        log::warn("actor emitters: {}", e.what());
    }
    IniFile ini;
    if (auto path = game.find("eqclient.ini")) ini.load(*path);
    auto value = [&](std::string_view key) -> std::optional<float> {
        auto v = ini.get("Defaults", key);
        if (!v || v->empty()) return std::nullopt;
        return std::strtof(v->c_str(), nullptr);
    };
    settings_ = game::spell_particle_settings(value("ActorParticleDensity"), value("ActorParticleOpacity"));
    eqclient_density_ = settings_.density;
    if (!particles_.empty()) renderer_ = std::make_unique<render::ParticleRenderer>(vk, game, shader_dir);
}


void ActorEmitters::update(float dt, std::span<const Actor> actors, const BoneWorld& bone_world,
                           const glm::vec3& viewer, const Playing& playing) {
    if (particles_.empty()) return;
    for (auto& [id, live] : live_) live.seen = false;
    // One persistent emitter for a mesh's entry `i` on the actor.
    auto start = [&](const Actor& a, std::size_t i, std::int32_t duration_ms = -1) -> std::optional<std::uint32_t> {
        const auto& me = a.mesh->emitters[i];
        glm::mat4 bone;
        if (!bone_world(a.id, me.bone, bone)) return std::nullopt;
        game::SpellEmitterRequest r;
        r.emitter = static_cast<std::int32_t>(me.emitter.emitter);
        r.key = next_key_++;
        // CActor::vf126 @0x1003a950 creates an actor's emitter for 5,000,000 ms (past 4,000 s the DLL keeps it
        // going), at the actor density and opacity, scale 1; the entry's own +0x58 is not passed. It sets the
        // particle capacity and what a lifetime below 0 lasts; the emitter itself lasts as long as the actor.
        r.duration_ms = duration_ms >= 0 ? duration_ms : game::kActorEmitterDurationMs;
        r.density = settings_.density;
        r.opacity = settings_.opacity;
        r.position = glm::vec3((bone * me.local)[3]);
        r.actor = a.id;
        r.bone_point = static_cast<std::int32_t>(i);
        r.point_only = true;
        r.persistent = duration_ms < 0;
        particles_.create(r);
        return r.key;
    };
    for (const Actor& a : actors) {
        if (!a.mesh || a.mesh->emitters.empty()) continue;
        Live& live = live_[a.id];
        live.seen = true;
        live.mesh = a.mesh; // (the same mesh, or one at the same address: the attachments below read this one)
        if (live.mesh_serial != a.mesh->serial.value()) {
            // A new mesh (dressed again, another weapon): its emitters replace the old ones.
            for (std::uint32_t key : live.keys) particles_.stop(key);
            for (std::uint32_t key : live.standing_keys) particles_.stop(key);
            for (auto& [i, t] : live.triggered)
                if (t.key) particles_.stop(t.key);
            live.keys.clear();
            live.standing_keys.clear();
            live.triggered.clear();
            live.mesh_serial = a.mesh->serial.value();
            for (std::size_t i = 0; i < a.mesh->emitters.size(); ++i) {
                const auto& e = a.mesh->emitters[i].emitter;
                if (e.trigger != 0 || e.variant >= 1) continue;
                if (auto key = start(a, i)) live.keys.push_back(*key);
            }
        }
        // The standing entries start when it stands and stop when it moves, casts or acts.
        if (a.standing && live.standing_keys.empty()) {
            for (std::size_t i = 0; i < a.mesh->emitters.size(); ++i) {
                const auto& e = a.mesh->emitters[i].emitter;
                if (e.trigger != 1 || e.variant >= 1 || (e.animation != kStnd && e.animation != kIdle)) continue;
                if (auto key = start(a, i)) live.standing_keys.push_back(*key);
            }
        } else if (!a.standing && !live.standing_keys.empty()) {
            for (std::uint32_t key : live.standing_keys) particles_.stop(key);
            live.standing_keys.clear();
        }
        // The other animation-started entries: each start (or loop) of the animation starts one after its delay.
        if (playing)
            for (std::size_t i = 0; i < a.mesh->emitters.size(); ++i) {
                const auto& e = a.mesh->emitters[i].emitter;
                if (e.trigger != 1 || e.variant >= 1 || e.animation == kStnd || e.animation == kIdle) continue;
                const std::string_view code = triggered_clip(e.animation);
                if (code.empty()) continue;
                auto& t = live.triggered[i];
                float time = 0.0f;
                const bool on = playing(a.id, code, time);
                if (on && (t.last < 0.0f || time + 1e-4f < t.last)) {
                    t.pending = static_cast<float>(std::max(e.delay_ms, 0)) * 0.001f;
                    t.length = 0.0f;
                }
                if (on) t.length = std::max(t.length, time);
                t.last = on ? time : -1.0f;
                if (t.pending >= 0.0f) {
                    t.pending -= dt;
                    if (t.pending < 0.0f) {
                        if (t.key) particles_.stop(t.key);
                        // Emitting for about one play of the animation (a swing, a step), at most the entry's own.
                        std::int32_t ms = e.duration_ms > 0 ? std::min(e.duration_ms, 1500) : 1000;
                        t.key = start(a, i, ms).value_or(0u);
                        log::debug("actor emitters: {} starts entry {} (emitter {}, clip {}): key {}", a.id, i,
                                   e.emitter, code, t.key);
                    }
                }
            }
    }
    for (auto it = live_.begin(); it != live_.end();) {
        if (it->second.seen) {
            ++it;
            continue;
        }
        for (std::uint32_t key : it->second.keys) particles_.stop(key);
        for (std::uint32_t key : it->second.standing_keys) particles_.stop(key);
        for (auto& [i, t] : it->second.triggered)
            if (t.key) particles_.stop(t.key);
        it = live_.erase(it);
    }
    particles_.set_viewer(viewer);
    particles_.update(dt, [&](std::uint32_t id, std::int32_t i) -> std::optional<game::EmitterAttachment> {
        auto it = live_.find(id);
        if (it == live_.end() || !it->second.mesh || i < 0 ||
            static_cast<std::size_t>(i) >= it->second.mesh->emitters.size())
            return std::nullopt;
        const auto& me = it->second.mesh->emitters[static_cast<std::size_t>(i)];
        glm::mat4 bone;
        if (!bone_world(id, me.bone, bone)) return std::nullopt;
        glm::mat4 m = bone * me.local;
        game::EmitterAttachment at;
        at.position = glm::vec3(m[3]);
        at.axes = glm::mat3(m);
        return at;
    });
    if (renderer_) renderer_->prepare(particles_);
}

void ActorEmitters::draw(const render::FrameInfo& frame, const glm::mat4& view_proj, const glm::mat4& view,
                         float glow, const render::SceneLighting* modern) {
    if (renderer_) renderer_->draw(frame, view_proj, view, particles_, glow, {}, modern);
}

void ActorEmitters::set_density(float density) {
    density = std::clamp(density, 0.0f, 1.0f);
    if (density == settings_.density) return;
    settings_.density = density;
    clear(); // made again at the next update (one at density 0 was never made)
}

void ActorEmitters::clear() {
    particles_.clear();
    live_.clear();
}

} // namespace hailstone::app
