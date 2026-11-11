#include <doctest/doctest.h>

#include "game/world_state.h"

using hailstone::game::Spawn;

namespace {

Spawn npc(const char* name) {
    Spawn s;
    s.is_npc = true;
    s.body_type = 1; // humanoid
    s.class_id = 1;  // warrior
    s.name = name;
    return s;
}

} // namespace

TEST_CASE("Spawn::IsNamed: capitalised NPC names without a last name") {
    auto s = npc("Lord_Bob00");
    CHECK(Spawn::IsNamed(&s));
    s = npc("#Lady_Vox00");
    CHECK(Spawn::IsNamed(&s));

    // Lower-case first letters, "A_"/"An_" and the guard-like prefixes (any case) are not named.
    for (const char* name : {"a_rat00", "A_rat00", "An_orc00", "guard_Bob00", "WARD_of_Fire00", "Diaku_scout00",
                             "Imperial_Crypt_door00", "Sage_Zed00", "aura00", ""}) {
        s = npc(name);
        CHECK_MESSAGE(!Spawn::IsNamed(&s), name);
    }
    // "A" or "An" alone is still a capitalised name.
    s = npc("A");
    CHECK(Spawn::IsNamed(&s));
    s = npc("An");
    CHECK(Spawn::IsNamed(&s));

    s = npc("Lord_Bob00");
    s.last_name = "the Brave";
    CHECK(!Spawn::IsNamed(&s));
    s = npc("Lord_Bob00");
    s.targetable = false;
    CHECK(!Spawn::IsNamed(&s));
    s = npc("Lord_Bob00");
    s.is_npc = false;
    CHECK(!Spawn::IsNamed(&s));
    CHECK(!Spawn::IsNamed(nullptr));
}
