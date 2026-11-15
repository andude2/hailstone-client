#include "app/gamepad.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "core/config.h"

namespace hailstone::app {

const char* pad_mode_name(PadMode m) {
    switch (m) {
    case PadMode::Menu: return "menu";
    case PadMode::Cursor: return "cursor";
    default: return "game";
    }
}

std::optional<PadMode> pad_mode_by_name(std::string_view name) {
    if (name == "game") return PadMode::Game;
    if (name == "menu") return PadMode::Menu;
    if (name == "cursor") return PadMode::Cursor;
    return std::nullopt;
}

Gamepad::Gamepad(Config& config, std::filesystem::path path) : config_(config), path_(std::move(path)) { reload(); }

void Gamepad::reload() {
    settings_.load(config_);
    bindings_.load(config_);
}

int Gamepad::cross_set() const { return active() && mode_ == PadMode::Game ? game::cross_set(*state_) : 0; }

bool Gamepad::set_setting(std::string_view name, float value) {
    if (!settings_.set(name, value)) return false;
    save();
    return true;
}

std::optional<std::string> Gamepad::binding_text(std::string_view input) const {
    auto in = game::parse_pad_input(input);
    if (!in) return std::nullopt;
    if (auto a = bindings_.binding(*in)) return game::pad_action_text(*a);
    if (in->chord & (game::kChordLT | game::kChordRT)) return std::string();
    return game::pad_action_text(game::default_pad_action(*in));
}

bool Gamepad::set_binding_text(std::string_view input, std::string_view action) {
    auto in = game::parse_pad_input(input);
    if (!in) return false;
    if (action.empty()) {
        bindings_.set_binding(*in, std::nullopt);
    } else {
        auto a = game::parse_pad_action(action);
        if (!a) return false;
        bindings_.set_binding(*in, *a);
    }
    save();
    return true;
}

bool Gamepad::set_cross_slot(int set, int slot, int bar, int button) {
    if (!bindings_.set_cross(set, slot, {bar, button})) return false;
    save();
    return true;
}

void Gamepad::request_focus(std::string window) {
    mode_ = PadMode::Menu;
    std::erase(focus_requests_, window);
    focus_requests_.push_back(std::move(window));
}

std::vector<std::string> Gamepad::take_focus_requests() { return std::exchange(focus_requests_, {}); }

bool Gamepad::rumble(float low, float high, int ms) {
    if (!settings_.rumble || !rumble_) return false;
    if (!std::isfinite(low) || !std::isfinite(high)) return false;
    return rumble_(std::clamp(low, 0.0f, 1.0f), std::clamp(high, 0.0f, 1.0f), std::clamp(ms, 0, kMaxRumbleMs));
}

void Gamepad::save() {
    settings_.save(config_);
    bindings_.save(config_);
    if (!path_.empty()) config_.save(path_);
}

} // namespace hailstone::app
