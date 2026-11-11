#pragma once

// Turns what happens in the zone into sounds (game/sound_rules.h picks the
// wave, audio/ plays it): the connection's sound cues (hits, level up, doors,
// casting), the spawns' animations (OP_Animation and their walking), and the
// local player's steps, jumps and landings.

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "assets/sound_files.h"
#include "audio/sound_control.h"
#include "game/sound_rules.h"
#include "game/world_state.h"

namespace hailstone {
class GameDir;
}
namespace hailstone::net {
class GameConnection;
}
namespace hailstone::audio {
class AudioSystem;
}

namespace hailstone::app {

class SoundEvents {
public:
    SoundEvents(audio::AudioSystem& audio, const GameDir& game);

    // What the local player's body did this frame (app/player_movement.h's Frame).
    struct LocalMotion {
        bool moving = false, running = true, wet = false, flying = false, sneaking = false;
        bool jumped = false, land_sound = false;
        std::optional<std::int32_t> fall_damage;
    };

    // Once a frame in the zone, after the connection's update.
    void update(net::GameConnection& conn, const LocalMotion& me, std::uint32_t now_ms);
    void reset(); // zone change

private:
    struct SpawnState {
        std::uint32_t action_seq = 0;
        bool seq_seen = false;
        game::AnimationSoundState anim;
        std::int32_t loop = 0;
        glm::vec3 last_position{0.0f};
        double last_time = 0.0;
    };
    // `volume_percent`: a volume option on top of the sound's own (audio::option_volume_factor).
    void play_at(const game::Spawn& s, std::int32_t wave, bool local, int volume_percent = 100);
    void animation(const game::Spawn& s, std::int32_t action, bool local, const LocalMotion* me, std::uint32_t now_ms,
                   SpawnState& st);

    audio::AudioSystem& audio_;
    std::vector<assets::SpellEffectSounds> spell_effects_;           // SpellsNew.eff
    std::vector<std::array<std::int32_t, 3>> old_effects_;           // spells.eff's stage sounds
    std::unordered_map<std::uint32_t, SpawnState> spawns_;
    std::vector<std::uint32_t> alive_; // the spawns with a loop this frame (kept for its capacity)
    audio::CrtRand rand_{static_cast<std::uint32_t>(0x5eed)};
};

} // namespace hailstone::app
