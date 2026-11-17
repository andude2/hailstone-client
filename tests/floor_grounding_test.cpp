// A landing on the zone floor lets ground_npcs reuse the physics query, but only when the grounding callback
// is one that includes that same zone floor (WorldState::smooth's ground_floor_includes_zone contract).

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <optional>
#include <vector>

#include "game/world_state.h"

using namespace hailstone::game;

namespace {

Spawn& add_npc(WorldState& w, float z, float avatar_height = 3.0f, double update_time = 1.0) {
    Spawn& s = w.spawns[1];
    s.id = 1;
    s.is_npc = true;
    s.heights.avatar_height = avatar_height;
    s.update_time = update_time;
    s.position = glm::vec3(5.0f, 5.0f, z);
    w.ground_npcs = true;
    return s;
}

} // namespace

TEST_CASE("grounding: a landing with an inclusive grounding callback skips the second floor query") {
    WorldState w;
    Spawn& s = add_npc(w, 13.0f); // floor 10 + AvatarHeight 3: the physics step settles it here
    int zone_queries = 0, grounding_queries = 0;
    WorldState::FloorFn floor = [&](const glm::vec3&) {
        ++zone_queries;
        return std::optional<float>(10.0f);
    };
    WorldState::FloorFn grounding = [&](const glm::vec3&) {
        ++grounding_queries;
        return std::optional<float>(12.0f); // zone floor 10 plus a reachable higher door (probe ceiling 13 + 4 = 17)
    };
    w.smooth(0.016f, 2.0, floor, {}, {}, grounding, {}, true);
    CHECK(zone_queries == 1); // the physics step's one zone-mesh search, instead of two
    CHECK(grounding_queries == 0);
    CHECK(s.position.z == doctest::Approx(13.0f));
    CHECK(s.ground_drop == 0.0f); // a reachable higher floor only shortens the drop: 13 - (12 + 3) < 0
    CHECK(s.display_position.z == doctest::Approx(13.0f));
    CHECK(s.ground_probe_at == s.position); // remembered as measured, as a zero drop would be
    // An unchanged position is not measured again.
    w.smooth(0.016f, 2.016, floor, {}, {}, grounding, {}, true);
    CHECK(zone_queries == 2); // physics still steps the spawn each frame
    CHECK(grounding_queries == 0);
}

TEST_CASE("grounding: a spawn the physics step does not land still runs the full query") {
    WorldState w;
    Spawn& s = add_npc(w, 30.0f); // 17 over the stand height: outside the landing check's reach
    int zone_queries = 0, grounding_queries = 0;
    WorldState::FloorFn floor = [&](const glm::vec3&) {
        ++zone_queries;
        return std::optional<float>(10.0f);
    };
    WorldState::FloorFn grounding = [&](const glm::vec3&) {
        ++grounding_queries;
        return std::optional<float>(10.0f);
    };
    w.smooth(0.016f, 2.0, floor, {}, {}, grounding, {}, true);
    CHECK(zone_queries == 1);
    CHECK(grounding_queries == 1);
    CHECK(s.ground_drop == doctest::Approx(17.0f));
    CHECK(s.display_position.z == doctest::Approx(13.0f));
}

TEST_CASE("grounding: no floor in the physics step still runs the full query") {
    WorldState w;
    Spawn& s = add_npc(w, 30.0f);
    int zone_queries = 0, grounding_queries = 0;
    WorldState::FloorFn floor = [&](const glm::vec3&) {
        ++zone_queries;
        return std::optional<float>();
    };
    WorldState::FloorFn grounding = [&](const glm::vec3&) {
        ++grounding_queries;
        return std::optional<float>(10.0f);
    };
    w.smooth(0.016f, 2.0, floor, {}, {}, grounding, {}, true);
    CHECK(zone_queries == 1);
    CHECK(grounding_queries == 1);
    CHECK(s.ground_drop == doctest::Approx(17.0f));
    CHECK(s.display_position.z == doctest::Approx(13.0f));
}

TEST_CASE("grounding: a stale spawn has no landing to reuse, so it runs the full query") {
    WorldState w;
    Spawn& s = add_npc(w, 16.0f, 3.0f, -20.0); // last update over 12 s ago: not stepped
    int zone_queries = 0, grounding_queries = 0;
    WorldState::FloorFn floor = [&](const glm::vec3&) {
        ++zone_queries;
        return std::optional<float>(10.0f);
    };
    WorldState::FloorFn grounding = [&](const glm::vec3&) {
        ++grounding_queries;
        return std::optional<float>(10.0f);
    };
    w.smooth(0.016f, 2.0, floor, {}, {}, grounding, {}, true);
    CHECK(zone_queries == 0);
    CHECK(grounding_queries == 1);
    CHECK(s.ground_drop == doctest::Approx(3.0f));
    CHECK(s.display_position.z == doctest::Approx(13.0f));
}

