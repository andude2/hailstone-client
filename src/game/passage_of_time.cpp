#include "game/passage_of_time.h"

#include <algorithm>
#include <array>
#include <climits>
#include <map>

#include "core/wrap_math.h"
#include "game/mechanics/character.h"
#include "game/mechanics/ticks.h"
#include "game/spells.h"

namespace hailstone::game {

namespace {

using mechanics::AffectBuff;
using mechanics::AffectSpell;
using mechanics::Character;

constexpr std::int64_t kSixSeconds = 6000; // @0x45f26e: at most 6000 (unsigned)
constexpr std::int64_t kOneSecond = 1000;
constexpr std::uint32_t kColourSpellFading = 0x121; // ProcessAffects' dsp_chat colour
constexpr std::uint32_t kColourBleeding = 0xd;      // @0x45f38c

std::int32_t wrap_add(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}

bool elapsed_over(std::int64_t now, std::int64_t last, std::int64_t limit) {
    // The original compares the unsigned difference of two __EQGetTime values.
    return static_cast<std::uint32_t>(now - last) > static_cast<std::uint32_t>(limit);
}

// CharacterZoneClient::ModifyCurHP @0x4537d0 on the player (never an NPC spawn): the HP plus the change, clamped to
// -0x20000..10000000; the spawn's Cur_HP read afterwards keeps it at most the maximum.
void modify_cur_hp(TickPools& pools, std::int32_t delta) {
    if (delta == 0) return;
    std::int64_t hp = std::clamp<std::int64_t>(pools.hp + delta, -0x20000, 10000000);
    if (pools.max_hp > 0 && hp > pools.max_hp) hp = pools.max_hp;
    pools.hp = hp;
}

// ProcessAffects' callees over the client's state.
class LiveAffects final : public mechanics::AffectQueries {
public:
    LiveAffects(SpellState& spells, mechanics::CharacterSheet& sheet, Character& ch, mechanics::AffectState& st,
                TickPools& pools, PassageHost& host)
        : spells_(spells), sheet_(sheet), ch_(ch), st_(st), pools_(pools), host_(host) {
        count_ = std::min(ch.max_effects(), kBuffSlots);
        for (int i = 0; i < count_; ++i) {
            const auto k = static_cast<std::size_t>(i);
            const BuffSlot& b = spells.buffs[k];
            if (b.empty()) continue;
            AffectBuff& a = buffs_[k];
            a.type = b.effect_type ? b.effect_type : 2;
            a.caster_level = k < sheet.buffs.size() ? sheet.buffs[k].level : b.level;
            a.spell_id = static_cast<std::int32_t>(b.spell);
            // The duration as the server last sent it, counted down by these ticks since (spawn +0x24c is the
            // character's own timer, not the packet's).
            a.duration = b.tics;
        }
    }

    // The countdowns back into the buff slots (a removed buff's slot is already clear).
    void write_back(std::int64_t now_ms) {
        for (int i = 0; i < count_; ++i) {
            BuffSlot& b = spells_.buffs[static_cast<std::size_t>(i)];
            if (b.empty()) continue;
            b.tics = buffs_[static_cast<std::size_t>(i)].duration;
            b.received_ms = now_ms;
        }
    }

