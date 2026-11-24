// D1 of the drive plan (docs/research/drive-headless.md): a GameConnection's zone journal and local state replay
// into a fresh GameConnection, which is what lets a GUI start with a running headless character's state.

#include <doctest/doctest.h>

#include <memory>
#include <string>

#include "mock_server/harness.h"
#include "net/game_connection.h"
#include "plugin/hooks.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

TEST_CASE("drive snapshot: applying a snapshot does not re-raise the game state or every spawn") {
    // The connection is new, but its plugins already knew the character was in game: a lease's resume must not look
    // like a fresh login (OnGameState(InGame) plus OnAddSpawn for every spawn), which resets them.
    MockEqServer server;
    EmptyGameDir dir;
    GameDir game(dir.path());
    net::ConnectionOptions options = quick_options();
    options.snapshot_journal = true;
    plugin::Hooks hooks;
    int game_states = 0, spawns = 0;
    hooks.on_game_state(hooks.new_owner("test"), [&](int) { ++game_states; });
    hooks.on_spawn(hooks.new_owner("test"), [&](std::uint32_t, bool) { ++spawns; });
    options.hooks = &hooks;
    net::GameConnection source(game, options);
    auto step = [&] {
        source.update();
        server.poll();
    };
    source.start(server.ticket());
    REQUIRE(pump(step, [&] { return source.state() == ConnectionState::CharacterSelect; }));
    source.enter_world("Gennro");
    REQUIRE(pump(step, [&] { return source.state() == ConnectionState::InZone; }));
    REQUIRE(pump(step, [&] { return source.player_status().max_hp != 0; }));
    const DriveSnapshot snapshot = source.capture_snapshot();
    REQUIRE_FALSE(snapshot.packets.empty());

    game_states = 0;
    spawns = 0;
    net::GameConnection restored(game, options);
    restored.apply_snapshot(snapshot);
    restored.update();
    CHECK(game_states == 0); // no fake login
    CHECK(spawns > 0);       // the spawn refresh the plugins read
}

TEST_CASE("drive snapshot: a zone journal replays into a fresh connection") {
    MockEqServer server;
    EmptyGameDir dir;
    GameDir game(dir.path());

    net::ConnectionOptions recording = quick_options();
    recording.snapshot_journal = true;
    net::GameConnection source(game, std::move(recording));
    auto step = [&] {
        source.update();
        server.poll();
    };
    source.start(server.ticket());
    REQUIRE(pump(step, [&] { return source.state() == ConnectionState::CharacterSelect; }));
    source.enter_world("Gennro");
    REQUIRE(pump(step, [&] { return source.state() == ConnectionState::InZone; }));
    REQUIRE(pump(step, [&] { return source.player_status().max_hp != 0; }));

    source.set_player_motion(glm::vec3(10.0f, 20.0f, 3.0f), 128.0f, glm::vec3(0.0f), false);
    source.set_target(101);
    const DriveSnapshot snapshot = source.capture_snapshot();
    REQUIRE_FALSE(snapshot.packets.empty());
    CHECK_FALSE(snapshot.truncated);
    CHECK(snapshot.target == 101);
    CHECK_FALSE(snapshot.inventory_packet.data.empty());
    CHECK(snapshot.position == glm::vec3(10.0f, 20.0f, 3.0f));

    const auto encoded = encode_drive_snapshot(snapshot);
    REQUIRE_FALSE(encoded.empty());
    const auto decoded = decode_drive_snapshot(encoded);
    REQUIRE(decoded);
    CHECK(decoded->character == snapshot.character);
    CHECK(decoded->packets.size() == snapshot.packets.size());
    CHECK(decoded->inventory_packet.data == snapshot.inventory_packet.data);
    CHECK(decoded->position == snapshot.position);
    CHECK(decoded->target == snapshot.target);
    CHECK_FALSE(decode_drive_snapshot({}));
    CHECK_FALSE(decode_drive_snapshot(std::span(encoded).first(encoded.size() / 2)));

    net::GameConnection restored(game, quick_options());
    restored.apply_snapshot(snapshot);
    CHECK(restored.state() == ConnectionState::InZone);
    CHECK(restored.character_name() == "Gennro");
    CHECK(restored.zone_id() == 2);
    CHECK(restored.world().spawns.count(101) == 1);
    CHECK(restored.world().spawns.count(201) == 0);
    CHECK(restored.target() == 101);
    CHECK(restored.inventory().items().size() == 2);
    CHECK(restored.player_status().max_hp != 0);
    REQUIRE(restored.profile());
    CHECK(restored.profile()->name == "Gennro");
    const auto* me = restored.world().player();
    REQUIRE(me);
    CHECK(me->position.x == doctest::Approx(10.0f));
    CHECK(me->position.y == doctest::Approx(20.0f));
    CHECK(me->position.z == doctest::Approx(3.0f));
    const auto zone = restored.take_zone_to_load();
    REQUIRE(zone);
    CHECK(*zone == "qeynos2");

    net::GameConnection from_wire(game, quick_options());
    from_wire.apply_snapshot(*decoded);
    CHECK(from_wire.state() == ConnectionState::InZone);
    CHECK(from_wire.character_name() == "Gennro");
    CHECK(from_wire.world().spawns.count(101) == 1);
    CHECK(from_wire.inventory().items().size() == 2);
    CHECK(from_wire.target() == 101);
}
