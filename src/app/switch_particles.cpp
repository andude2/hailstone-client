#include "app/switch_particles.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <optional>
#include <system_error>

#include "assets/legacy_particles.h"
#include "assets/pfs.h"
#include "assets/wld_particles.h"
#include "core/game_dir.h"
#include "core/log.h"

namespace hailstone::app {

namespace {
constexpr std::int32_t kSwitchDurationMs = 43'200'000; // ProcessSwitches, 0x48d530: twelve hours
constexpr std::uint32_t key(std::uint8_t id) { return 1u + id; }
bool positioned(const game::Door& d) {
    const auto p = d.world_position();
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

// The resolved effects of spells.eff with gequip.s3d's sprites; empty when nothing resolves.
std::vector<SwitchParticles::Effect> read_effects(const GameDir& game, const std::filesystem::path& s3d_path) {
    const auto data = game.read("spells.eff");
    if (!data) return {};
    const auto archive = assets::PfsArchive::open(s3d_path, assets::PfsArchive::storage_for(game, s3d_path),
                                                  game.shared_file_cache());
    const auto bytes = archive.read("gequip.wld");
    if (!bytes) return {};
    const auto wld = assets::wld::WldFile::parse(*bytes, "gequip.s3d:gequip.wld");
    const auto parsed = assets::parse_legacy_switch_effects(*data);
    std::vector<SwitchParticles::Effect> effects(parsed.size());
    bool any = false;
    for (std::size_t i = 1; i < parsed.size(); ++i)
        for (std::size_t slot = 0; slot < parsed[i].size(); ++slot) {
            if (!parsed[i][slot]) continue;
            const auto& source = *parsed[i][slot];
            const auto* fragment = wld.find(source.sprite);
            if (!fragment) continue;
            const auto ref = static_cast<assets::wld::Ref>(fragment - wld.fragments().data() + 1);
            const auto texture = assets::particle_texture(wld, ref);
            if (!texture) continue;
            // A missing sprite is skipped rather than drawn as an opaque fallback quad.
            if (std::any_of(texture->files.begin(), texture->files.end(),
                            [&](const std::string& name) { return !archive.contains(name); })) continue;
            effects[i][slot] = assets::legacy_switch_definition(source, *texture);
            any = any || effects[i][slot].has_value();
        }
    if (!any) effects.clear();
    return effects;
}

struct Stamp {
    std::filesystem::path path;
    std::uintmax_t size = 0;
    std::filesystem::file_time_type time{};
    bool operator==(const Stamp&) const = default;
};

std::optional<Stamp> stamp(const std::filesystem::path& path) {
    std::error_code size_error, time_error;
    const auto size = std::filesystem::file_size(path, size_error);
    const auto time = std::filesystem::last_write_time(path, time_error);
    if (size_error || time_error) return std::nullopt;
    return Stamp{path, size, time};
}

// Read again only when either file found is another or has changed: every zone-in makes SwitchParticles again, and
// this parses the whole of gequip.wld. The archive itself is not kept (an overlay's may be replaced meanwhile).
std::vector<SwitchParticles::Effect> load_effects(const GameDir& game) {
    struct Cached {
        Stamp eff, s3d;
        std::vector<SwitchParticles::Effect> effects;
    };
    static std::optional<Cached> cache;
    const auto eff_path = game.find("spells.eff");
    const auto s3d_path = game.find("gequip.s3d");
    if (!eff_path || !s3d_path) return {};
    const auto eff = stamp(*eff_path), s3d = stamp(*s3d_path);
    if (cache && eff && s3d && cache->eff == *eff && cache->s3d == *s3d) return cache->effects;
    std::vector<SwitchParticles::Effect> effects;
    try {
        effects = read_effects(game, *s3d_path);
    } catch (const std::exception& e) {
        log::warn("switch particles: {}", e.what());
    }
    if (eff && s3d) cache = Cached{*eff, *s3d, effects};
    return effects;
}
} // namespace

SwitchParticles::SwitchParticles(std::vector<Effect> effects) { set_effects(std::move(effects)); }

void SwitchParticles::set_effects(std::vector<Effect> effects) {
    clear();
    particles_ = game::ParticleSystem(std::vector<assets::EmitterDefinition>{assets::EmitterDefinition{}});
    effects_.assign(std::min<std::size_t>(effects.size(), 256), {});
    for (std::size_t i = 1; i < effects_.size(); ++i)
        for (std::size_t slot = 0; slot < effects[i].size(); ++slot)
            if (effects[i][slot]) effects_[i][slot] = particles_.add_definition(std::move(*effects[i][slot]));
}

SwitchParticles::SwitchParticles(const GameDir& game) {
    auto effects = load_effects(game);
    if (effects.empty()) return;
    try {
        const auto path = game.find("gequip.s3d");
        if (!path) return;
        auto archive = std::make_shared<assets::PfsArchive>(assets::PfsArchive::open(
            *path, assets::PfsArchive::storage_for(game, *path), game.shared_file_cache()));
        set_effects(std::move(effects));
        if (particles_.definitions().size() <= 1) return;
        archive_ = std::move(archive);
    } catch (const std::exception& e) {
        log::warn("switch particles: {}", e.what());
    }
}

SwitchParticles::SwitchParticles(render::VkContext& vk, const GameDir& game,
                                 const std::filesystem::path& shader_dir)
    : SwitchParticles(game) {
    vk_ = &vk;
    game_ = &game;
    shader_dir_ = shader_dir;
}

bool SwitchParticles::start(const game::Door& door, std::int32_t effect, std::uint8_t level) {
    if (door.type != 0x35 || density_ <= 0.0f || effect < 1 || static_cast<std::size_t>(effect) >= effects_.size() ||
        !positioned(door)) return false;
    const auto& definitions = effects_[static_cast<std::size_t>(effect)];
    if (std::all_of(definitions.begin(), definitions.end(), [](std::size_t index) { return index == 0; })) return false;
    auto& live = live_[door.id];
    if (live && live->effect == effect && live->position == door.world_position() && particles_.has(key(door.id)))
        return true;
    stop(door);
    for (std::size_t slot = 0; slot < definitions.size(); ++slot) {
        // DoParticleEffect's stage-2 first slot is available at every level;
        // its second and third slots require levels 24 and 39 respectively.
        if ((slot == 1 && level < 24) || (slot == 2 && level < 39)) continue;
        const auto index = definitions[slot];
        if (index == 0) continue;
        game::SpellEmitterRequest request;
        request.emitter = static_cast<std::int32_t>(index);
        request.key = key(door.id);
        request.position = door.world_position();
        request.duration_ms = kSwitchDurationMs;
        request.persistent = true;
        request.density = density_;
        // The native legacy list has no spell-stage density or opacity multiplier.
        particles_.create(request);
    }
    if (!particles_.has(key(door.id))) return false;
    live = Live{effect, door.world_position()};
    return true;
}

void SwitchParticles::stop(const game::Door& door) {
    particles_.stop(key(door.id));
    live_[door.id].reset();
}

bool SwitchParticles::active(const game::Door& door) const {
    const auto& live = live_[door.id];
    return live && live->effect == door.param && live->position == door.world_position() &&
           particles_.has(key(door.id));
}

void SwitchParticles::reconcile(std::span<const game::Door> doors) {
    std::array<bool, 256> seen{};
    for (const auto& door : doors) {
        const auto& live = live_[door.id];
        if (live && door.type == 0x35 && door.state != game::kSwitchClosed &&
            live->effect == door.param && live->position == door.world_position()) seen[door.id] = true;
    }
    for (std::size_t id = 0; id < live_.size(); ++id)
        if (live_[id] && !seen[id]) {
            particles_.stop(key(static_cast<std::uint8_t>(id)));
            live_[id].reset();
        }
}

void SwitchParticles::clear() {
    particles_.clear();
    live_.fill(std::nullopt);
}

void SwitchParticles::update(float dt, const glm::vec3& viewer) {
    particles_.set_viewer(viewer);
    particles_.update(dt, {});
    // Most zones have no particle switches. Allocate the GPU buffers only once
    // an effect actually starts, outside the network callback and draw frame.
    if (!renderer_ && vk_ && !particles_.emitters().empty()) {
        renderer_ = std::make_unique<render::ParticleRenderer>(*vk_, *game_, shader_dir_);
        renderer_->set_texture_reader([archive = archive_](const std::string& name) { return archive->read(name); });
        renderer_->set_rows_bottom_up(true); // the legacy WLD cloud collection
    }
    if (renderer_) renderer_->prepare(particles_);
}

void SwitchParticles::set_density(float density) {
    density_ = std::isfinite(density) ? std::clamp(density, 0.0f, 1.0f) : 0.0f;
    particles_.set_persistent_density(density_);
}

void SwitchParticles::draw(const render::FrameInfo& frame, const glm::mat4& view_proj, const glm::mat4& view,
                           float glow, const render::SceneLighting* modern) {
    if (renderer_) renderer_->draw(frame, view_proj, view, particles_, glow, {}, modern);
}

} // namespace hailstone::app
