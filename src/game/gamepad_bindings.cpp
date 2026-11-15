#include "game/gamepad_bindings.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>
#include <utility>

#include "core/config.h"
#include "game/keymap.h"

namespace hailstone::game {

namespace {

using Kind = PadAction::Kind;

constexpr std::string_view kBindPrefix = "gamepad.bind.";
constexpr std::string_view kCrossPrefix = "gamepad.cross.";
constexpr int kHotBars = 10, kHotBarButtons = 12; // ActionsUi::hot_button's bars and buttons

// The pad's own actions by name.
constexpr std::pair<Kind, std::string_view> kPadActions[] = {
    {Kind::Menu, "menu"},
    {Kind::Cursor, "cursor"},
    {Kind::CameraToggle, "camera_toggle"},
    {Kind::Interact, "interact"},
    {Kind::Cancel, "cancel"},
    {Kind::TargetPrev, "target_prev"},
    {Kind::TargetNext, "target_next"},
    {Kind::TargetGroup, "target_group"},
    {Kind::HotPagePrev, "hotpage_prev"},
    {Kind::HotPageNext, "hotpage_next"},
    {Kind::Chat, "chat"},
    {Kind::CameraIso, "camera_iso"},
};

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::optional<int> parse_int(std::string_view s) {
    int v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || p != s.data() + s.size()) return std::nullopt;
    return v;
}

// "<a>:<b>" as two ints.
std::optional<std::pair<int, int>> parse_pair(std::string_view s) {
    std::size_t colon = s.find(':');
    if (colon == std::string_view::npos) return std::nullopt;
    auto a = parse_int(trim(s.substr(0, colon))), b = parse_int(trim(s.substr(colon + 1)));
    if (!a || !b) return std::nullopt;
    return std::pair{*a, *b};
}

std::optional<float> parse_float(std::string_view s) {
    std::string text(trim(s));
    if (text.empty()) return std::nullopt;
    char* end = nullptr;
    float v = std::strtof(text.c_str(), &end);
    if (end == text.c_str()) return std::nullopt;
    return v;
}

bool is_trigger(PadButton b) { return b == PadButton::LT || b == PadButton::RT; }

} // namespace

std::optional<PadAction> parse_pad_action(std::string_view text) {
    text = trim(text);
    const std::string l = lower(text);
    if (l.empty() || l == "none") return PadAction{};
    if (l.starts_with("hot:")) {
        auto p = parse_pair(std::string_view(l).substr(4));
        if (!p || p->first < 1 || p->first > kHotBars || p->second < 1 || p->second > kHotBarButtons) return std::nullopt;
        return PadAction::hot(p->first, p->second);
    }
    for (auto [kind, name] : kPadActions)
        if (l == name) return PadAction::of(kind);
    // A key command, with or without the CMD_ prefix the original gives some (docs/gamepad.md writes CMD_FORWARD
    // for FORWARD).
    const std::string u = upper(text);
    if (auto c = command_by_name(u)) return PadAction::key(*c);
    if (auto c = command_by_name("CMD_" + u)) return PadAction::key(*c);
    if (u.starts_with("CMD_"))
        if (auto c = command_by_name(std::string_view(u).substr(4))) return PadAction::key(*c);
    return std::nullopt;
}

std::string pad_action_text(const PadAction& a) {
    switch (a.kind) {
    case Kind::None: return "none";
    case Kind::Command: return command_name(a.command);
    case Kind::Hot: return std::format("hot:{}:{}", a.bar, a.button);
    default:
        for (auto [kind, name] : kPadActions)
            if (kind == a.kind) return std::string(name);
        return "none";
    }
}

std::optional<PadInput> parse_pad_input(std::string_view text) {
    const std::string l = lower(trim(text));
    PadInput in;
    std::string_view rest = l;
    while (true) {
        std::size_t plus = rest.find('+');
        std::string_view token = trim(rest.substr(0, plus));
        if (plus == std::string_view::npos) {
            auto b = platform::pad_button_by_name(token);
            if (!b) return std::nullopt;
            in.button = *b;
            break;
        }
        std::uint8_t bit = token == "lt" ? kChordLT : token == "rt" ? kChordRT : token == "back" ? kChordBack : 0;
        if (!bit || (in.chord & bit)) return std::nullopt;
        in.chord = static_cast<std::uint8_t>(in.chord | bit);
        rest = rest.substr(plus + 1);
    }
    // Back is a modifier only for Start, and alone; the triggers take no chord themselves.
    if ((in.chord & kChordBack) && (in.chord != kChordBack || in.button != PadButton::Start)) return std::nullopt;
    if (in.chord && is_trigger(in.button)) return std::nullopt;
    if ((in.chord & (kChordLT | kChordRT)) && (in.button == PadButton::Back || in.button == PadButton::Start))
        return std::nullopt;
    return in;
}

