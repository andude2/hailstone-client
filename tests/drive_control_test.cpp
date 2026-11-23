// D3 of the drive plan (docs/research/drive-headless.md): a control lease lends a live, authenticated session pair
// to another GameConnection and takes the exact state back. The lease pipe is the next step; this proves the state
// seam (release_sessions / adopt_sessions) that both ends need.

#include <doctest/doctest.h>

#include <memory>
#include <string>

#include "mock_server/harness.h"
#include "net/game_connection.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

TEST_CASE("drive control: a live zone session is lent to another connection and taken back") {
    MockEqServer server;
    EmptyGameDir dir;
    GameDir game(dir.path());

    net::ConnectionOptions options = quick_options();
    options.snapshot_journal = true;
    net::GameConnection owner(game, options);
    auto owner_step = [&] {
        owner.update();
        server.poll();
    };
    owner.start(server.ticket());
    REQUIRE(pump(owner_step, [&] { return owner.state() == ConnectionState::CharacterSelect; }));
    owner.enter_world("Gennro");
    REQUIRE(pump(owner_step, [&] { return owner.state() == ConnectionState::InZone; }));
    REQUIRE(pump(owner_step, [&] { return owner.player_status().max_hp != 0; }));

    const DriveSnapshot lent = owner.capture_snapshot();
    REQUIRE_FALSE(lent.packets.empty());
    // In a zone the world session is gone (the client dropped it when it zoned); the zone session is the live one.
    auto [world, zone] = owner.release_sessions();
    REQUIRE(zone);
    CHECK_FALSE(world);

    // The viewer is a normal GameConnection on the lent sessions: no world login goes out.
    net::GameConnection viewer(game, options);
    viewer.adopt_sessions(std::move(world), std::move(zone));
    viewer.apply_snapshot(lent);
    CHECK(viewer.state() == ConnectionState::InZone);
    CHECK(viewer.character_name() == "Gennro");

    auto viewer_step = [&] {
        viewer.update();
        server.poll();
    };
    const int messages = MockEqServer::count(server.zone_log, Op::ChannelMessage);
    viewer.send_command("/say hello from the viewer");
    REQUIRE(pump(viewer_step, [&] { return MockEqServer::count(server.zone_log, Op::ChannelMessage) > messages; }));

    const std::size_t updates = server.client_updates.size();
    viewer.set_player_motion(glm::vec3(4.0f, 5.0f, 6.0f), 90.0f, glm::vec3(0.0f), false);
    viewer.send_position_now();
    REQUIRE(pump(viewer_step, [&] { return server.client_updates.size() > updates; }));

    // Hand the zone's exact state back to a fresh owner and keep playing on the same sessions.
    const DriveSnapshot returned = viewer.capture_snapshot();
    CHECK_FALSE(returned.packets.empty());
    auto [world2, zone2] = viewer.release_sessions();
    net::GameConnection resumed(game, options);
    resumed.adopt_sessions(std::move(world2), std::move(zone2));
    resumed.apply_snapshot(returned);
    CHECK(resumed.state() == ConnectionState::InZone);
    CHECK(resumed.character_name() == "Gennro");
    auto resumed_step = [&] {
        resumed.update();
        server.poll();
    };
    const int messages2 = MockEqServer::count(server.zone_log, Op::ChannelMessage);
    resumed.send_command("/say back again");
    REQUIRE(pump(resumed_step, [&] { return MockEqServer::count(server.zone_log, Op::ChannelMessage) > messages2; }));
}
