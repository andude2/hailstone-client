// D3 of the drive plan (docs/research/drive-headless.md): a control lease lends a live, authenticated session pair
// to another GameConnection and takes the exact state back. The lease pipe is the next step; this proves the state
// seam (release_sessions / adopt_sessions) that both ends need.

#include <doctest/doctest.h>

#include <memory>
#include <string>

#include "app/drive_control.h"
#include "mock_server/harness.h"
#include "net/game_connection.h"
#include "platform/local_pipe.h"

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

TEST_CASE("drive control: the lease pipe drives the headless character's live zone session") {
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

    // The headless host lends the zone to a viewer over the authenticated pipe.
    app::DriveRelay relay(platform::make_local_pipe_session());
    auto [world, zone] = owner.release_sessions();
    CHECK_FALSE(world); // in a zone the world session is gone
    relay.adopt(2, std::move(zone));
    relay.set_snapshot(owner.capture_snapshot());

    std::string error;
    auto client = app::DriveControlClient::connect(relay.session(), 10000, error);
    REQUIRE_MESSAGE(client != nullptr, error);
    // The viewer's sessions exist before it pumps: a State message for an id it has not made yet would be dropped.
    auto mirror_world = client->make_session(); // the world, id 1
    auto mirror_zone = client->make_session();  // the zone, id 2
    // The accept thread stores the accepted pipe just after the handshake; wait for it, then exchange the snapshot
    // (which crosses loopback TCP, so it may take a moment).
    REQUIRE(pump([] {}, [&] { return relay.viewer_connected(); }, std::chrono::milliseconds(2000)));
    std::optional<net::DriveSnapshot> snapshot;
    for (int i = 0; i < 500 && !snapshot; ++i) {
        relay.pump();
        client->pump();
        snapshot = client->take_snapshot();
        if (!snapshot) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(snapshot);

    net::GameConnection viewer(game, options);
    viewer.adopt_sessions(std::move(mirror_world), std::move(mirror_zone));
    viewer.apply_snapshot(*snapshot);
    CHECK(viewer.state() == ConnectionState::InZone);
    CHECK(viewer.character_name() == "Gennro");

    // The state message may arrive just after the snapshot; a GameConnection sends only when its zone is Connected.
    REQUIRE(pump([&] { relay.pump(); client->pump(); }, [&] { return client->session_connected(2); },
                 std::chrono::milliseconds(2000)));

    auto step = [&] {
        relay.pump();
        viewer.update();
        server.poll();
    };
    const int messages = MockEqServer::count(server.zone_log, Op::ChannelMessage);
    viewer.send_command("/say over the lease");
    REQUIRE(pump(step, [&] { return MockEqServer::count(server.zone_log, Op::ChannelMessage) > messages; }));

    const std::size_t updates = server.client_updates.size();
    viewer.set_player_motion(glm::vec3(7.0f, 8.0f, 9.0f), 45.0f, glm::vec3(0.0f), false);
    viewer.send_position_now();
    REQUIRE(pump(step, [&] { return server.client_updates.size() > updates; }));

    // The viewer hands the state back; the headless adopts the same sessions and plays on.
    client->release(viewer.capture_snapshot());
    REQUIRE(pump([&] { relay.pump(); }, [&] { return relay.released(); }));
    const DriveSnapshot returned = relay.take_release();
    REQUIRE_FALSE(returned.packets.empty());
    auto back_world = relay.take_session(1);
    auto back_zone = relay.take_session(2);
    REQUIRE(back_zone);

    net::GameConnection resumed(game, options);
    resumed.adopt_sessions(std::move(back_world), std::move(back_zone));
    resumed.apply_snapshot(returned);
    auto resumed_step = [&] {
        resumed.update();
        server.poll();
    };
    const int messages2 = MockEqServer::count(server.zone_log, Op::ChannelMessage);
    resumed.send_command("/say back on the headless");
    REQUIRE(pump(resumed_step, [&] { return MockEqServer::count(server.zone_log, Op::ChannelMessage) > messages2; }));
}
