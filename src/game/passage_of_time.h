#pragma once

// The timed blocks of CharacterZoneClient::DoPassageOfTime @0x45daf0 that
// change the character's pools, run by the connection every frame in a zone
// (docs/mechanics.md, Cadences), in the original's order:
//
// - more than 1000 ms since the spawn's +0x2d8 (0 is set to now first; @0x45ee13): for
//   a living player the endurance upkeep (ApplyEnduranceUpkeep @0x448bb0; the
//   discipline goes when the endurance runs out).
// - more than 6000 ms (unsigned) since the spawn's +0x24c: ProcessAffects
//   (game/mechanics/ticks.h: every buff's countdown and its per-tick HP, mana
//   and endurance effects; a buff whose duration was already 0 is removed),
//   then for a living player below its maximum HP the HP regen (GetHPRegen,
//   "You are bleeding to death!" when it says so, ModifyCurHP), then, dead
//   or alive, the endurance regen (GetEnduranceRegen onto profile +0x3390,
//   Cur_Endurance(1)) and the mana regen (ApplyManaRegenTick); +0x24c becomes
//   now (@0x45f25b..0x45f425).
//
// The server's own HP, mana and endurance updates still overwrite the values
// whenever they come, as in the original: the local tick only moves them on
// in between. The connection updates drunkenness before ProcessAffects and
// consciousness after HP regen. Not ported: the autosave counter and the 1 s block's
// movement rate, visibility and air supply (other modules or not needed).

#include <cstdint>

#include "game/mechanics/character_sheet.h"

namespace hailstone::game {

struct SpellState;

namespace mechanics {
struct GameTables;
}

// The pools the ticks change: the player's HP (the spawn's current and maximum, as the server last sent them and
// the client changed them since) and the profile's stored mana (+0x338c) and endurance (+0x3390).
struct TickPools {
    std::int64_t hp = 0, max_hp = 0;
    std::int32_t mana = 0, endurance = 0;
    bool dead = false; // the spawn's +0x125
    // Deliberate difference from RoF2 (set by the live connection): when true, ProcessAffects still counts down
    // buffs, removes expired ones and breaks mesmerize on a DoT tick, but leaves pools.hp to the server's
    // OP_HPUpdate instead of locally subtracting unmitigated DoT damage (which can drop a full-HP character
    // out-of-phase after a server heal or mitigation and leave them stuck below 100% when the server stops
    // sending OP_HPUpdate at full HP).
    bool skip_buff_hp_loss = false;
};

struct DrunkennessTick {
    std::int32_t value = 0;
    bool changed = false;
    bool remove_mount = false;
};

// UpdateDrunkenness @0x44af40: one point per buff tick. The mount threshold reads the raw Alcohol Tolerance
// skill, before any skill modifiers, and compares the value after the decrement.
DrunkennessTick drunkenness_tick(std::int32_t value, std::int32_t alcohol_tolerance, bool mounted);
// UpdateDrunkenness chooses first person at 70, then sets the projection to that camera's base FOV + value-70
// (the base truncated, the original's integer SetFOV argument). Deliberate difference: the widening, value-70 (0
// below 70), is added on top of the player's chosen FOV each frame instead of replacing it, so the Options or /fov
// setting comes back exactly, also when drunkenness is reset at once (death, bind) above 70.
float drunkenness_fov_offset(std::int32_t value);

// What the ticks ask of the connection.
class PassageHost {
public:
    virtual ~PassageHost() = default;
    // CEverQuest::dsp_chat(StringTable::getString(id), colour).
    virtual void chat(std::uint32_t eqstr, std::uint32_t colour) = 0;
    // CharacterZoneClient_RemoveAffect @0x439700 on the buff in `slot` (0 based): the slot is cleared, and with
    // `notify` OP_Buff tells the server. An empty slot is left alone.
    virtual void remove_buff(int slot, bool notify) = 0;
    virtual std::int32_t rand() = 0;  // the C runtime's rand(): 0..0x7fff
    virtual void stop_cast() = 0;     // CharacterZoneClient_StopCurrentSpellCast(1)
    virtual void end_stun() = 0;      // CharacterZoneClient_EndStun
    // ProcessAffects' SPA 0x71 case: without a mount, the "<name>`s_Mount" spawn is mounted (summon_mount_tick).
    virtual void summon_mount() = 0;
    virtual bool eye_present() { return false; }
    virtual void view_eye() {}
};

// The 6 s block. `sheet` is the character as now (its buffs from `spells`, its stored mana and endurance from
// `pools`); the ticks write the pools into it as they change them, so the Character's Cur_Mana / Cur_Endurance
// read the live values. Returns true when the block ran.
bool six_second_tick(SpellState& spells, mechanics::CharacterSheet& sheet, const mechanics::GameTables& tables,
                     const mechanics::SpellLookup& lookup, std::int64_t now_ms, TickPools& pools, PassageHost& host);

// The 1 s block (the endurance upkeep). Returns true when the block ran.
bool one_second_tick(SpellState& spells, mechanics::CharacterSheet& sheet, const mechanics::GameTables& tables,
                     const mechanics::SpellLookup& lookup, std::int64_t now_ms, TickPools& pools, PassageHost& host);

// Whether each block has something to do at `now_ms` (the caller builds the sheet only then).
bool six_second_tick_due(const SpellState& spells, std::int64_t now_ms);
bool one_second_tick_due(const SpellState& spells, std::int64_t now_ms);

inline constexpr std::uint32_t kStrBleedingToDeath = 0x318d; // 12685 "You are bleeding to death!"

} // namespace hailstone::game