std::string pad_input_text(PadInput in) {
    std::string out;
    if (in.chord & kChordBack) out += "back+";
    if (in.chord & kChordLT) out += "lt+";
    if (in.chord & kChordRT) out += "rt+";
    out += platform::pad_button_name(in.button);
    return out;
}

int cross_slot_of(PadButton b) {
    switch (b) {
    case PadButton::Left: return 1;
    case PadButton::Up: return 2;
    case PadButton::Right: return 3;
    case PadButton::Down: return 4;
    case PadButton::X: return 5;
    case PadButton::Y: return 6;
    case PadButton::B: return 7;
    case PadButton::A: return 8;
    default: return 0;
    }
}

int cross_set(const GamepadState& pad) {
    return (pad.is_down(PadButton::LT) ? 1 : 0) | (pad.is_down(PadButton::RT) ? 2 : 0);
}

PadAction default_pad_action(PadInput in) {
    using B = PadButton;
    using C = KeyCommand;
    if (in.chord == kChordBack && in.button == B::Start) return PadAction::of(Kind::Chat);
    if (in.chord) return {};
    auto key = [](C c) { return PadAction::key(static_cast<int>(c)); };
    switch (in.button) {
    case B::A: return PadAction::of(Kind::Interact);
    case B::B: return PadAction::of(Kind::Cancel);
    case B::X: return key(C::AUTOPRIM);
    case B::Y: return key(C::JUMP);
    case B::LB: return PadAction::of(Kind::TargetPrev);
    case B::RB: return PadAction::of(Kind::TargetNext);
    case B::Up: return PadAction::of(Kind::TargetGroup);
    case B::Down: return key(C::SIT_STAND);
    case B::Left: return PadAction::of(Kind::HotPagePrev);
    case B::Right: return PadAction::of(Kind::HotPageNext);
    case B::LS: return key(C::AUTORUN);
    case B::RS: return PadAction::of(Kind::CameraToggle);
    case B::Start: return PadAction::of(Kind::Menu);
    case B::Back: return PadAction::of(Kind::Cursor);
    default: return {};
    }
}

void GamepadBindings::reset_defaults() {
    bindings_.clear();
    for (int s = 0; s < kCrossSets; ++s)
        for (int k = 0; k < kCrossSlots; ++k)
            cross_[static_cast<std::size_t>(s)][static_cast<std::size_t>(k)] = {s + 1, k + 1};
}

PadAction GamepadBindings::action(PadInput in) const {
    if (auto it = bindings_.find(in); it != bindings_.end()) return it->second;
    if (int set = in.chord & (kChordLT | kChordRT)) {
        if (int slot = cross_slot_of(in.button)) {
            CrossSlot c = cross(set, slot);
            return PadAction::hot(c.bar, c.button);
        }
        return action(PadInput{in.button, 0});
    }
    return default_pad_action(in);
}

std::optional<PadAction> GamepadBindings::binding(PadInput in) const {
    if (auto it = bindings_.find(in); it != bindings_.end()) return it->second;
    return std::nullopt;
}

void GamepadBindings::set_binding(PadInput in, std::optional<PadAction> a) {
    // A plain input bound to its default keeps no entry; a chord's explicit action always does.
    const bool plain = (in.chord & (kChordLT | kChordRT)) == 0;
    if (!a || (plain && *a == default_pad_action(in))) bindings_.erase(in);
    else bindings_[in] = *a;
}

CrossSlot GamepadBindings::cross(int set, int slot) const {
    if (set < 1 || set > kCrossSets || slot < 1 || slot > kCrossSlots) return {0, 0};
    return cross_[static_cast<std::size_t>(set - 1)][static_cast<std::size_t>(slot - 1)];
}

