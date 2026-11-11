// ClientApp: the 3D target indicator, the ring at the current target's feet (game/target_indicator.h).

#include "app/client_app_internal.h"

#include "core/ini_file.h"
#include "game/coords.h"

namespace hailstone::app::client_internal {

bool ClientApp::target_ring_shown() const {
    if (!conn_ || !conn_->client_option(game::ClientOption::ShowTargetRing)) return false;
    const auto& world = conn_->world();
    return world.target_id && world.spawns.contains(world.target_id);
}

void ClientApp::draw_target_ring(const render::FrameInfo& frame, const glm::mat4& view_proj, float far_plane,
                                 bool modern, double seconds) {
    const double dt = ring_last_seconds_ >= 0.0 ? std::clamp(seconds - ring_last_seconds_, 0.0, 0.25) : 0.0;
    ring_last_seconds_ = seconds;
    // TargetIndicator_Update @0x5c5650 → TargetIndicator_UpdateTargetRing @0x5c5250: hidden without a target or
    // with the indicator off (Show3dTargetIndicator, TargetIndicator +0); otherwise the section of the target's
    // con, hidden for con 0.
    auto hide = [this] { ring_target_ = 0; };
    if (!target_ring_shown()) return hide();
    const game::Spawn& s = conn_->world().spawns.at(conn_->world().target_id);
    auto section = game::target_ring_section(game::con_level(s, conn_->player_status().level, conn_->pvp_rules()));
    if (!section) return hide();
    // Hailstone: the player's colour and opacity over the ini's (Options > Display "Target Ring").
    const game::TargetRingStyle style =
        game::apply_ring_colors(target_indicator().styles[static_cast<std::size_t>(*section)], *section, ring_colors_);

    bool has_actor = actor_renderer_ && actor_renderer_->model_for(s);
    float radius = game::target_ring_radius(s.heights.avatar_height, s.heights.model_scale, has_actor);
    // Hailstone, a deliberate difference: the player's Max Size, off by default (game::cap_ring_radius).
    radius = game::cap_ring_radius(radius, ring_max_size_);
    glm::vec3 base = s.display_valid ? s.display_position : s.position;
    glm::vec3 feet = base - glm::vec3(0, 0, s.heights.avatar_height);

    // Drawn this client's way (render/target_ring_renderer.h); the look is Hailstone's (game::target_ring_look).
    if (s.id != ring_target_) {
        ring_target_ = s.id;
        ring_target_since_ = seconds;
    }
    const bool attacking = conn_->auto_attack();
    ring_spin_ = static_cast<float>(std::fmod(
        ring_spin_ + game::target_ring_spin_rate(attacking, ring_cues_) * dt, static_cast<double>(glm::two_pi<float>())));
    game::TargetRingLook look = game::target_ring_look(radius, seconds, seconds - ring_target_since_, attacking, ring_cues_);

    if (!target_ring_renderer_) target_ring_renderer_ = std::make_unique<render::TargetRingRenderer>(vk_, shader_dir_);

    render::TargetRing ring;
    ring.feet = feet;
    ring.radius = radius;
    ring.color = glm::vec4(style.red, style.green, style.blue, style.alpha) / 255.0f;
    ring.width = look.width;
    ring.spin = ring_spin_;
    ring.facing = game::heading_to_yaw(s.display_valid ? s.display_heading : s.heading);
    ring.pulse = look.pulse;
    ring.flash = look.flash;
    ring.xray = ring_xray_ ? 0.2f : 0.0f;
    ring.glow = game::target_ring_core_boost(ring_colors_.glow);
    render::TargetRingView view;
    view.view_proj = view_proj;
    view.camera = camera_.position;
    view.forward = camera_.forward();
    view.near_plane = camera_.near_plane;
    view.far_plane = far_plane;
    view.haze = modern ? frame_lighting_.haze : glm::vec4(0.0f);
    view.seconds = seconds;
    target_ring_renderer_->draw(frame, view, ring);
}

const game::TargetIndicatorSettings& ClientApp::target_indicator() const {
    if (!target_indicator_) {
        // TargetIndicator_LoadIni on the first update: the skin's TargetIndicator.ini; this client's skin is its
        // Lua UI, so the default UI's.
        IniFile ini;
        if (auto bytes = game_.read("uifiles/default/TargetIndicator.ini"))
            ini.parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
        target_indicator_ = game::TargetIndicatorSettings::parse(ini);
    }
    return *target_indicator_;
}

namespace {
std::string ring_color_key(int section) {
    return std::format("display.target_ring_color.{}", game::kTargetIndicatorSections[static_cast<std::size_t>(section)]);
}
// target_ring_color_<n>, n a con section (0 Trivial .. 6 Deadly).
std::optional<int> ring_color_option(std::string_view name) {
    constexpr std::string_view prefix = "target_ring_color_";
    if (!name.starts_with(prefix) || name.size() != prefix.size() + 1) return std::nullopt;
    int n = name.back() - '0';
    if (n < 0 || n >= game::kTargetRingConSections) return std::nullopt;
    return n;
}
} // namespace

void ClientApp::load_ring_colors() {
    for (int i = 0; i < game::kTargetRingConSections; ++i)
        ring_colors_.rgb[static_cast<std::size_t>(i)] = game::parse_ring_rgb(config_.get_string(ring_color_key(i), ""));
    ring_colors_.opacity = std::clamp(config_.get_int("display.target_ring_opacity", game::kTargetRingOpacityDefault),
                                      game::kTargetRingOpacityMin, game::kTargetRingOpacityMax);
    ring_colors_.glow = std::clamp(config_.get_int("display.target_ring_glow", game::kTargetRingGlowDefault), 0, 100);
    ring_max_size_ = game::clamp_ring_max_size(config_.get_int("display.target_ring_max_size", game::kTargetRingMaxSizeOff));
}

// The Options window's Target Ring rows: a section's colour (0xRRGGBB, the player's or the ini's; set -1 for the
// ini's again), the opacity, the glow and the max size (0 off).
std::optional<int> ClientApp::ring_option(std::string_view name) const {
    if (auto n = ring_color_option(name)) {
        auto i = static_cast<std::size_t>(*n);
        if (ring_colors_.rgb[i]) return static_cast<int>(*ring_colors_.rgb[i]);
        const game::TargetRingStyle& s = target_indicator().styles[i];
        return (s.red << 16) | (s.green << 8) | s.blue;
    }
    if (name == "target_ring_opacity") return ring_colors_.opacity;
    if (name == "target_ring_glow") return ring_colors_.glow;
    if (name == "target_ring_max_size") return ring_max_size_;
    return std::nullopt;
}

bool ClientApp::set_ring_option(std::string_view name, int value) {
    if (auto n = ring_color_option(name)) {
        auto i = static_cast<std::size_t>(*n);
        if (value < 0) {
            ring_colors_.rgb[i].reset();
            config_.erase(ring_color_key(*n));
        } else {
            ring_colors_.rgb[i] = static_cast<std::uint32_t>(value) & 0xffffffu;
            config_.set(ring_color_key(*n), game::format_ring_rgb(*ring_colors_.rgb[i]));
        }
    } else if (name == "target_ring_opacity") {
        ring_colors_.opacity = std::clamp(value, game::kTargetRingOpacityMin, game::kTargetRingOpacityMax);
        config_.set_int("display.target_ring_opacity", ring_colors_.opacity);
    } else if (name == "target_ring_glow") {
        ring_colors_.glow = std::clamp(value, 0, 100);
        config_.set_int("display.target_ring_glow", ring_colors_.glow);
    } else if (name == "target_ring_max_size") {
        ring_max_size_ = game::clamp_ring_max_size(value);
        config_.set_int("display.target_ring_max_size", ring_max_size_);
    } else {
        return false;
    }
    config_.save(config_path_);
    return true;
}

} // namespace hailstone::app::client_internal
