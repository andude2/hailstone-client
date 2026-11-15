#include "game/spell_effect_timers.h"

#include <algorithm>
#include <utility>

namespace hailstone::game {

void SpellEffectTimers::add(std::uint32_t caster, std::uint32_t target, std::uint32_t effect, std::uint32_t stage1_ms,
                            std::uint32_t stage1_length_ms, std::uint32_t stage2_length_ms, bool full,
                            std::uint64_t now_ms) {
    // The stage 2 start wraps as the original's 32-bit add does.
    Entry e{caster, target, effect, stage1_ms, stage1_ms + stage1_length_ms, stage2_length_ms, full, 0, now_ms};
    plays_.push_back({caster, target, effect, 0, static_cast<std::int32_t>(stage1_ms), full});
    entries_.push_back(e);
}

bool SpellEffectTimers::remove(std::uint32_t caster, std::uint32_t target, std::uint32_t effect) {
    return std::erase_if(entries_, [&](const Entry& e) {
               return e.caster == caster && e.target == target && e.effect == effect;
           }) > 0;
}

void SpellEffectTimers::update(std::uint64_t now_ms) {
    for (std::size_t i = 0; i < entries_.size();) {
        Entry& e = entries_[i];
        // A deliberate change from the original, whose 32-bit elapsed + since wraps: an entry whose stage 2 start
        // is near 2^32 (or that a wrap carries back under it) would then stay for good. Here elapsed stops at the
        // maximum, and an entry that reaches it is dropped below whatever its stage 2 start; the stages play as the
        // original's until then.
        const std::uint64_t since = now_ms > e.last ? std::min<std::uint64_t>(now_ms - e.last, 0xffffffffu) : 0;
        const std::uint64_t sum = static_cast<std::uint64_t>(e.elapsed) + since;
        const std::uint32_t elapsed = sum < 0xffffffffu ? static_cast<std::uint32_t>(sum) : 0xffffffffu;
        if (e.elapsed < e.stage1 && elapsed > e.stage1)
            plays_.push_back({e.caster, e.target, e.effect, 1, static_cast<std::int32_t>(e.stage2 - e.stage1), e.full});
        if (e.elapsed < e.stage2 && elapsed > e.stage2)
            plays_.push_back({e.caster, e.target, e.effect, 2, static_cast<std::int32_t>(e.stage2_length), e.full});
        e.elapsed = elapsed;
        e.last = now_ms;
        if (e.elapsed > e.stage2 || e.elapsed == 0xffffffffu) entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
}

std::vector<SpellEffectStagePlay> SpellEffectTimers::take() { return std::exchange(plays_, {}); }

void SpellEffectTimers::clear() {
    entries_.clear();
    plays_.clear();
}

} // namespace hailstone::game