TEST_CASE("grounding: zero physics steps still run the full query") {
    WorldState w;
    Spawn& s = add_npc(w, 16.0f);
    w.physics_steps = std::vector<float>{};
    int zone_queries = 0, grounding_queries = 0;
    WorldState::FloorFn floor = [&](const glm::vec3&) {
        ++zone_queries;
        return std::optional<float>(10.0f);
    };
    WorldState::FloorFn grounding = [&](const glm::vec3&) {
        ++grounding_queries;
        return std::optional<float>(10.0f);
    };
    w.smooth(0.016f, 2.0, floor, {}, {}, grounding, {}, true);
    CHECK(zone_queries == 0);
    CHECK(grounding_queries == 1);
    CHECK(s.ground_drop == doctest::Approx(3.0f));
    CHECK(s.display_position.z == doctest::Approx(13.0f));
}

TEST_CASE("grounding: a callback that does not include the zone floor still runs the full query") {
    WorldState w;
    Spawn& s = add_npc(w, 13.0f); // a landing did happen
    int zone_queries = 0, grounding_queries = 0;
    WorldState::FloorFn floor = [&](const glm::vec3&) {
        ++zone_queries;
        return std::optional<float>(10.0f);
    };
    WorldState::FloorFn grounding = [&](const glm::vec3&) {
        ++grounding_queries;
        return std::optional<float>(5.0f); // a door mesh that missed the zone floor: no inclusion contract
    };
    w.smooth(0.016f, 2.0, floor, {}, {}, grounding, {}, false);
    CHECK(zone_queries == 1);
    CHECK(grounding_queries == 1);
    CHECK(s.ground_drop == doctest::Approx(5.0f));
    CHECK(s.display_position.z == doctest::Approx(8.0f));
}

TEST_CASE("grounding: a landed floor above the grounding query's reach still runs the full query") {
    WorldState w;
    Spawn& s = add_npc(w, 5.0f, -5.0f); // lands at floor 10 + AvatarHeight -5 = 5; 10 > 5 + reach 4
    int zone_queries = 0, grounding_queries = 0;
    WorldState::FloorFn floor = [&](const glm::vec3&) {
        ++zone_queries;
        return std::optional<float>(10.0f);
    };
    WorldState::FloorFn grounding = [&](const glm::vec3&) {
        ++grounding_queries;
        return std::optional<float>(10.0f);
    };
    w.smooth(0.016f, 2.0, floor, {}, {}, grounding, {}, true);
    CHECK(zone_queries == 1);
    CHECK(grounding_queries == 1);
    CHECK(s.position.z == doctest::Approx(5.0f)); // still the physics landing height
    CHECK(s.ground_drop == 0.0f);
}

TEST_CASE("grounding: an invalid landing floor still runs the full query") {
    WorldState w;
    Spawn& s = add_npc(w, 13.0f);
    s.speed_run = 1.0f; // moving, so the step "lands" even with a bad floor
    int zone_queries = 0, grounding_queries = 0;
    WorldState::FloorFn floor = [&](const glm::vec3&) {
        ++zone_queries;
        return std::optional<float>(std::numeric_limits<float>::quiet_NaN());
    };
    WorldState::FloorFn grounding = [&](const glm::vec3&) {
        ++grounding_queries;
        return std::optional<float>(10.0f);
    };
    w.smooth(0.016f, 2.0, floor, {}, {}, grounding, {}, true);
    CHECK(zone_queries == 1);
    CHECK(grounding_queries == 1);
    CHECK_FALSE(std::isfinite(s.position.z)); // the bad floor was used by the physics step
    CHECK(s.ground_drop == 0.0f); // the full query's NaN drop is not drawn
}

TEST_CASE("grounding: a spawn that moves and lands this frame skips the second floor query") {
    WorldState w;
    Spawn& s = add_npc(w, 13.0f);
    s.speed_run = 1.0f; // horizontal movement: the step's moved branch lands it on the floor
    const glm::vec3 original = s.position;
    int zone_queries = 0, grounding_queries = 0;
    WorldState::FloorFn floor = [&](const glm::vec3&) {
        ++zone_queries;
        return std::optional<float>(10.0f);
    };
    WorldState::FloorFn grounding = [&](const glm::vec3&) {
        ++grounding_queries;
        return std::optional<float>(10.0f);
    };
    w.smooth(0.016f, 2.0, floor, {}, {}, grounding, {}, true);
    CHECK(zone_queries == 1);
    CHECK(grounding_queries == 0);
    CHECK(s.position != original); // it really moved this frame
    CHECK(s.position.z == doctest::Approx(13.0f));
    CHECK(s.ground_drop == 0.0f);
    CHECK(s.display_position == s.position); // drawn at the physics position
}

