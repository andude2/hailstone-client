#pragma once

// What a sound is played with: the original's SoundControl (SoundControl::SoundControl @0x5bead0), and
// the distance rule of its 3D voices. Shared by the zone emitters, the event sounds and the mixer.

#include <algorithm>
#include <cstdint>

#include <glm/glm.hpp>

namespace hailstone::audio {

struct SoundControl {
    float volume = 1.0f;          // +0x00
    std::int32_t fade_ms = 0;     // +0x04: ramp from silence to volume over this long
    std::int32_t loops = 1;       // +0x08: plays; 0 = forever
    glm::vec3 position{0.0f};     // +0x10..+0x18 (zone file coordinates)
    float min_distance = 0.0f;    // +0x1c: full volume inside
    float max_distance = 0.0f;    // +0x20: silent beyond; both 0 = a 2D sound
    float echo = 1.0f;            // +0x24: reverb send (the 3D provider's sample effects level)
    std::int32_t pool = 0;        // +0x28: the SoundManager instance pool it takes a voice from (kVoicePools)
    bool positional() const { return min_distance != 0.0f || max_distance != 0.0f; }
};

// What the "Miles Fast 2D Positional Audio" provider the original opens (msssoft.m3d of MSS 6.5b, its sample update
// @0x22401a50, read for this client; SoundManager_InitMilesDrivers @0x5c02c0) makes of a 3D sample: the two channel
// levels it gives AIL_set_sample_volume_levels, which mss32 mixes as linear amplitudes. `offset` is the sample's
// position minus the listener's, `face` and `right` the listener's unit facing and right vectors (Miles' right is
// up x face; the original's up is (0, 0, -1)):
//   - d = |offset|. Beyond max_distance (strictly) the sample is muted while it plays on (the provider's "Mute at
//     maximum" preference, on by default; the original never changes it): AIL_set_sample_volume_pan(0, 0.5).
//   - the gain is volume^(5/3) (applied by the mixer to the voice's ramped volume, `miles_volume_curve`), times
//     min / (min + (d - min) x rolloff) once d is past min + 0.0001 (min at least 0.0001; the rolloff factor stays
//     1, so this is min / d), times 0.75 when the sample is behind the listener (dot(face, dir) < 0);
//   - the pan p = acos(dot(right, dir)) / pi (0 right .. 1 left; 0.5 within 0.0001 of the listener, direction (1, 0, 0)
//     at distance 0), and the levels are left = p x gain, right = (1 - p) x gain: linear, so a sound straight ahead
//     is at half the gain on each side.
// Velocities stay 0 in the original (AIL_set_3D_velocity_vector(0, 0, 0) on every start), so there is no Doppler.
// The reverb send is the sample's effects level as it is (the obstruction and exclusion are never set).
struct MilesLevels {
    float left = 0.0f, right = 0.0f;
    bool muted = false;
};
MilesLevels miles_fast2d_levels(const glm::vec3& offset, float min_distance, float max_distance, const glm::vec3& face,
                                const glm::vec3& right);

// The provider's volume curve: AIL_set_3D_sample_volume's level raised to 5/3.
float miles_volume_curve(float volume);

// What to multiply a SoundControl's volume by for a volume option of `percent` (0..100) to scale the sound's
// amplitude linearly, as the mixer's bus volumes do: a positional sound's volume goes through miles_volume_curve,
// so its factor is the level raised to 3/5.
float option_volume_factor(int percent, bool positional);

// The MSVC C runtime's rand() (LCG 214013 / 2531011, 15 bits), which the
// original's emitter timers and sound picks use.
class CrtRand {
public:
    explicit CrtRand(std::uint32_t seed = 1) : state_(seed) {}
    std::int32_t operator()() {
        state_ = state_ * 214013u + 2531011u;
        return static_cast<std::int32_t>((state_ >> 16) & 0x7fffu);
    }
    // RandomNumberLowHigh-style pick in [low, high] (both included).
    std::int32_t between(std::int32_t low, std::int32_t high) {
        if (high <= low) return low;
        return low + (*this)() % (high - low + 1);
    }

private:
    std::uint32_t state_;
};

} // namespace hailstone::audio

namespace hailstone::audio {

// PlayerClient::PlaySound @0x58b780: a spawn's sound at its position, Miles
// distances height/2 and 12 x height, 0.8 volume and pool 3 for the local
// player (or what it rides), else pool 0.
inline SoundControl spawn_sound_control(const glm::vec3& position, float height, bool local) {
    SoundControl c;
    c.position = position;
    c.min_distance = height * 0.5f;
    c.max_distance = c.min_distance * 24.0f;
    if (local) {
        c.volume *= 0.8f;
        c.pool = 3;
    }
    return c;
}

// How far a spawn's sounds reach: its one-shots (spawn_sound_control, 12 x height) and its loop (EqMobileEmitter's
// 24 x max(height/2, 2), AudioSystem::set_spawn_loop), past which both are muted. Hailstone's (app/sound_events.cpp):
// the walking of a spawn farther than this plus `margin` is not turned into sounds at all.
inline bool spawn_out_of_hearing(const glm::vec3& listener, const glm::vec3& position, float height, float margin) {
    const float reach = std::max(height * 0.5f * 24.0f, std::max(height * 0.5f, 2.0f) * 24.0f) + margin;
    const glm::vec3 d = position - listener;
    return !(glm::dot(d, d) <= reach * reach); // (NaN: out)
}

// The SoundManager's instance pools (SoundManager_InitMilesDrivers @0x5c02c0 with 16 3D and 6 2D voices, then
// EqSoundManager's AddPool calls @0x4ac0e5): voices per pool index for 3D (positional) and 2D sounds.
// SoundAssistPlay @0x5bfbd0 takes the first free instance of the control's pool and plays nothing when there is
// none (music borrows: a new instance when the pool is empty). Pool 0 is the general one, 3 the local player's
// (PlaySound), 4 the zone's wave emitters (EmitterAdd / CreateOldEmitter); 1 and 2 have no 3D voices.
struct VoicePool {
    std::int32_t voices_3d = 0, voices_2d = 0;
};
inline constexpr VoicePool kVoicePools[5] = {{16, 6}, {0, 1}, {0, 1}, {2, 1}, {4, 2}};
inline std::int32_t pool_voices(std::int32_t pool, bool positional) {
    if (pool < 0 || pool >= 5) return 0;
    return positional ? kVoicePools[pool].voices_3d : kVoicePools[pool].voices_2d;
}

// CDisplay::PlaySoundAtLocation @0x48b550 (doors and switches): distances 5 and 80.
inline SoundControl location_sound_control(const glm::vec3& position) {
    SoundControl c;
    c.position = position;
    c.min_distance = 5.0f;
    c.max_distance = 80.0f;
    return c;
}

} // namespace hailstone::audio
