// The Ability Book (game/ability_book.h): its keys, the autoskill list's ini text, Autoskill's choice, and the
// server's switch for it (hailstone.cfg [abilities] autoskill).
#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <set>

#include "game/ability_book.h"
#include "game/mechanics/abilities.h"
#include "plugin/policy.h"

using namespace hailstone;
using namespace hailstone::game;

TEST_CASE("The autoskill list's rules move by any step, stopping at the ends") {
    std::vector<AutoskillRule> rules = parse_autoskill_rules("skill:1,skill:2,skill:3");
    auto keys = [&] {
        std::string s;
        for (const auto& r : rules) s += (s.empty() ? "" : ",") + r.key;
        return s;
    };
    CHECK(move_autoskill_rule(rules, "skill:1", 1));
    CHECK(keys() == "skill:2,skill:1,skill:3");
    // Steps near the int64 range end at the list's ends, not in an overflow.
    CHECK(move_autoskill_rule(rules, "skill:1", std::numeric_limits<std::int64_t>::max()));
    CHECK(keys() == "skill:2,skill:3,skill:1");
    CHECK(move_autoskill_rule(rules, "skill:1", std::numeric_limits<std::int64_t>::min()));
    CHECK(keys() == "skill:1,skill:2,skill:3");
    CHECK_FALSE(move_autoskill_rule(rules, "skill:1", -1)); // at the top already
    CHECK_FALSE(move_autoskill_rule(rules, "skill:9", 1));
    std::vector<AutoskillRule> one = parse_autoskill_rules("skill:1");
    CHECK_FALSE(move_autoskill_rule(one, "skill:1", std::numeric_limits<std::int64_t>::min()));
}

TEST_CASE("ability book: keys") {
    CHECK(ability_key(AbilityKind::AltAbility, 1234) == "aa:1234");
    CHECK(ability_key(AbilityKind::Skill, 10) == "skill:10");
    CHECK(discipline_key(77, 4500) == "disc:77");
    CHECK(discipline_key(0, 4500) == "spell:4500");
    auto p = parse_ability_key("spell:4500");
    REQUIRE(p);
    CHECK(p->kind == AbilityKind::Discipline);
    CHECK(p->spell_id);
    CHECK(p->id == 4500);
    CHECK(parse_ability_key("disc:77")->spell_id == false);
    CHECK_FALSE(parse_ability_key("item:5"));
    CHECK_FALSE(parse_ability_key("aa:"));
    CHECK_FALSE(parse_ability_key("aa:12x"));
    CHECK_FALSE(parse_ability_key("skill"));
}

TEST_CASE("ability book: skill descriptions") {
    CHECK_FALSE(skill_description(mechanics::skill::kBash).empty());
    CHECK_FALSE(skill_description(mechanics::skill::kTaunt).empty());
    CHECK(skill_description(0).empty()); // 1H Blunt: not activated
}

TEST_CASE("ability book: the autoskill list's text") {
    std::vector<AutoskillRule> rules{{"skill:10"}, {"aa:15", 30, 0, 50}, {"disc:77", 0, 20, 0}};
    const std::string text = format_autoskill_rules(rules);
    CHECK(text == "skill:10,aa:15;30;0;50,disc:77;0;20;0");
    auto back = parse_autoskill_rules(text);
    REQUIRE(back.size() == 3);
    CHECK(back[1].key == "aa:15");
    CHECK(back[1].min_endurance_pct == 30);
    CHECK(back[1].target_hp_below == 50);
    CHECK(back[2].min_mana_pct == 20);
    // unknown keys and repeats are dropped, percents clamped, spaces allowed
    auto odd = parse_autoskill_rules(" skill:10 , bogus:1, skill:10;5, aa:3;150;-4;x ,");
    REQUIRE(odd.size() == 2);
    CHECK(odd[0].min_endurance_pct == 0);
    CHECK(odd[1].key == "aa:3");
    CHECK(odd[1].min_endurance_pct == 100);
    CHECK(odd[1].min_mana_pct == 0);
    CHECK(odd[1].target_hp_below == 0);
    CHECK(parse_autoskill_rules("").empty());
}

