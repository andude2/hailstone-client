#pragma once

// A spawn's heights: how the original client turns its size, race and model
// into its model scale, AvatarHeight (its z above its feet), ViewHeight (the
// eye above the feet) and the rest SetHeights writes, and what follows a size
// change. They follow the original's x87 arithmetic (game/mechanics/fpu.h),
// checked against the vectors recorded from eqgame.exe
// (tests/spawn_heights_vectors_test.cpp).
//
// The rules the rest of the client follows from them:
//  - feet = z - AvatarHeight: a spawn at rest has z = floor + AvatarHeight
//    (LandingAndFallCheck), and the server's z is the spawn's z;
//  - the actor's origin is the spawn's z. WLD models are authored with their
//    origin there; EQG models have it at the feet, and the animation loader
//    lowers their ROOT_BONE by moddat.ini's ROffset (model units, so
//    ROffset x the model scale in the world): eqg_root_drop();
//  - a human in zone (Height 6, classic model): scale 1.2, AvatarHeight 3.75,
//    ViewHeight 7.44.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "game/mechanics/fpu.h"

namespace hailstone::game {

using mechanics::Fpu;

// The fields EQPlayer::SetHeights writes (PlayerBase / PlayerZoneClient).
struct SpawnHeights {
    float height = 0.0f;             // +0x13c Height: the size (OP_ZoneEntry, OP_ChangeSize, illusions)
    float avatar_height = 0.0f;      // +0x138 AvatarHeight: the spawn's z above its feet
    float model_scale = 0.0f;        // +0x31c: the model scale (the actor's scale)
    float view_height = 0.0f;        // +0x190 ViewHeight: the eye above the feet
    float camera_offset = 0.0f;      // +0x274 CameraOffset: SetHeights' view offset
    float animation_scale = 1.0f;    // +0x228: SetHeights' animation scale
    float length = 0.0f;             // +0x144 Length: the special height (races 36, 91)
    float jump_strength = 0.0f;      // +0x10 JumpStrength (1.35)
    float swim_strength = 0.0f;      // +0x14 SwimStrength (3.375)
    float max_speak_distance = 0.0f; // vf44's +0x11c4 MaxSpeakDistance
};

// What SetHeights asks the game's data for one spawn.
struct HeightInputs {
    std::int32_t race = 0;          // GetRace (vf88)
    bool new_style_model = false;   // NpcManager_IsNewStyleModelRace(race)
    bool has_race_manager = true;   // the RaceManager (racedata.txt) exists
    float racedata_height = 6.0f;   // RaceManager_GetDefaultHeight(race, gender)
    float roffset = 3.125f;         // ModDat_GetROffset(model code): moddat.ini ROffset
    bool old_model_scale = false;   // dword 0xddfba4 (NewZone +0x2b4; 1 until the first OP_NewZone)
};

// racedata.txt's default heights and moddat.ini's ROffsets and FPCOffsets.
struct HeightTables {
    std::unordered_map<std::int32_t, std::array<float, 2>> race_heights; // male, female
    // The live client's racedata.txt also names each race and gender's model code (field 50); RoF2's has no
    // codes (its table is in the client, game/race_models.h). Keyed race * 4 + gender (0, 1, 2 for both).
    std::unordered_map<std::int32_t, std::string> racedata_codes;
    // The live racedata.txt's model code for (race, gender), else its gender-2 row's, else empty.
    std::string_view racedata_code(std::int32_t race, std::uint8_t gender) const;
    std::unordered_map<std::string, float> roffsets;                     // upper-case model code -> ROffset
    std::unordered_map<std::string, float> fpc_offsets;                  // upper-case model code -> FPCOffset
    // RaceManager_GetDefaultHeight @0x7f4730: column 90 when gender == 1, else column 89; 6.0 for a race
    // without a row.
    float default_height(std::int32_t race, std::uint8_t gender) const;
    // ModDat_GetROffset: [code] ROffset, 3.125 when there is none.
    float roffset(std::string_view code) const;
    // ModDat_GetFPCOffset @0x56e000: [code] FPCOffset, 0 when there is none (CreateActorDefinition keeps it in the
    // actor definition's +0x40; the first-person camera on a mount moves forward by it).
    float fpc_offset(std::string_view code) const;
    // ModDat_GetCCCDis @0x56e160 / ModDat_GetCCCHt @0x56e180: the creation camera's distance and height for a
    // new-style model ([code] CCCDis / CCCHt; 2.5 and -1.0 when there is none).
    std::unordered_map<std::string, float> ccc_distances, ccc_heights;
    float ccc_distance(std::string_view code) const;
    float ccc_height(std::string_view code) const;
    // ModDat_GetPESFactor @0x56dfc0: [code] PESFactor, 1.0 when there is none. CreateActorDefinition keeps it in the
    // ActorAnimation's +0x38 and the actor's particle scale (vtable +0x214 / +0x218) takes it: spell emitters on the
    // actor scale by it.
    std::unordered_map<std::string, float> pes_factors;
    float pes_factor(std::string_view code) const;
};
// racedata.txt (RaceManager_LoadFile @0x7f47d0: lines starting with a digit, '^'-separated; columns 89 / 90 are
// the male / female heights, atof, a value below 1.0 becomes 6.0) and Resources/moddat.ini.
HeightTables parse_height_tables(std::string_view racedata, std::string_view moddat);

inline constexpr float kDefaultROffset = 3.125f; // 0x9d6820
inline constexpr float kDefaultCccDistance = 2.5f; // ModDat_GetCCCDis's default (0x40200000)
inline constexpr float kDefaultCccHeight = -1.0f;  // ModDat_GetCCCHt's (0xbf800000)
inline constexpr float kDefaultHeight = 6.0f;    // 0x9c7da0

// NpcManager_IsNewStyleModelRace @0x50a540: the race's model record has flag 0x08 (197, 374, 403, 405-414,
// 417-442, 445-501, 503-599, 601-645, 647-732).
bool is_new_style_model_race(std::int32_t race);
// NpcManager_GetModelCode: the model code ROffset is read for (HUM / HUF for a race without a model).
std::string_view height_model_code(std::int32_t race, std::uint8_t gender);
// The inputs for a spawn of this race and gender (`tables` null: no RaceManager, the default ROffset).
HeightInputs height_inputs(const HeightTables* tables, std::int32_t race, std::uint8_t gender, bool old_model_scale);

// PlayerZoneClient::ComputeSpecialHeights @0x59efd0: the per-race overrides of (scale, eye, avatar height,
// length).
void compute_special_heights(std::int32_t race, float& scale, float& eye, float& avatar_height, float& length,
                             const Fpu& fpu = Fpu{});

// EQPlayer::SetHeights(viewOffset, animScale) @0x5a3f40, from s.height (a Height not above 0 takes racedata's
// default). Returns whether it asked RaceManager_GetDefaultHeight. (The local player's race-72 state check, the
// sneak modifier and the innates are not heights and are not here.)
bool set_heights(SpawnHeights& s, const HeightInputs& in, float view_offset, float anim_scale, const Fpu& fpu = Fpu{});

// One spawn of ChangeHeight's mount chain.
struct HeightLink {
    SpawnHeights* heights = nullptr;
    HeightInputs inputs;
    std::uint8_t type = 0;   // PlayerBase::Type +0x125 (1 NPC)
    bool has_actor = false;  // PlayerClient +0x101c
    bool mount_race = false; // PlayerBase_IsMountRace
};
// What ChangeHeight gives a spawn's actor: its scale (0x40c090, vf78(scale, 1)) and, with an actor, the
// collision-volume radius (vf48, 0x5062a0 stored as a float).
struct HeightActorUpdate {
    std::size_t spawn = 0; // index into the chain
    float scale = 0.0f;
    std::optional<float> volume_radius;
};
// PlayerZoneClient::ChangeHeight @0x5a4ec0 (OP_ChangeSize): Height = size, SetHeights(viewOffset, animScale), the
// actor's scale and volume radius; then each further spawn of the mount chain the same with SetHeights(0, 1.0).
std::vector<HeightActorUpdate> change_height(std::span<HeightLink> chain, float size, float view_offset,
                                             float anim_scale, const Fpu& fpu = Fpu{});

// PlayerClient_CalcScaleFactorForHeight @0x59ef50: SetHeights' model scale for `height`; unrounded.
double calc_scale_factor_for_height(float height, bool new_style_model, bool old_model_scale, const Fpu& fpu = Fpu{});

// PlayerClient_GetEyeHeight @0x58cf00 (vfd0): the eye above the spawn's z; unrounded.
struct EyeInputs {
    bool uses_new_style_model = false; // PlayerClient_IsUsingNewStyleModel (ActorClient vf8)
    float definition_eye = 0.0f;       // the actor definition's (+0x1024) +0x48
    float model_scale = 0.0f;          // +0x31c
    float camera_offset = 0.0f;        // +0x274
    bool mounted = false;              // EQPlayer::GetMount != 0
    std::int32_t race = 0;             // GetRace, asked when mounted
    float view_height = 0.0f;          // +0x190
    float avatar_height = 0.0f;        // +0x138
};
double eye_height(const EyeInputs& in, const Fpu& fpu = Fpu{});
// The eye above the spawn's z for a classic-model spawn on foot: ViewHeight - AvatarHeight.
float eye_height(const SpawnHeights& s, std::int32_t race, bool mounted = false);

// CreatePlayerActor's height fix-up (0x4903e0..0x490542): for an actor whose definition has a tunic point
// (+0x34), these exact sizes are changed (ChangeHeight(new, 0, 1.0)) and the ViewHeight from before is put back:
// Half Elf 5.5 -> 5.3, Troll 8 -> 7.5, Ogre 9 -> 8.8, Iksar 6 -> 6.5, Froglok 6 -> 5.0.
std::optional<float> actor_height_fixup(std::int32_t race, float height);

// The EQG model's root drop (EQGraphicsDX9 0x1003cd55): the ROOT_BONE keys lowered by moddat.ini's ROffset of the
// model code, in model units; times the model scale in the world. ComputeSpecialHeights does not change it.
inline float eqg_root_drop(const HeightTables* tables, std::string_view code, float model_scale) {
    return (tables ? tables->roffset(code) : kDefaultROffset) * model_scale;
}

// CreatePlayerActor's in-world test (0x48f8e1..0x48fad2): the scene graph's vf4c at the position, then with z
// raised by 2.0 (each step stored as a float) up to four times; when all fail, the zone's safe point
// (0xddfb3c..44). x and y are not moved.
template <typename InWorld>
std::array<float, 3> place_in_world(std::array<float, 3> p, const std::array<float, 3>& safe_point, InWorld&& in_world) {
    if (in_world(p)) return p;
    for (int i = 0; i < 4; ++i) {
        p[2] = static_cast<float>(static_cast<double>(p[2]) + 2.0); // exact in either precision
        if (in_world(p)) return p;
    }
    return safe_point;
}

} // namespace hailstone::game
