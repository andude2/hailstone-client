#include "app/spell_particles.h"

#include <algorithm>
#include <cstdlib>

#include "assets/character.h"
#include "core/game_dir.h"
#include "core/ini_file.h"
#include "core/log.h"
#include "game/spells.h"
#include "net/game_connection.h"
#include "render/actor_renderer.h"

namespace hailstone::app {

SpellParticles::SpellParticles(render::VkContext& vk, const GameDir& game, const std::filesystem::path& shader_dir) {
    try {
        if (auto data = game.read("spellsnew.edd")) particles_ = game::ParticleSystem(assets::parse_edd(*data));
        if (auto data = game.read("spellsnew.eff")) effects_ = assets::parse_spell_effects(*data);
    } catch (const std::exception& e) {
        log::warn("spell particles: {}", e.what());
    }
    IniFile ini;
    if (auto path = game.find("eqclient.ini")) ini.load(*path);
    auto value = [&](std::string_view key) -> std::optional<float> {
        auto v = ini.get("Defaults", key);
        if (!v || v->empty()) return std::nullopt;
        return std::strtof(v->c_str(), nullptr);
    };
    settings_ = game::spell_particle_settings(value("SpellParticleDensity"), value("SpellParticleOpacity"));
    eqclient_density_ = settings_.density;
    if (auto path = game.find("Resources/SPOffsets.ini")) sp_offsets_.load(*path);
    if (auto moddat = game.read("Resources/moddat.ini"))
        heights_ = game::parse_height_tables({}, std::string_view(reinterpret_cast<const char*>(moddat->data()),
                                                                  moddat->size()));
    spell_points_ = assets::SpellPoints([this](std::string_view section, std::string_view key) {
        std::optional<float> v;
        if (auto text = sp_offsets_.get(section, key)) v = std::strtof(text->c_str(), nullptr);
        return v;
    });
    if (!particles_.empty()) {
        renderer_ = std::make_unique<render::ParticleRenderer>(vk, game, shader_dir);
        log::info("spell particles: {} emitter definitions, {} effects, density {:.2f}, opacity {:.2f}",
                  particles_.definitions().size(), effects_.size(), settings_.density, settings_.opacity);
    }
}

std::optional<glm::mat4> SpellParticles::point_world(const render::ActorRenderer& actors, std::uint32_t id, int point) {
    const assets::CharacterModel* model = actors.drawn_model(id);
    if (!model || point < 0 || point >= 10) return std::nullopt;
    const assets::SpellPoints::Model& sp = spell_points_.of(*model);
    if (!((sp.mask >> point) & 1u)) return std::nullopt;
    glm::mat4 bone(1.0f);
    if (!actors.drawn_bone(id, sp.bone[static_cast<std::size_t>(point)], bone)) return std::nullopt;
    return bone * sp.local[static_cast<std::size_t>(point)];
}

void SpellParticles::update(net::GameConnection& conn, std::span<const net::SpellEffectCue> cues,
                            std::span<const net::EffectPacketCue> effect_packets, const render::ActorRenderer* actors,
                            float dt) {
    clock_ += dt;
    for (const auto& cue : cues) {
        Held h{clock_ + cue.delay, cue.stage, cue.caster, cue.target, cue.spell, cue.source, cue.duration_ms};
        if (cue.delay > 0.0) held_.push_back(h);
        else play(conn, h, actors);
    }
    const auto now_ms = static_cast<std::uint64_t>(clock_ * 1000.0);
    using Kind = net::EffectPacketCue::Kind;
    for (const auto& e : effect_packets) {
        log::info("effect packet {}: effect {} caster {} target {} ({} / {} / {} ms, full {})", static_cast<int>(e.kind),
                  e.effect, e.caster, e.target, e.stage1_ms, e.stage1_length_ms, e.stage2_length_ms, e.full);
        switch (e.kind) {
        case Kind::Add:
            timers_.add(e.caster, e.target, e.effect, e.stage1_ms, e.stage1_length_ms, e.stage2_length_ms, e.full,
                        now_ms);
            break;
        case Kind::Remove:
            // SpellEffectTimers_Remove: the emitters stop only when an entry was dropped. A nimbus (EQEmu sends
            // it as an OP_SpellEffect add) outlives its entry, so its removal is OP_RemoveNimbusEffect's.
            if (timers_.remove(e.caster, e.target, e.effect)) {
                particles_.stop(e.caster << 16 | (e.effect & 0xffffu));
                if (e.target != 0xffffffffu) particles_.stop(e.target << 16 | (e.effect & 0xffffu));
            }
            break;
        case Kind::AddNimbus:
            // Handle_OP_AddNimbusEffect @0x4bce70: the impact stage, the spawn as caster and target, duration 0.
            play_effect(conn, {e.caster, e.caster, e.effect, 2, 0, false}, actors);
            break;
        case Kind::RemoveNimbus:
            // Handle_OP_RemoveNimbusEffect @0x4b7a10.
            particles_.stop(e.caster << 16 | (e.effect & 0xffffu));
            break;
        }
    }
    timers_.update(now_ms);
    for (const auto& play : timers_.take()) play_effect(conn, play, actors);
    for (std::size_t i = 0; i < held_.size();) {
        if (held_[i].at > clock_) {
            ++i;
            continue;
        }
        Held h = held_[i];
        held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(i));
        play(conn, h, actors);
    }
    update_path(conn);
    const auto& world = conn.world();
    auto position = [&](std::uint32_t id) -> std::optional<glm::vec3> {
        auto it = world.spawns.find(id);
        if (it == world.spawns.end()) return std::nullopt;
        const auto& s = it->second;
        return s.display_valid ? s.display_position : s.position;
    };
    particles_.update(dt, [&](std::uint32_t id, std::int32_t pt) -> std::optional<game::EmitterAttachment> {
        auto it = world.spawns.find(id);
        if (it == world.spawns.end()) return std::nullopt;
        game::EmitterAttachment a;
        // The actor's root GetScaleFactor (its model scale) and GetParticleScaleFactor (moddat.ini PESFactor).
        if (it->second.heights.model_scale > 0.0f) a.actor_scale = it->second.heights.model_scale;
        if (actors) {
            if (const assets::CharacterModel* model = actors->spawn_model(id)) a.particle_scale = particle_scale(*model);
            if (pt >= 0)
                if (auto m = point_world(*actors, id, pt)) {
                    a.position = glm::vec3((*m)[3]);
                    a.axes = glm::mat3(*m);
                }
            glm::mat4 t;
            if (actors->drawn_transform(id, t)) a.actor_axes = glm::mat3(t);
        }
        // A point the model turned out not to have (asked for before it loaded): point 0, as the slot's own fallback
        // would have chosen. Without its model drawn, a pointed emitter keeps to the spawn's place.
        if (pt > 0 && !a.position && actors)
            if (auto m = point_world(*actors, id, 0)) {
                a.position = glm::vec3((*m)[3]);
                a.axes = glm::mat3(*m);
            }
        if (pt >= 0 && !a.position) a.position = position(id);
        return a;
    });
}