TEST_CASE("ability book: the autoskill list's text with when, buff and health") {
    AutoskillRule r{"aa:15"};
    r.self_hp_below = 40;
    r.target_hp_above = 10;
    r.when = AutoskillWhen::Resting;
    r.keep_buff = true;
    const std::string text = format_autoskill_rules({r, {"skill:30", 0, 0, 20}});
    CHECK(text == "aa:15;0;0;0;10;40;2;1,skill:30;0;0;20");
    auto back = parse_autoskill_rules(text);
    REQUIRE(back.size() == 2);
    CHECK(back[0].target_hp_above == 10);
    CHECK(back[0].self_hp_below == 40);
    CHECK(back[0].when == AutoskillWhen::Resting);
    CHECK(back[0].keep_buff);
    CHECK(back[1].when == AutoskillWhen::Fighting);
    CHECK_FALSE(back[1].keep_buff);
    // out of range: when and buff clamped
    auto odd = parse_autoskill_rules("aa:3;0;0;0;0;0;9;5");
    REQUIRE(odd.size() == 1);
    CHECK(odd[0].when == AutoskillWhen::Resting);
    CHECK(odd[0].keep_buff);
}

namespace {

AutoskillWorld fighting(std::uint32_t now) {
    AutoskillWorld w;
    w.now_ms = now;
    w.in_game = true;
    w.auto_attack = true;
    w.target_npc = true;
    w.target_in_range = true;
    return w;
}

// A query over sets of ready, beneficial and buffed keys; keys in none of them cannot be used.
struct Abilities {
    std::set<std::string> ready, beneficial, buffed, known;
    AutoskillQuery query() {
        return [this](const std::string& k) -> std::optional<AutoskillAbility> {
            if (!known.contains(k) && !ready.contains(k)) return std::nullopt;
            return AutoskillAbility{ready.contains(k), beneficial.contains(k), buffed.contains(k)};
        };
    }
};

} // namespace

TEST_CASE("ability book: autoskill acts only while fighting") {
    AutoskillWorld w = fighting(1000);
    CHECK(autoskill_can_act(w));
    auto without = [&](auto change) {
        AutoskillWorld c = w;
        change(c);
        return autoskill_can_act(c);
    };
    CHECK_FALSE(without([](auto& c) { c.auto_attack = false; }));
    CHECK_FALSE(without([](auto& c) { c.target_npc = false; }));
    CHECK_FALSE(without([](auto& c) { c.target_in_range = false; }));
    CHECK_FALSE(without([](auto& c) { c.sitting = true; }));
    CHECK_FALSE(without([](auto& c) { c.feigned = true; }));
    CHECK_FALSE(without([](auto& c) { c.stunned = true; }));
    CHECK_FALSE(without([](auto& c) { c.casting = true; }));
    CHECK_FALSE(without([](auto& c) { c.invisible = true; }));
    CHECK_FALSE(without([](auto& c) { c.dead = true; }));
    CHECK_FALSE(without([](auto& c) { c.in_game = false; }));
}