    int max_effects() override { return count_; }
    AffectBuff& effect(int index) override { return buffs_[static_cast<std::size_t>(index)]; }
    bool valid_spell(std::int32_t id) override { return ch_.valid_spell(id); }
    const AffectSpell* spell(std::int32_t id) override {
        auto it = spells_seen_.find(id);
        if (it != spells_seen_.end()) return it->second.id ? &it->second : nullptr;
        AffectSpell& a = spells_seen_[id];
        const SpellData* d = ch_.spell(id);
        if (!d) return nullptr;
        a.id = id;
        a.duration_formula = static_cast<std::int32_t>(d->duration_formula);
        a.float_21c = d->distance_mod_start_x;
        a.float_224 = d->distance_mod_end_x;
        a.drain_removes = d->spell_type; // +0x26b is the record's SpellType
        a.uninterruptible = d->uninterruptable;
        for (std::size_t s = 0; s < 12; ++s) {
            a.spa[s] = d->effect[s];
            a.base[s] = d->base[s];
            a.base2[s] = d->base2[s];
        }
        return &a;
    }
    // CacheBuffSlotEffects for a distance-modified buff: the Character builds its cache again on the next read.
    void cache_buff_slot_effects(int) override { ch_.invalidate_effects(); }
    bool countdown_held(const AffectSpell* spell) override {
        return buff_countdown_held(spell ? ch_.spell(spell->id) : nullptr, st_.hold_flag != 0,
                                   spells_.zone_suspends_buffs);
    }
    // GetPCSpellAffect @0x433fd0: a buff (type not 0) whose spell has the SPA before its first blank slot.
    bool has_affect(std::int32_t spa) override {
        for (int i = 0; i < count_; ++i) {
            const AffectBuff& b = buffs_[static_cast<std::size_t>(i)];
            if (b.type == 0) continue;
            if (const SpellData* d = ch_.spell(b.spell_id); d && d->has_effect_before_blank(spa)) return true;
        }
        return false;
    }
    // SpellCache_IsTickVarying only clears the spell cache's +0x1a4, which this client does not keep.
    bool tick_varying(std::int32_t) override { return false; }
    // CalcAffectChange(spell, level, slot, buff, index, 0, 0, -1, 1): CalcAffectChangeGeneric with the slot's base,
    // scaled by the distance of the slot's caster (BuffCasterIDs[index]; the far modifier when it is not found).
    std::int32_t affect_change(const AffectSpell& spell, const AffectBuff& buff, int slot, int index) override {
        const SpellData* d = ch_.spell(spell.id);
        const mechanics::SheetBuff* sb = sheet_buff(index);
        if (!d) return 0;
        std::int32_t v = ch_.calc_affect_change(*d, buff.caster_level, slot, sb, d->base[static_cast<std::size_t>(slot)],
                                                true);
        return ch_.scale_by_distance(*d, v, sb ? sb->caster_distance : std::nullopt);
    }
    // EQ_Character::Cur_Endurance(1) / Cur_Mana(1): the stored pool clamped (and stored back).
    std::int32_t cur_endurance() override {
        sheet_.endurance = st_.endurance;
        std::int32_t stored = st_.endurance;
        std::int32_t v = ch_.cur_endurance(true, &stored);
        st_.endurance = sheet_.endurance = stored;
        return v;
    }
    std::int32_t cur_mana() override {
        sheet_.mana = st_.mana;
        std::int32_t stored = st_.mana;
        std::int32_t v = ch_.cur_mana(true, &stored);
        st_.mana = sheet_.mana = stored;
        return v;
    }
    void remove_affect(AffectBuff& buff, int notify, int) override {
        const auto index = &buff - buffs_.data();
        if (index < 0 || index >= count_) return;
        buff.type = 0;
        host_.remove_buff(static_cast<int>(index), notify != 0);
        if (static_cast<std::size_t>(index) < sheet_.buffs.size()) sheet_.buffs[static_cast<std::size_t>(index)] = {};
        ch_.invalidate_effects();
    }
    // The connection runs the tick only in a zone with the player's spawn.
    bool spawn_present() override { return true; }
    // SPA 0x71: summon_mount_tick does GetMount, the "<name>`s_Mount" lookup and the ride itself.
    bool has_mount() override { return false; }
    bool find_summon(bool eye) override {
        return eye ? host_.eye_present() : true;
    }
    void ride_mount() override { host_.summon_mount(); }
    void view_eye() override { host_.view_eye(); }
    std::int32_t spell_level_needed(const AffectSpell* spell, int class_id) override {
        const SpellData* d = spell ? ch_.spell(spell->id) : nullptr;
        return d ? d->level_needed(static_cast<std::uint8_t>(class_id)) : 0xff;
    }
    void chat(std::uint32_t string_id) override { host_.chat(string_id, kColourSpellFading); }
    std::int32_t rand() override { return host_.rand(); }
    void stop_cast() override { host_.stop_cast(); }
    // The spell manager's +8 (SpellReqAssociationID): the client has no requirement data, every tick applies
    // (docs/unknowns.md; CastSpell's check answers the same).
    bool hp_tick_allowed(std::int32_t) override { return true; }
    // CalcTickHPChange @0x436a60 (from the disassembly).
    std::int32_t tick_hp_change(int index, const AffectBuff& buff, int slot, const AffectSpell& spell) override {
        const SpellData* d = ch_.spell(spell.id);
        const mechanics::SheetBuff* sb = sheet_buff(index);
        if (!d) return 0;
        // CalcAffectChange(spell, level, slot, buff, -1, GetSpawnByID(caster), 0, -1, 1): with no caster spawn
        // found the value is not scaled by distance at all.
        std::int32_t v = ch_.calc_affect_change(*d, buff.caster_level, slot, sb, d->base[static_cast<std::size_t>(slot)],
                                                true);
        std::optional<float> distance = sb ? sb->caster_distance : std::nullopt;
        if (!distance && sb && sb->caster_is_self) distance = 0.0f; // the player's own spawn
        const bool caster_found = distance.has_value();
        if (caster_found) v = ch_.scale_by_distance(*d, v, distance);
        // A modifier not exactly 1.0 (NaN included) scales: v * modifier, truncated.
        const float modifier = sb ? sb->modifier : 1.0f;
        if (!(modifier == 1.0f))
            v = mechanics::Fpu::ftol(ch_.fpu().mul(static_cast<double>(v), static_cast<double>(modifier)));
        if (buff.type == 4) v = static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(v));
        // A player caster (type 0, the same reading as CacheBuffSlotEffects') still standing, on a living player:
        // 75% of a loss when the caster is the player, else 70%, at least 1.
        if (caster_found && sb->caster_is_pc && v < 0 && !pools_.dead) {
            v = core::wrapped_mul_div(v, sb->caster_is_self ? 0x4b : 0x46, 100);
            if (v > -1) v = -1;
        }
        return v;
    }
    bool timers_on_hold() override { return st_.hold_flag != 0; }
    bool spell_has_spa(const AffectSpell& spell, std::int32_t spa) override {
        const SpellData* d = ch_.spell(spell.id);
        return d && d->has_effect(spa); // EQ_Spell::SpellAffects: all 12 slots
    }
    // 0x4326b0 only decides whether SPA 3 / 0x63 end with the spawn check, which always passes here.
    std::int32_t recheck(std::int32_t) override { return 0; }
    void end_stun() override { host_.end_stun(); }
    void modify_cur_hp(std::int32_t amount) override { game::modify_cur_hp(pools_, amount); }

private:
    const mechanics::SheetBuff* sheet_buff(int index) const {
        if (index < 0 || static_cast<std::size_t>(index) >= sheet_.buffs.size()) return nullptr;
        const mechanics::SheetBuff& b = sheet_.buffs[static_cast<std::size_t>(index)];
        return b.empty() ? nullptr : &b;
    }