TEST_CASE("grounding: a later physics step that does not land takes the full query") {
    WorldState w;
    Spawn& s = add_npc(w, 14.0f); // within the settle reach of floor 10 + AvatarHeight 3
    w.physics_steps = std::vector<float>{16.0f, 16.0f};
    int zone_queries = 0, grounding_queries = 0;
    WorldState::FloorFn floor = [&](const glm::vec3&) {
        ++zone_queries;
        return zone_queries == 1 ? std::optional<float>(10.0f) : std::optional<float>();
    };
    WorldState::FloorFn grounding = [&](const glm::vec3&) {
        ++grounding_queries;
        return std::optional<float>(10.0f);
    };
    w.smooth(0.032f, 2.0, floor, {}, {}, grounding, {}, true);
    CHECK(zone_queries == 2);
    CHECK(grounding_queries == 1); // the first step's landing is not reused: the last step found no floor
    CHECK(s.position.z == doctest::Approx(13.0f));
    CHECK(s.ground_drop == 0.0f);
    CHECK(s.display_position.z == doctest::Approx(13.0f));
}

TEST_CASE("grounding: a changed floor without a new landing is measured, then the skip resumes") {
    WorldState w;
    Spawn& s = add_npc(w, 13.0f);
    int zone_queries = 0, grounding_queries = 0;
    float zone_floor = 10.0f; // both callbacks follow it: grounding always includes the zone floor
    WorldState::FloorFn floor = [&](const glm::vec3&) {
        ++zone_queries;
        return std::optional<float>(zone_floor);
    };
    WorldState::FloorFn grounding = [&](const glm::vec3&) {
        ++grounding_queries;
        return std::optional<float>(zone_floor);
    };
    s.display_valid = false;
    w.smooth(0.016f, 2.0, floor, {}, {}, grounding, {}, true);
    CHECK(grounding_queries == 0); // the landing let it skip
    CHECK(s.ground_drop == 0.0f);
    CHECK(s.display_position.z == doctest::Approx(13.0f));

    // The floor changed and this frame has no physics step, so there is no landing to reuse.
    w.reground(); // the zone's collision loaded, or a door came or moved
    zone_floor = 8.0f;
    w.physics_steps = std::vector<float>{};
    s.display_valid = false;
    w.smooth(0.016f, 2.016, floor, {}, {}, grounding, {}, true);
    CHECK(zone_queries == 1);
    CHECK(grounding_queries == 1); // the new floor is measured
    CHECK(s.ground_drop == doctest::Approx(2.0f)); // 13 - (8 + 3)
    CHECK(s.display_position.z == doctest::Approx(11.0f));

    // Once the spawn moves and a step lands again, the short-cut applies as before.
    zone_floor = 10.0f;
    w.physics_steps.reset();
    s.position.x += 1.0f; // a new position is measured again
    s.display_valid = false;
    w.smooth(0.016f, 2.032, floor, {}, {}, grounding, {}, true);
    CHECK(zone_queries == 2);
    CHECK(grounding_queries == 1);
    CHECK(s.ground_drop == 0.0f);
    CHECK(s.display_position.z == doctest::Approx(13.0f));
}

TEST_CASE("grounding: a query that finds no floor is retried the next frame") {
    WorldState w;
    Spawn& s = add_npc(w, 30.0f); // floating: no landing, so the full query is needed
    int grounding_queries = 0;
    bool have_floor = false; // both callbacks follow it: no floor for either, or the same zone floor
    WorldState::FloorFn floor = [&](const glm::vec3&) {
        return have_floor ? std::optional<float>(10.0f) : std::optional<float>();
    };
    WorldState::FloorFn grounding = [&](const glm::vec3&) {
        ++grounding_queries;
        return have_floor ? std::optional<float>(10.0f) : std::optional<float>();
    };
    s.display_valid = false;
    w.smooth(0.016f, 2.0, floor, {}, {}, grounding, {}, true);
    CHECK(grounding_queries == 1);
    CHECK(s.ground_drop == 0.0f);
    CHECK(std::isnan(s.ground_probe_at.x)); // not measured: the same position is tried again next frame
    w.smooth(0.016f, 2.016, floor, {}, {}, grounding, {}, true);
    CHECK(grounding_queries == 2);
    have_floor = true;
    s.display_valid = false;
    w.smooth(0.016f, 2.032, floor, {}, {}, grounding, {}, true);
    CHECK(grounding_queries == 3);
    CHECK(s.ground_drop == doctest::Approx(17.0f));
    CHECK(s.display_position.z == doctest::Approx(13.0f));
    CHECK(std::isfinite(s.ground_probe_at.x));
}
