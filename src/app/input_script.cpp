#include "app/input_script.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>

#include "core/log.h"
#include "platform/window.h"

namespace hailstone::app {

namespace {

int pad_button(std::string_view name) {
    static constexpr std::pair<std::string_view, SDL_GamepadButton> kButtons[] = {
        {"a", SDL_GAMEPAD_BUTTON_SOUTH},       {"b", SDL_GAMEPAD_BUTTON_EAST},
        {"x", SDL_GAMEPAD_BUTTON_WEST},        {"y", SDL_GAMEPAD_BUTTON_NORTH},
        {"back", SDL_GAMEPAD_BUTTON_BACK},     {"guide", SDL_GAMEPAD_BUTTON_GUIDE},
        {"start", SDL_GAMEPAD_BUTTON_START},   {"ls", SDL_GAMEPAD_BUTTON_LEFT_STICK},
        {"rs", SDL_GAMEPAD_BUTTON_RIGHT_STICK}, {"lb", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
        {"rb", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {"up", SDL_GAMEPAD_BUTTON_DPAD_UP},
        {"down", SDL_GAMEPAD_BUTTON_DPAD_DOWN}, {"left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
        {"right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    };
    for (auto [n, b] : kButtons)
        if (n == name) return b;
    return -1;
}

int pad_axis(std::string_view name) {
    static constexpr std::pair<std::string_view, SDL_GamepadAxis> kAxes[] = {
        {"lx", SDL_GAMEPAD_AXIS_LEFTX},         {"ly", SDL_GAMEPAD_AXIS_LEFTY},
        {"rx", SDL_GAMEPAD_AXIS_RIGHTX},        {"ry", SDL_GAMEPAD_AXIS_RIGHTY},
        {"lt", SDL_GAMEPAD_AXIS_LEFT_TRIGGER},  {"rt", SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
    };
    for (auto [n, a] : kAxes)
        if (n == name) return a;
    return -1;
}

double arg_or(const std::vector<std::string>& args, std::size_t i, double fallback) {
    return i < args.size() ? script_number(args[i], fallback) : fallback;
}

} // namespace

InputScript::InputScript(std::string_view script, platform::Window& window, Hooks hooks)
    : window_(window), hooks_(std::move(hooks)) {
    std::size_t at = 0;
    while (at <= script.size()) {
        std::size_t end = script.find(';', at);
        std::string part(script.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
        at = end == std::string_view::npos ? script.size() + 1 : end + 1;
        std::istringstream in(part);
        Step s;
        std::string time;
        if (!(in >> time >> s.action) || time.empty()) {
            if (part.find_first_not_of(" \t") != std::string::npos) log::warn("input script: bad step '{}'", part);
            continue;
        }
        s.clock = time[0] == 'c' ? Clock::Select : time[0] == 'd' ? Clock::Disconnect : Clock::Zone;
        s.at = script_number(time.substr(s.clock != Clock::Zone ? 1 : 0));
        std::getline(in >> std::ws, s.rest);
        std::istringstream words(s.rest);
        for (std::string w; words >> w;) s.args.push_back(w);
        steps_.push_back(std::move(s));
    }
    // By time within each run of steps on one clock.
    for (auto run = steps_.begin(); run != steps_.end();) {
        auto end = std::find_if(run, steps_.end(), [&](const Step& x) { return x.clock != run->clock; });
        std::stable_sort(run, end, [](const Step& a, const Step& b) { return a.at < b.at; });
        run = end;
    }
    if (!window_.attach_virtual_gamepad()) log::warn("input script: no virtual gamepad");
    log::info("input script: {} steps", steps_.size());
}

void InputScript::frame(double seconds, double select_seconds, double disconnect_seconds) {
    ++frame_;
    auto on = [&](Clock c) {
        return c == Clock::Select ? select_seconds : c == Clock::Disconnect ? disconnect_seconds : seconds;
    };
    // Releases first, so a press and the next step's press of the same button are separate frames. A press whose
    // screen went away (its clock back at -1: the character select left for the world, a zone left) lets go then.
    for (auto it = releases_.begin(); it != releases_.end();) {
        const double clock = on(it->clock);
        if ((clock < 0.0 || clock >= it->at) && frame_ >= it->frame) {
            it->fn();
            it = releases_.erase(it);
        } else {
            ++it;
        }
    }
    // One step a frame at most, so steps meant to be apart do not land in the same frame.
    if (next_ < steps_.size()) {
        const Step& s = steps_[next_];
        double now = on(s.clock);
        if (now >= s.at) { // a clock is -1 while its screen is not up
            ++next_;
            run(s, now);
        }
    }
}

void InputScript::run(const Step& s, double now) {
    log::info("input script {:.2f}: {} {}", s.at, s.action, s.rest);
    const auto& a = s.args;
    if (s.action == "pad" && !a.empty()) {
        int b = pad_button(a[0]);
        if (b < 0) return log::warn("input script: no pad button '{}'", a[0]);
        window_.set_virtual_pad_button(b, true);
        releases_.push_back({now + arg_or(a, 1, 0.15), s.clock, frame_ + kMinHeldFrames, [this, b] { window_.set_virtual_pad_button(b, false); }});
    } else if (s.action == "axis" && a.size() >= 2) {
        int ax = pad_axis(a[0]);
        if (ax < 0) return log::warn("input script: no pad axis '{}'", a[0]);
        window_.set_virtual_pad_axis(ax, static_cast<float>(script_number(a[1])));
        releases_.push_back({now + arg_or(a, 2, 0.5), s.clock, frame_ + kMinHeldFrames, [this, ax] { window_.set_virtual_pad_axis(ax, 0.0f); }});
    } else if (s.action == "key" && !a.empty()) {
        std::string name = a[0]; // "Left_Alt": an underscore stands for the space SDL's names have
        std::replace(name.begin(), name.end(), '_', ' ');
        SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
        if (sc == SDL_SCANCODE_UNKNOWN) return log::warn("input script: no key '{}'", a[0]);
        window_.push_key(sc, true);
        releases_.push_back({now + arg_or(a, 1, 0.08), s.clock, frame_ + kMinHeldFrames, [this, sc] { window_.push_key(sc, false); }});
    } else if (s.action == "text") {
        window_.push_text(s.rest);
    } else if (s.action == "click" && a.size() >= 2) {
        int w = 0, h = 0;
        SDL_GetWindowSize(window_.sdl_window(), &w, &h);
        float x = static_cast<float>(script_number(a[0]) * w), y = static_cast<float>(script_number(a[1]) * h);
        bool right = a.size() >= 3 && a[2] == "right";
        int button = right ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
        window_.push_mouse_button(x, y, button, true);
        releases_.push_back({now + arg_or(a, right ? 3 : 2, 0.08), s.clock, frame_ + kMinHeldFrames, [this, x, y, button] { window_.push_mouse_button(x, y, button, false); }});
    } else if (s.action == "wheel" && a.size() >= 3) {
        int w = 0, h = 0;
        SDL_GetWindowSize(window_.sdl_window(), &w, &h);
        float x = static_cast<float>(script_number(a[0]) * w), y = static_cast<float>(script_number(a[1]) * h);
        window_.push_mouse_motion(x, y);
        SDL_Event ev{};
        ev.type = SDL_EVENT_MOUSE_WHEEL;
        ev.wheel.windowID = SDL_GetWindowID(window_.sdl_window());
        ev.wheel.y = static_cast<float>(script_number(a[2]));
        ev.wheel.mouse_x = x;
        ev.wheel.mouse_y = y;
        SDL_PushEvent(&ev);
    } else if ((s.action == "move" || s.action == "press" || s.action == "release") && a.size() >= 2) {
        int w = 0, h = 0;
        SDL_GetWindowSize(window_.sdl_window(), &w, &h);
        float x = static_cast<float>(script_number(a[0]) * w), y = static_cast<float>(script_number(a[1]) * h);
        if (s.action == "move") window_.push_mouse_motion(x, y);
        else window_.push_mouse_button(x, y, SDL_BUTTON_LEFT, s.action == "press");
    } else if (s.action == "shot" && !a.empty()) {
        if (hooks_.screenshot) hooks_.screenshot(a[0]);
    } else if (s.action == "state") {
        if (hooks_.state) log::info("input script state: {}", hooks_.state());
    } else if (s.action == "close") {
        window_.push_close_request();
    } else if (s.action == "droplink") {
        if (hooks_.drop_link) hooks_.drop_link();
    } else {
        log::warn("input script: unknown step '{} {}'", s.action, s.rest);
    }
}

} // namespace hailstone::app