bool GamepadBindings::set_cross(int set, int slot, CrossSlot s) {
    if (set < 1 || set > kCrossSets || slot < 1 || slot > kCrossSlots) return false;
    if (s.bar < 1 || s.bar > kHotBars || s.button < 1 || s.button > kHotBarButtons) return false;
    cross_[static_cast<std::size_t>(set - 1)][static_cast<std::size_t>(slot - 1)] = s;
    return true;
}

void GamepadBindings::load(const Config& config) {
    reset_defaults();
    for (const auto& [key, value] : config.entries()) {
        std::string_view k = key;
        if (k.starts_with(kBindPrefix)) {
            auto in = parse_pad_input(k.substr(kBindPrefix.size()));
            auto a = parse_pad_action(value);
            if (in && a) set_binding(*in, *a);
        } else if (k.starts_with(kCrossPrefix)) {
            std::string where_text(k.substr(kCrossPrefix.size())); // "<set>.<slot>"
            std::replace(where_text.begin(), where_text.end(), '.', ':');
            auto where = parse_pair(where_text);
            auto hot = parse_pair(value);
            if (where && hot) set_cross(where->first, where->second, {hot->first, hot->second});
        }
    }
}

void GamepadBindings::save(Config& config) const {
    std::vector<std::string> old;
    for (const auto& e : config.entries())
        if (std::string_view(e.first).starts_with(kBindPrefix) || std::string_view(e.first).starts_with(kCrossPrefix))
            old.push_back(e.first);
    for (const auto& k : old) config.erase(k);
    for (const auto& [in, a] : bindings_) config.set(std::string(kBindPrefix) + pad_input_text(in), pad_action_text(a));
    for (int s = 1; s <= kCrossSets; ++s)
        for (int k = 1; k <= kCrossSlots; ++k)
            if (CrossSlot c = cross(s, k); c != CrossSlot{s, k})
                config.set(std::format("{}{}.{}", kCrossPrefix, s, k), std::format("{}:{}", c.bar, c.button));
}

void GamepadBindings::update(const GamepadState& pad, std::vector<PadEvent>& out) {
    const std::uint8_t chord = static_cast<std::uint8_t>(cross_set(pad));
    auto fire = [&](const PadAction& a) {
        if (a.kind == Kind::None) return;
        out.push_back({a, true});
        out.push_back({a, false});
    };
    for (std::size_t i = 0; i < platform::kPadButtons; ++i) {
        const auto b = static_cast<PadButton>(i);
        if (b == PadButton::Back || b == PadButton::Start) continue;
        if (pad.pressed[i]) {
            // A release that was missed ends the old action first.
            if (held_[i]) out.push_back({*std::exchange(held_[i], std::nullopt), false});
            PadAction a = action(PadInput{b, is_trigger(b) ? std::uint8_t{0} : chord});
            if (a.kind == Kind::None) continue;
            out.push_back({a, true});
            if (pad.down[i]) held_[i] = a;
            else out.push_back({a, false}); // pressed and let go within the frame
        } else if ((pad.released[i] || !pad.down[i]) && held_[i]) {
            out.push_back({*std::exchange(held_[i], std::nullopt), false});
        }
    }
    // Back and Start: together they are back+start; alone each fires as it is let go.
    const bool back = pad.is_down(PadButton::Back) || pad.was_pressed(PadButton::Back);
    const bool start = pad.is_down(PadButton::Start) || pad.was_pressed(PadButton::Start);
    if ((pad.was_pressed(PadButton::Back) || pad.was_pressed(PadButton::Start)) && back && start && !back_start_) {
        back_start_ = true;
        fire(action(PadInput{PadButton::Start, kChordBack}));
    }
    for (PadButton b : {PadButton::Back, PadButton::Start})
        if (pad.was_released(b) && !back_start_) fire(action(PadInput{b, 0}));
    if (!pad.is_down(PadButton::Back) && !pad.is_down(PadButton::Start)) back_start_ = false;
}

void GamepadBindings::release_all(std::vector<PadEvent>& out) {
    for (auto& h : held_)
        if (h) out.push_back({*std::exchange(h, std::nullopt), false});
    back_start_ = false;
}

StickMove stick_move(float lx, float ly, float move, float strafe) {
    StickMove m;
    m.forward = ly < -move;
    m.back = ly > move;
    m.left = lx < -strafe;
    m.right = lx > strafe;
    return m;
}

// ---------------------------------------------------------------- settings