    SpellState& spells_;
    mechanics::CharacterSheet& sheet_;
    Character& ch_;
    mechanics::AffectState& st_;
    TickPools& pools_;
    PassageHost& host_;
    int count_ = 0;
    std::array<AffectBuff, kBuffSlots> buffs_{};
    std::map<std::int32_t, AffectSpell> spells_seen_;
};

// Cur_Mana(1) / Cur_Endurance(1) on a stored value: the value clamped as the original stores it back.
std::int32_t clamped_mana(const Character& ch, mechanics::CharacterSheet& sheet, std::int32_t stored) {
    sheet.mana = stored;
    ch.cur_mana(true, &stored);
    sheet.mana = stored;
    return stored;
}
std::int32_t clamped_endurance(const Character& ch, mechanics::CharacterSheet& sheet, std::int32_t stored) {
    sheet.endurance = stored;
    ch.cur_endurance(true, &stored);
    sheet.endurance = stored;
    return stored;
}

} // namespace

DrunkennessTick drunkenness_tick(std::int32_t value, std::int32_t alcohol_tolerance, bool mounted) {
    DrunkennessTick out{value, false, false};
    if (value <= 0) return out;
    out.value = value - 1;
    out.changed = true;
    const std::int32_t threshold = std::min(150, alcohol_tolerance / 2 + 50);
    out.remove_mount = mounted && out.value >= threshold;
    return out;
}

float drunkenness_fov_offset(std::int32_t value) { return value < 70 ? 0.0f : static_cast<float>(value - 70); }

bool six_second_tick_due(const SpellState& spells, std::int64_t now_ms) {
    return elapsed_over(now_ms, spells.last_buff_tick_ms, kSixSeconds);
}

bool one_second_tick_due(const SpellState& spells, std::int64_t now_ms) {
    return spells.last_upkeep_tick_ms == 0 || elapsed_over(now_ms, spells.last_upkeep_tick_ms, kOneSecond);
}