float SpellParticles::particle_scale(const assets::CharacterModel& model) const {
    return heights_.pes_factor(model.code);
}

namespace {
// The keys of the path's emitters (above any spawn id << 16 | spell id a spell's emitters use).
constexpr std::uint32_t kPathLineKey = 0xfff00000u;
constexpr std::uint32_t kPathFinishKey = 0xfff00001u;
std::uint32_t mover_key(std::uint32_t serial, int i) { return 0xfff10000u + ((serial & 0x3fffu) << 2) + static_cast<std::uint32_t>(i); }
} // namespace

void SpellParticles::update_path(net::GameConnection& conn) {
    if (particles_.empty()) return;
    const game::PlayerPath& path = conn.player_path();
    const game::PathSettings& ps = path.settings();
    // The line: soft glowing quads along PlayerPath's spline points (and half way between them), their alpha as
    // PlayerPath fades the line near and far from the player. This client's look: the original draws a textured tube
    // (ThickLineSystem with Path.ini's Texture and ShapeSet); a soft blob (spellsnew's blobb.dds, a finish emitter's
    // texture) reads better as camera-facing quads than the tube's stripe texture.
    if (!path_definition_) {
        assets::EmitterDefinition d;
        d.name = "PlayerPath";
        d.texture = "blobb.dds";
        d.additive = 1;
        d.no_zwrite = 1;
        d.frames = 1;
        path_definition_ = particles_.add_definition(std::move(d));
    }
    std::vector<game::Particle> quads;
    if (path.active()) {
        const auto& line = path.line();
        auto quad = [&](const glm::vec3& at, float scale, float alpha) {
            if (alpha <= 0.0f) return;
            game::Particle q;
            q.position = at;
            q.width = q.height = 2.5f * scale;
            q.color = glm::vec4(0.75f, 0.9f, 1.0f, alpha);
            q.lifetime = 1.0f;
            quads.push_back(q);
        };
        for (std::size_t i = 0; i < line.size(); ++i) {
            quad(line[i].position, line[i].scale, line[i].alpha);
            if (i + 1 < line.size())
                quad((line[i].position + line[i + 1].position) * 0.5f, (line[i].scale + line[i + 1].scale) * 0.5f,
                     std::min(line[i].alpha, line[i + 1].alpha));
        }
    }
    if (path_definition_) {
        if (quads.empty()) particles_.stop(kPathLineKey);
        else particles_.set_still(kPathLineKey, *path_definition_, std::move(quads));
    }
    // The movers: their four EmitterN emitters made where a mover is first placed, moved along with it, and taken away
    // when it is off the path or gone (a reused mover has a new serial).
    std::vector<std::uint32_t> live;
    if (path.active())
        for (const auto& m : path.movers()) {
            auto at = path.mover_position(m);
            if (!at) continue;
            live.push_back(m.serial);
            for (int i = 0; i < 4; ++i) {
                const auto& es = ps.emitters[static_cast<std::size_t>(i)];
                if (es.definition < 1) continue;
                std::uint32_t key = mover_key(m.serial, i);
                if (particles_.has(key)) {
                    particles_.move(key, *at);
                    continue;
                }
                game::SpellEmitterRequest r;
                r.emitter = es.definition;
                r.key = key;
                r.duration_ms = es.life_span;
                r.density = es.density * settings_.density;
                r.opacity = es.opacity * settings_.opacity;
                r.position = *at;
                r.persistent = true;
                particles_.create(r);
            }
        }
    for (std::uint32_t serial : path_movers_)
        if (std::find(live.begin(), live.end(), serial) == live.end())
            for (int i = 0; i < 4; ++i) particles_.stop(mover_key(serial, i));
    path_movers_ = std::move(live);
    // PlayerPath_OnDestinationReached: the FinishEmitterN at the destination for their LifeSpan.
    if (auto at = conn.take_path_finish())
        for (const auto& es : ps.finish_emitters) {
            if (es.definition < 1) continue;
            game::SpellEmitterRequest r;
            r.emitter = es.definition;
            r.key = kPathFinishKey;
            r.duration_ms = es.life_span;
            r.density = es.density * settings_.density;
            r.opacity = es.opacity * settings_.opacity;
            r.position = *at;
            particles_.create(r);
        }
}