TEST_CASE("ability book: autoskill's order, conditions, gap and retry") {
    std::vector<AutoskillRule> rules{{"skill:30"}, {"skill:10", 50, 0, 0}, {"aa:7", 0, 0, 20}};
    Abilities ab;
    ab.ready = {"skill:30", "skill:10", "aa:7"};
    ab.known = ab.ready;
    auto& ready = ab.ready;
    const AutoskillQuery is_ready = ab.query();
    Autoskill a;
    AutoskillWorld w = fighting(10000);

    // the first ready one in order
    auto i = a.next(rules, is_ready, w);
    REQUIRE(i);
    CHECK(*i == 0);
    a.used(rules[*i].key, w.now_ms);
    // nothing within the gap
    w.now_ms += Autoskill::kGapMs - 1;
    CHECK_FALSE(a.next(rules, is_ready, w));
    // after the gap: kick was just tried (retry wait), bash next
    w.now_ms += 1;
    i = a.next(rules, is_ready, w);
    REQUIRE(i);
    CHECK(*i == 1);
    // bash needs 50% endurance; the AA a target below 20%
    w.endurance_pct = 40;
    w.target_hp_pct = 50;
    CHECK_FALSE(a.next(rules, is_ready, w));
    w.target_hp_pct = 19;
    i = a.next(rules, is_ready, w);
    REQUIRE(i);
    CHECK(*i == 2);
    // not ready: skipped
    ready.erase("aa:7");
    CHECK_FALSE(a.next(rules, is_ready, w));
    // kick again once its retry wait is over
    w.now_ms = 10000 + Autoskill::kRetryMs;
    i = a.next(rules, is_ready, w);
    REQUIRE(i);
    CHECK(*i == 0);
    // not fighting: nothing
    w.auto_attack = false;
    CHECK_FALSE(a.next(rules, is_ready, w));
}

TEST_CASE("ability book: a use that keeps failing backs off") {
    std::vector<AutoskillRule> rules{{"skill:10"}};
    bool ready = true;
    const AutoskillQuery is_ready = [&](const std::string&) { return AutoskillAbility{ready}; };
    Autoskill a;
    std::uint32_t now = 1000;
    AutoskillWorld w = fighting(now);
    // still ready after each try (no shield): 2 s, 4 s, 8 s, 16 s, then 32 s at most
    std::uint32_t waits[] = {2000, 4000, 8000, 16000, 32000, 32000};
    REQUIRE(a.next(rules, is_ready, w));
    a.used("skill:10", w.now_ms);
    for (std::uint32_t wait : waits) {
        w.now_ms += wait - 1;
        CHECK_FALSE(a.next(rules, is_ready, w));
        w.now_ms += 1;
        REQUIRE(a.next(rules, is_ready, w));
        a.used("skill:10", w.now_ms);
    }
    // seen on its timer (the use went through): the wait is back to 2 s
    ready = false;
    w.now_ms += 32000;
    CHECK_FALSE(a.next(rules, is_ready, w));
    ready = true;
    REQUIRE(a.next(rules, is_ready, w));
    a.used("skill:10", w.now_ms);
    w.now_ms += Autoskill::kRetryMs;
    CHECK(a.next(rules, is_ready, w));
}

TEST_CASE("ability book: the server's autoskill switch") {
    std::vector<std::string> warnings;
    CHECK(plugin::parse_server_cfg("", &warnings).autoskill);
    CHECK_FALSE(plugin::parse_server_cfg("[abilities]\nautoskill = off\n", &warnings).autoskill);
    CHECK(warnings.empty());
    CHECK(plugin::parse_server_cfg("[abilities]\nautoskill = on\n", &warnings).autoskill);
    plugin::parse_server_cfg("[abilities]\nautoskill = maybe\n", &warnings);
    CHECK(warnings.size() == 1);
}

