#include "game/spawn_heights.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "core/msvc_atoi.h"
#include "game/mechanics/collision_movement.h"
#include "game/race_models.h"

namespace hailstone::game {

using mechanics::X87;
using mechanics::x87_add;
using mechanics::x87_mul;
using mechanics::x87_sub;

namespace {

// The original's float constants.
constexpr float kSixth = 1.0f / 6.0f;          // 0x9d77cc (0x3e2aaaab)
constexpr float kNewModelFactor = 1.3f;        // 0x9c7b68 (0x3fa66666)
constexpr float kOldModelFactor = 0.2f;        // 0x9c5728 (0x3e4ccccd)
constexpr float kOldModelSwitched = 0.769230783f; // 0x9d70ec (0x3f44ec4f, 1 / 1.3)
constexpr float kViewHeight = 6.2f;            // 0x9d79cc
constexpr float kNoScale = -1.0f;              // 0x9c4ec4
constexpr float kJumpStrength = 1.35f;         // 0x9d79c8
constexpr float kSwimStrength = 3.375f;        // 0x9d79c4
constexpr float kFour = 4.0f;                  // 0x9c5764
constexpr float kSpeakNear = 0.3f;             // 0x9ca178 (also the floor)
constexpr float kSpeakFar = 0.6f;              // 0x9d79c0
constexpr float kTenth = 0.1f;                 // 0x9c4a48
constexpr float kThreeQuarters = 0.75f;        // 0x9c59f8
constexpr std::int32_t kRaceFroglok = 330;     // 0x14a

// A float copied through the FPU: a signalling NaN comes out quiet.
float quiet(float v) {
    std::uint32_t b;
    std::memcpy(&b, &v, 4);
    if ((b & 0x7f800000u) == 0x7f800000u && (b & 0x007fffffu)) b |= 0x00400000u;
    std::memcpy(&v, &b, 4);
    return v;
}

float to_f(double v) { return static_cast<float>(v); }

enum class Special : std::uint8_t { None, EyeScaled, Eye5, Length, Tiny, Small, Small2, Giant, Short, Eye39 };

// ComputeSpecialHeights' two jump tables (races 0x1a..0xfc and 0x164..0x228) and race 0x14a.
Special special_case(std::int32_t race) {
    switch (race) {
    case 26: case 27: case 330: return Special::EyeScaled;
    case 36: case 91: return Special::Length;
    case 49: case 158: case 196: return Special::Giant;
    case 50: return Special::Eye5;
    case 72: return Special::Small2;
    case 73: case 114: case 141: case 252: case 404: case 533: case 544: case 545: case 546: case 550: case 551:
    case 552: return Special::Small;
    case 108: return Special::Tiny;
    case 120: case 171: case 356: return Special::Eye39;
    case 142: case 143: return Special::Short;
    default: return Special::None;
    }
}

} // namespace

// ---------------------------------------------------------------- tables

float HeightTables::default_height(std::int32_t race, std::uint8_t gender) const {
    auto it = race_heights.find(race);
    if (it == race_heights.end()) return kDefaultHeight;
    return it->second[gender == 1 ? 1 : 0];
}

float HeightTables::roffset(std::string_view code) const {
    std::string key(code);
    for (char& c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    auto it = roffsets.find(key);
    return it == roffsets.end() ? kDefaultROffset : it->second;
}

namespace {
float by_code(const std::unordered_map<std::string, float>& table, std::string_view code, float fallback) {
    std::string key(code);
    for (char& c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    auto it = table.find(key);
    return it == table.end() ? fallback : it->second;
}
} // namespace

std::string_view HeightTables::racedata_code(std::int32_t race, std::uint8_t gender) const {
    for (int g : {static_cast<int>(gender), 2})
        if (auto it = racedata_codes.find(race * 4 + g); it != racedata_codes.end()) return it->second;
    return {};
}

float HeightTables::fpc_offset(std::string_view code) const { return by_code(fpc_offsets, code, 0.0f); }
float HeightTables::ccc_distance(std::string_view code) const { return by_code(ccc_distances, code, kDefaultCccDistance); }
float HeightTables::ccc_height(std::string_view code) const { return by_code(ccc_heights, code, kDefaultCccHeight); }
float HeightTables::pes_factor(std::string_view code) const { return by_code(pes_factors, code, 1.0f); }

HeightTables parse_height_tables(std::string_view racedata, std::string_view moddat) {
    HeightTables t;
    std::size_t pos = 0;
    while (pos < racedata.size()) {
        std::size_t nl = racedata.find('\n', pos);
        std::string_view line = racedata.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? racedata.size() : nl + 1;
        if (line.empty() || !std::isdigit(static_cast<unsigned char>(line.front()))) continue;
        std::vector<std::string> fields;
        std::size_t at = 0;
        while (at <= line.size()) {
            std::size_t caret = line.find('^', at);
            fields.emplace_back(line.substr(at, caret == std::string_view::npos ? std::string_view::npos : caret - at));
            if (caret == std::string_view::npos) break;
            at = caret + 1;
        }
        auto height = [](const std::string& f) {
            float v = static_cast<float>(std::atof(f.c_str()));
            return v < 1.0f ? kDefaultHeight : v; // @0x7f4997 / @0x7f49c5
        };
        if (fields.size() >= 48 && fields.size() < 86) {
            // The live client's layout: a line per race and gender (0, 1, or 2 for both), the heights at 46 and 47.
            auto& h = t.race_heights
                          .try_emplace(core::msvc_atoi(fields[0]), std::array<float, 2>{kDefaultHeight, kDefaultHeight})
                          .first->second;
            if (fields[1] != "1") h[0] = height(fields[46]);
            if (fields[1] != "0") h[1] = height(fields[47]);
            if (fields.size() > 50 && !fields[50].empty()) {
                int gender = fields[1] == "0" ? 0 : fields[1] == "1" ? 1 : 2;
                std::string code = fields[50];
                for (char& c : code) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                t.racedata_codes[core::msvc_atoi(fields[0]) * 4 + gender] = std::move(code);
            }
            continue;
        }
        if (fields.size() < 91) continue;
        t.race_heights[core::msvc_atoi(fields[0])] = {height(fields[89]), height(fields[90])};
    }
    // moddat.ini as GetPrivateProfileString reads it: the first [section] of a name and its first ROffset /
    // FPCOffset key, both compared without case; the value is read with sscanf "%f" (the default when it does not
    // parse).
    auto trim = [](std::string_view s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
        return s;
    };
    auto upper = [](std::string_view s) {
        std::string u(s);
        for (char& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return u;
    };
    std::string section;
    std::unordered_map<std::string, bool> seen; // sections already passed (a repeated one is not read again)
    bool reading = false;
    pos = 0;
    while (pos < moddat.size()) {
        std::size_t nl = moddat.find('\n', pos);
        std::string_view line = trim(moddat.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos));
        pos = nl == std::string_view::npos ? moddat.size() : nl + 1;
        if (line.empty() || line.front() == ';') continue;
        if (line.front() == '[') {
            std::size_t close = line.find(']');
            section = upper(trim(line.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1)));
            reading = !seen[section];
            seen[section] = true;
            continue;
        }
        std::size_t eq = line.find('=');
        if (!reading || eq == std::string_view::npos) continue;
        const std::string key = upper(trim(line.substr(0, eq)));
        std::unordered_map<std::string, float>* table = key == "ROFFSET"   ? &t.roffsets
                                                        : key == "FPCOFFSET" ? &t.fpc_offsets
                                                        : key == "CCCDIS"    ? &t.ccc_distances
                                                        : key == "CCCHT"     ? &t.ccc_heights
                                                        : key == "PESFACTOR" ? &t.pes_factors
                                                                             : nullptr;
        if (!table || table->contains(section)) continue;
        const float fallback = key == "ROFFSET" ? kDefaultROffset
                               : key == "CCCDIS" ? kDefaultCccDistance
                               : key == "CCCHT"  ? kDefaultCccHeight
                               : key == "PESFACTOR" ? 1.0f
                                                 : 0.0f;
        std::string value(trim(line.substr(eq + 1)));
        char* end = nullptr;
        float parsed = std::strtof(value.c_str(), &end);
        (*table)[section] = end != value.c_str() ? parsed : fallback;
    }
    return t;
}

bool is_new_style_model_race(std::int32_t race) {
    if (race < 0 || race > 0xffff) return false;
    const RaceModel* m = find_race_model(static_cast<std::uint16_t>(race), 2); // gender 2, then 0, then 1
    return m && (m->flags & kNewStyleModel);
}

std::string_view height_model_code(std::int32_t race, std::uint8_t gender) {
    const RaceModel* m = race >= 0 && race <= 0xffff ? find_race_model(static_cast<std::uint16_t>(race), gender) : nullptr;
    if (m) return m->code;
    return gender == 1 ? "HUF" : "HUM";
}

HeightInputs height_inputs(const HeightTables* tables, std::int32_t race, std::uint8_t gender, bool old_model_scale) {
    HeightInputs in;
    in.race = race;
    in.new_style_model = is_new_style_model_race(race);
    in.has_race_manager = tables != nullptr;
    in.racedata_height = tables ? tables->default_height(race, gender) : kDefaultHeight;
    in.roffset = tables ? tables->roffset(height_model_code(race, gender)) : kDefaultROffset;
    in.old_model_scale = old_model_scale;
    return in;
}

// ---------------------------------------------------------------- the rules

void compute_special_heights(std::int32_t race, float& scale, float& eye, float& avatar, float& length, const Fpu& f) {
    switch (special_case(race)) {
    case Special::EyeScaled: eye = to_f(x87_mul(f, X87(eye), X87(kThreeQuarters))); return;
    case Special::Eye5: eye = 5.0f; return;
    case Special::Length:
        length = to_f(x87_mul(f, X87(avatar), X87(kThreeQuarters)));
        eye = 3.0f;
        return;
    case Special::Tiny: eye = 3.0f; avatar = 3.0f; scale = 0.05f; return;
    case Special::Small: eye = 1.0f; avatar = kTenth; scale = 1.0f; return;
    case Special::Small2: eye = 1.0f; avatar = kTenth; scale = 2.0f; return;
    case Special::Giant: eye = 34.5f; avatar = 20.0f; scale = 1.0f; return;
    case Special::Short: eye = 6.0f; avatar = kTenth; scale = 1.0f; return;
    case Special::Eye39: eye = 3.9f; return;
    case Special::None: return;
    }
}

bool set_heights(SpawnHeights& s, const HeightInputs& in, float view_offset, float anim_scale, const Fpu& f) {
    s.length = 0.0f;
    bool asked = false;
    float height;
    if (!(s.height <= 0.0f)) {
        height = quiet(s.height); // kept (a NaN too)
    } else {
        if (in.has_race_manager) {
            height = quiet(in.racedata_height);
            asked = true;
        } else {
            height = kDefaultHeight;
        }
        s.height = height;
    }
    float avatar0 = quiet(in.roffset); // ModDat_GetROffset's result into +0x138
    // Height * first [* second], stored as +0x31c (kept unrounded); that * [+0x138], stored as +0x138.
    double scale = in.new_style_model ? x87_mul(f, X87(height), X87(kSixth)) : x87_mul(f, X87(height), X87(kOldModelFactor));
    if (in.new_style_model && !in.old_model_scale) scale = x87_mul(f, scale, X87(kNewModelFactor));
    if (!in.new_style_model && in.old_model_scale) scale = x87_mul(f, scale, X87(kOldModelSwitched));
    s.model_scale = to_f(scale);
    s.avatar_height = to_f(x87_mul(f, scale, X87(avatar0)));
    s.view_height = to_f(x87_mul(f, X87(kViewHeight), X87(s.model_scale))); // 0x506330 x [+0x31c]

    float new_scale = kNoScale;
    float eye = quiet(s.view_height);
    float avatar = quiet(s.avatar_height);
    float length = quiet(s.length);
    compute_special_heights(in.race, new_scale, eye, avatar, length, f);
    // eye + viewOffset stored as +0x190; viewOffset stored as +0x274.
    s.view_height = to_f(x87_add(f, X87(eye), X87(view_offset)));
    s.camera_offset = quiet(view_offset);
    s.animation_scale = quiet(anim_scale);
    s.avatar_height = quiet(avatar);
    s.length = quiet(length);
    if (!(new_scale == kNoScale)) s.model_scale = quiet(new_scale);
    s.jump_strength = kJumpStrength;
    s.swim_strength = kSwimStrength;
    // avatar x (4.0 <= avatar ? 0.3 : 0.6) (a NaN takes 0.6), at least 0.3.
    bool near = kFour <= avatar;
    s.max_speak_distance = to_f(x87_mul(f, X87(avatar), X87(near ? kSpeakNear : kSpeakFar)));
    if (kSpeakNear > s.max_speak_distance) s.max_speak_distance = kSpeakNear;
    if (s.avatar_height == 0.0f) s.avatar_height = kTenth;
    return asked;
}

std::vector<HeightActorUpdate> change_height(std::span<HeightLink> chain, float size, float view_offset,
                                             float anim_scale, const Fpu& f) {
    std::vector<HeightActorUpdate> out;
    for (std::size_t k = 0; k < chain.size(); ++k) {
        HeightLink& link = chain[k];
        SpawnHeights& h = *link.heights;
        h.height = quiet(size);
        set_heights(h, link.inputs, k == 0 ? quiet(view_offset) : 0.0f, k == 0 ? quiet(anim_scale) : 1.0f, f);
        HeightActorUpdate u;
        u.spawn = k;
        u.scale = quiet(h.model_scale);
        if (link.has_actor)
            u.volume_radius = to_f(mechanics::collision_volume_radius(h.avatar_height, link.type == 1,
                                                                      static_cast<std::uint32_t>(link.inputs.race),
                                                                      link.mount_race, f));
        out.push_back(u);
    }
    return out;
}

double calc_scale_factor_for_height(float height, bool new_style_model, bool old_model_scale, const Fpu& f) {
    if (new_style_model) {
        double v = x87_mul(f, X87(height), X87(kSixth));
        return old_model_scale ? v : x87_mul(f, v, X87(kNewModelFactor));
    }
    double v = x87_mul(f, X87(height), X87(kOldModelFactor));
    return old_model_scale ? x87_mul(f, v, X87(kOldModelSwitched)) : v;
}

double eye_height(const EyeInputs& in, const Fpu& f) {
    if (in.uses_new_style_model) {
        // [def +0x48] * [+0x31c] + [+0x274].
        return x87_add(f, x87_mul(f, X87(in.definition_eye), X87(in.model_scale)), X87(in.camera_offset));
    }
    float avatar = quiet(in.avatar_height); // [+0x138] copied through the FPU into a temporary
    double v = x87_sub(f, X87(quiet(in.view_height)), X87(avatar)); // 0x506330 less the temporary
    if (in.mounted) v = x87_add(f, v, X87(in.race == kRaceFroglok ? 1.0f : 0.5f));
    return v;
}

float eye_height(const SpawnHeights& s, std::int32_t race, bool mounted) {
    EyeInputs in;
    in.mounted = mounted;
    in.race = race;
    in.view_height = s.view_height;
    in.avatar_height = s.avatar_height;
    in.model_scale = s.model_scale;
    in.camera_offset = s.camera_offset;
    return to_f(eye_height(in));
}

std::optional<float> actor_height_fixup(std::int32_t race, float height) {
    struct Fix {
        std::int32_t race;
        float from, to;
    };
    static constexpr Fix kFixes[] = {{7, 5.5f, 5.3f}, {9, 8.0f, 7.5f}, {10, 9.0f, 8.8f}, {128, 6.0f, 6.5f}, {330, 6.0f, 5.0f}};
    for (const Fix& x : kFixes)
        if (x.race == race && height == x.from) return x.to; // a NaN never matches
    return std::nullopt;
}

} // namespace hailstone::game