std::optional<game::StageActor> SpellParticles::stage_actor(const net::GameConnection& conn, std::uint32_t id,
                                                            const render::ActorRenderer* actors) {
    const auto& world = conn.world();
    auto it = world.spawns.find(id);
    if (it == world.spawns.end()) return std::nullopt;
    const auto& s = it->second;
    game::StageActor a;
    a.id = id;
    a.position = s.display_valid ? s.display_position : s.position;
    a.level = s.level > 0 ? s.level : 1;
    a.height = s.heights.height; // Height (+0x13c): the size, or racedata's default
    // The points are the model's, drawn or not: a spawn not drawn this frame (the player in first person, one out of
    // view) still has them in the original, and an emitter made without a point would stay where it was made.
    a.model_known = false;
    if (actors)
        if (const assets::CharacterModel* model = actors->spawn_model(id)) {
            a.model_known = true;
            a.points = spell_points_.of(*model).mask;
            if (auto chest = point_world(*actors, id, 1)) a.chest = glm::vec3((*chest)[3]);
            a.particle_scale = particle_scale(*model);
        }
    if (floor_) {
        // Attach 12's z: GetPlayerFloorHeight at the chest's x and y (the spawn's without a chest), from 0.1 above
        // the spawn.
        const glm::vec3 at = a.points & 2u ? glm::vec3(a.chest.x, a.chest.y, a.position.z) : a.position;
        a.floor = floor_(at + glm::vec3(0.0f, 0.0f, 0.1f));
    }
    return a;
}