bool six_second_tick(SpellState& spells, mechanics::CharacterSheet& sheet, const mechanics::GameTables& tables,
                     const mechanics::SpellLookup& lookup, std::int64_t now_ms, TickPools& pools, PassageHost& host) {
    if (!elapsed_over(now_ms, spells.last_buff_tick_ms, kSixSeconds)) return false;
    sheet.mana = pools.mana;
    sheet.endurance = pools.endurance;
    Character ch(sheet, tables, lookup);
    ch.process_all_stats(); // the maxima Cur_Mana / Cur_Endurance clamp to

    // CharacterZoneClient::ProcessAffects @0x43ca30.
    mechanics::AffectState st;
    st.class_id = sheet.class_id;
    st.mana = pools.mana;
    st.endurance = pools.endurance;
    st.spawn_type = 0; // the player
    st.casting_spell_id = spells.casting.casting() ? static_cast<std::int32_t>(spells.casting.spell) : -1;
    st.hold_flag = spells.buff_timers_on_hold ? 1 : 0;
    {
        LiveAffects q(spells, sheet, ch, st, pools, host);
        mechanics::process_affects(st, q);
        q.write_back(now_ms);
    }
    spells.buff_timers_on_hold = st.hold_flag != 0;
    pools.mana = st.mana;
    pools.endurance = st.endurance;

    // The rest reads the character after the buff tick: its buffs, pools and HP as they are now.
    sheet.buff_timers_on_hold = spells.buff_timers_on_hold;
    sheet.zone_suspends_buffs = spells.zone_suspends_buffs;
    sheet.mana = pools.mana;
    sheet.endurance = pools.endurance;
    sheet.stored_hp = sheet.spawn_hp = static_cast<std::int32_t>(std::clamp<std::int64_t>(pools.hp, INT32_MIN, INT32_MAX));
    Character after(sheet, tables, lookup);
    after.process_all_stats();
    // A living player below its maximum: the HP regen (GetHPRegen(1, &bleeding, 0)), its bleeding line, ModifyCurHP.
    if (!pools.dead && pools.hp < pools.max_hp) {
        bool bleeding = false;
        std::int32_t regen = after.hp_regen(true, &bleeding, false);
        if (bleeding) host.chat(kStrBleedingToDeath, kColourBleeding);
        modify_cur_hp(pools, regen);
    }
    // Dead or alive: the endurance regen onto the stored endurance and Cur_Endurance(1), then ApplyManaRegenTick
    // (its Cur_Mana(1) clamps the stored mana before it compares with the maximum).
    pools.endurance = clamped_endurance(after, sheet, wrap_add(pools.endurance, after.endurance_regen(true, false)));
    sheet.mana = pools.mana;
    std::int32_t stored_mana = pools.mana;
    const std::int32_t current = after.cur_mana(true, &stored_mana);
    stored_mana = mechanics::apply_mana_regen_tick(stored_mana, current, after.max_mana(true),
                                                   [&] { return after.mana_regen(true, false); });
    pools.mana = clamped_mana(after, sheet, stored_mana); // what the next Cur_Mana(1) read stores
    spells.last_buff_tick_ms = now_ms;
    return true;
}

bool one_second_tick(SpellState& spells, mechanics::CharacterSheet& sheet, const mechanics::GameTables& tables,
                     const mechanics::SpellLookup& lookup, std::int64_t now_ms, TickPools& pools, PassageHost& host) {
    if (spells.last_upkeep_tick_ms == 0) spells.last_upkeep_tick_ms = now_ms;
    if (!elapsed_over(now_ms, spells.last_upkeep_tick_ms, kOneSecond)) return false;
    spells.last_upkeep_tick_ms = now_ms;
    if (pools.dead) return true;
    sheet.mana = pools.mana;
    sheet.endurance = pools.endurance;
    Character ch(sheet, tables, lookup);
    ch.process_all_stats();
    // ApplyEnduranceUpkeep @0x448bb0: the cached upkeep (+0x94) off the stored endurance; below 0 it is 0 and the
    // active discipline (GetFirstEffectSlot(0, 1): the last slot) goes with RemovePCAffectex (notify 1).
    auto r = mechanics::apply_endurance_upkeep(
        pools.endurance, ch.effect_cache().endurance_upkeep,
        // The base's virtual +0x70 read as the stored endurance itself: Cur_Endurance would give 0 for a negative
        // value and the discipline could never be dropped (which slot of the vtable it is was not traced).
        [](std::int32_t stored) { return stored; },
        true);
    pools.endurance = clamped_endurance(ch, sheet, r.endurance);
    if (r.drop_discipline) host.remove_buff(ch.max_effects() - 1, true);
    return true;
}

} // namespace hailstone::game