TEST_CASE("ability book: autoskill's when: any time and out of combat only for beneficial abilities") {
    AutoskillRule buff{"aa:20"};
    buff.when = AutoskillWhen::Always;
    AutoskillRule rest{"skill:27"};
    rest.when = AutoskillWhen::Resting;
    AutoskillRule harm{"aa:30"};
    harm.when = AutoskillWhen::Always; // not beneficial: kept to a fight
    std::vector<AutoskillRule> rules{harm, buff, rest};
    Abilities ab;
    ab.ready = {"aa:20", "skill:27", "aa:30"};
    ab.beneficial = {"aa:20", "skill:27"};
    AutoskillWorld w;
    w.in_game = true;
    w.now_ms = 1000;
    {
        Autoskill a;
        auto i = a.next(rules, ab.query(), w); // not fighting: the harmful one waits
        REQUIRE(i);
        CHECK(*i == 1);
    }
    {
        Autoskill a;
        ab.ready.erase("aa:20");
        auto i = a.next(rules, ab.query(), w); // out of combat: the resting rule
        REQUIRE(i);
        CHECK(*i == 2);
        w.in_combat = true; // the server's combat flag
        CHECK_FALSE(a.next(rules, ab.query(), w));
        w.in_combat = false;
        w.auto_attack = true;
        CHECK_FALSE(a.next(rules, ab.query(), w));
        w.target_npc = w.target_in_range = true; // fighting: the harmful one, first in the list
        i = a.next(rules, ab.query(), w);
        REQUIRE(i);
        CHECK(*i == 0);
    }
    // the player must still be free to act
    w = AutoskillWorld{};
    w.in_game = true;
    w.sitting = true;
    Autoskill a;
    CHECK_FALSE(a.next(rules, ab.query(), w));
    CHECK(autoskill_when_holds(AutoskillWhen::Always, true, fighting(0)));
    CHECK_FALSE(autoskill_when_holds(AutoskillWhen::Resting, true, fighting(0)));
}

TEST_CASE("ability book: autoskill keeps a buff up: once each time it wears off") {
    AutoskillRule r{"disc:5"};
    r.when = AutoskillWhen::Always;
    r.keep_buff = true;
    std::vector<AutoskillRule> rules{r};
    Abilities ab;
    ab.ready = {"disc:5"};
    ab.beneficial = {"disc:5"};
    ab.known = {"disc:5"};
    AutoskillWorld w;
    w.in_game = true;
    w.now_ms = 1000;
    Autoskill a;
    REQUIRE(a.next(rules, ab.query(), w));
    a.used("disc:5", w.now_ms);
    // the buff landed: not again while it is on, though the ability is ready
    ab.buffed = {"disc:5"};
    for (int k = 0; k < 10; ++k) {
        w.now_ms += 10000;
        CHECK_FALSE(a.next(rules, ab.query(), w));
    }
    // worn off: once more, with no back-off left over
    ab.buffed.clear();
    auto i = a.next(rules, ab.query(), w);
    REQUIRE(i);
    a.used("disc:5", w.now_ms);
    w.now_ms += Autoskill::kRetryMs;
    CHECK(a.next(rules, ab.query(), w)); // still off (it did not land): tried again after the retry wait
}

TEST_CASE("ability book: autoskill's health conditions") {
    AutoskillRule heal{"skill:32"};
    heal.self_hp_below = 50;
    AutoskillRule opener{"aa:9"};
    opener.target_hp_above = 90;
    std::vector<AutoskillRule> rules{heal, opener};
    Abilities ab;
    ab.ready = {"skill:32", "aa:9"};
    AutoskillWorld w = fighting(1000);
    Autoskill a;
    w.hp_pct = 80;
    w.target_hp_pct = 90; // not above 90
    CHECK_FALSE(a.next(rules, ab.query(), w));
    w.target_hp_pct = 95;
    auto i = a.next(rules, ab.query(), w);
    REQUIRE(i);
    CHECK(*i == 1);
    w.hp_pct = 49;
    i = a.next(rules, ab.query(), w);
    REQUIRE(i);
    CHECK(*i == 0);
    // a target condition outside a fight needs a living NPC target
    AutoskillRule any{"aa:11"};
    any.when = AutoskillWhen::Always;
    any.target_hp_below = 50;
    ab.ready.insert("aa:11");
    ab.beneficial.insert("aa:11");
    AutoskillWorld idle;
    idle.in_game = true;
    idle.now_ms = 1000;
    idle.target_hp_pct = 10;
    CHECK_FALSE(a.next({any}, ab.query(), idle));
    idle.target_npc = true;
    CHECK(a.next({any}, ab.query(), idle));
}