float SpellParticles::source_density_for(const net::GameConnection& conn, std::uint32_t cast_by,
                                         std::uint32_t struck) const {
    const auto& world = conn.world();
    auto is_npc = [&](std::uint32_t id) -> std::optional<bool> {
        auto it = world.spawns.find(id);
        if (id == 0 || it == world.spawns.end()) return std::nullopt;
        return it->second.is_npc;
    };
    return source_density(game::spell_fx_source(cast_by, struck, world.player_id, is_npc(cast_by), is_npc(struck)));
}

void SpellParticles::play(net::GameConnection& conn, const Held& cue, const render::ActorRenderer* actors) {
    if (particles_.empty()) return;
    const game::SpellData* sp = conn.spell_table().get(cue.spell);
    if (!sp || sp->spell_anim < 1 || static_cast<std::size_t>(sp->spell_anim) >= effects_.size()) return;
    // Hailstone: the Options window's density for whose spell it is (the impact stage's caster is the cue's source).
    const std::uint32_t cast_by = cue.stage == 2 ? cue.source : cue.caster;
    game::SpellParticleSettings settings = settings_;
    settings.density *= source_density_for(conn, cast_by, cue.target != 0 ? cue.target : cue.caster);
    if (settings.density <= 0.0f) return;
    auto caster = stage_actor(conn, cue.caster, actors);
    auto target = stage_actor(conn, cue.target, actors);
    glm::vec3 point = caster ? caster->position : target ? target->position : glm::vec3(0.0f);
    std::int32_t longest = 0;
    for (const auto& r : game::spawn_stage_emitters(effects_[static_cast<std::size_t>(sp->spell_anim)], cue.stage,
                                                     cue.spell, caster ? &*caster : nullptr,
                                                     target ? &*target : nullptr, point, cue.duration_ms, settings))
        longest = std::max(longest, particles_.create(r));
    log::debug("spell {} stage {}: effect {} ({} ms)", cue.spell, cue.stage, sp->spell_anim, longest);
}

void SpellParticles::play_effect(net::GameConnection& conn, const game::SpellEffectStagePlay& play,
                                 const render::ActorRenderer* actors) {
    // SpellEffectTimers_PlayStage / Handle_OP_AddNimbusEffect: an effect in 1..count-1 on a caster that exists, at
    // the caster's position; the target may be missing.
    if (particles_.empty() || play.effect < 1 || play.effect >= effects_.size()) return;
    auto caster = stage_actor(conn, play.caster, actors);
    if (!caster) return;
    auto target = stage_actor(conn, play.target, actors);
    // The packet's flag puts the emitters at density and opacity 1 (SpawnStageEmitters' `full`), past
    // SpellParticleDensity. Hailstone: the Options window's per-source density still scales them, and Off hides them.
    game::SpellParticleSettings settings = settings_;
    const float source = source_density_for(conn, play.caster, play.target != 0 ? play.target : play.caster);
    if (play.full) settings = {source, 1.0f};
    else settings.density *= source;
    if (settings.density <= 0.0f) return;
    std::int32_t longest = 0;
    for (const auto& r : game::spawn_stage_emitters(effects_[play.effect], play.stage, play.effect, &*caster,
                                                     target ? &*target : nullptr, caster->position, play.duration_ms,
                                                     settings))
        longest = std::max(longest, particles_.create(r));
    log::debug("effect {} stage {} on {}: {} ms", play.effect, play.stage, play.caster, longest);
}

void SpellParticles::prepare() {
    if (renderer_) renderer_->prepare(particles_);
}

void SpellParticles::draw(const render::FrameInfo& frame, const glm::mat4& view_proj, const glm::mat4& view,
                          float glow, const render::SceneLighting* modern) {
    if (renderer_) renderer_->draw(frame, view_proj, view, particles_, glow, {}, modern);
}

void SpellParticles::clear() {
    particles_.clear();
    held_.clear();
    timers_.clear();
    path_movers_.clear();
}

std::vector<game::LightSource> SpellParticles::lights(std::size_t max) const {
    return game::particle_lights(
        particles_, [this](const std::string& t) { return renderer_ ? renderer_->texture_color(t) : glm::vec3(1.0f); },
        max);
}

} // namespace hailstone::app