namespace {

struct SettingField {
    std::string_view name;
    float GamepadSettings::* number = nullptr;
    bool GamepadSettings::* flag = nullptr;
    float lo = 0.0f, hi = 1.0f;
};

const std::array<SettingField, 9>& setting_fields() {
    static const std::array<SettingField, 9> fields = {{
        {"enabled", nullptr, &GamepadSettings::enabled, 0, 1},
        {"deadzone", &GamepadSettings::deadzone, nullptr, 0.0f, 0.9f},
        {"trigger_threshold", &GamepadSettings::trigger_threshold, nullptr, 0.05f, 0.95f},
        {"camera_speed", &GamepadSettings::camera_speed, nullptr, 0.1f, 20.0f},
        {"invert_y", nullptr, &GamepadSettings::invert_y, 0, 1},
        {"rumble", nullptr, &GamepadSettings::rumble, 0, 1},
        {"move_threshold", &GamepadSettings::move_threshold, nullptr, 0.05f, 0.95f},
        {"strafe_threshold", &GamepadSettings::strafe_threshold, nullptr, 0.05f, 0.95f},
        {"cursor_speed", &GamepadSettings::cursor_speed, nullptr, 50.0f, 5000.0f},
    }};
    return fields;
}

const SettingField* setting_field(std::string_view name) {
    if (name.starts_with("gamepad.")) name.remove_prefix(8);
    for (const auto& f : setting_fields())
        if (f.name == name) return &f;
    return nullptr;
}

} // namespace

void GamepadSettings::load(const Config& config) {
    *this = GamepadSettings{};
    for (const auto& f : setting_fields())
        if (auto v = config.get(std::string("gamepad.") + std::string(f.name)))
            if (auto n = parse_float(*v)) set(f.name, *n);
}

void GamepadSettings::save(Config& config) const {
    for (const auto& f : setting_fields()) {
        std::string key = std::string("gamepad.") + std::string(f.name);
        if (f.flag) config.set_bool(key, this->*f.flag);
        else config.set(key, std::format("{}", this->*f.number));
    }
}

std::optional<float> GamepadSettings::get(std::string_view name) const {
    const SettingField* f = setting_field(name);
    if (!f) return std::nullopt;
    return f->flag ? (this->*f->flag ? 1.0f : 0.0f) : this->*f->number;
}

bool GamepadSettings::set(std::string_view name, float value) {
    const SettingField* f = setting_field(name);
    // NaN would pass std::clamp and be saved to hailstone.ini; an infinity is no setting either.
    if (!f || !std::isfinite(value)) return false;
    if (f->flag) this->*f->flag = value != 0.0f;
    else this->*f->number = std::clamp(value, f->lo, f->hi);
    return true;
}

const std::vector<std::string_view>& GamepadSettings::names() {
    static const std::vector<std::string_view> n = [] {
        std::vector<std::string_view> v;
        for (const auto& f : setting_fields()) v.push_back(f.name);
        return v;
    }();
    return n;
}

// ---------------------------------------------------------------- glyphs

const char* pad_glyph(PadType type, PadButton b) {
    // PadButton order: a b x y lb rb lt rt back start guide ls rs up down left right touchpad.
    static constexpr std::array<const char*, platform::kPadButtons> kXbox = {
        "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "View", "Menu", "Guide", "LS", "RS", "Up", "Down", "Left", "Right",
        "Touchpad"};
    static constexpr std::array<const char*, platform::kPadButtons> kPlayStation = {
        "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "Share", "Options", "PS", "L3", "R3", "Up",
        "Down", "Left", "Right", "Touchpad"};
    static constexpr std::array<const char*, platform::kPadButtons> kNintendo = {
        "B", "A", "Y", "X", "L", "R", "ZL", "ZR", "-", "+", "Home", "LS", "RS", "Up", "Down", "Left", "Right",
        "Touchpad"};
    static constexpr std::array<const char*, platform::kPadButtons> kGeneric = {
        "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Back", "Start", "Guide", "LS", "RS", "Up", "Down", "Left", "Right",
        "Touchpad"};
    if (b >= PadButton::Count) return "";
    const auto i = static_cast<std::size_t>(b);
    switch (type) {
    case PadType::Xbox: return kXbox[i];
    case PadType::PlayStation: return kPlayStation[i];
    case PadType::Nintendo: return kNintendo[i];
    default: return kGeneric[i];
    }
}

} // namespace hailstone::game
