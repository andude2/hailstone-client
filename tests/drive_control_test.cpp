// D3 of the drive plan (docs/research/drive-headless.md): a control lease lends a live, authenticated session pair
// to another GameConnection and takes the exact state back. The lease pipe is the next step; this proves the state
// seam (release_sessions / adopt_sessions) that both ends need.

#include <doctest/doctest.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "app/drive_control.h"
#include "mock_server/harness.h"
#include "net/game_connection.h"
#include "platform/local_pipe.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace {
// OP_XTargetResponse {max, count, slot, status, id, name}: one slot holding a spawn.
net::AppPacket xtarget_slot(std::uint32_t slot, std::uint32_t id, const std::string& name) {
    ByteWriter w;
    w.le<std::uint32_t>(5);
    w.le<std::uint32_t>(1);
    w.le<std::uint32_t>(slot);
    w.le<std::uint8_t>(1);
    w.le<std::uint32_t>(id);
    w.fixed_string(name, name.size() + 1);
    return net::AppPacket(net::to_wire(net::Op::XTargetResponse), w.take());
}
} // namespace

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

TEST_CASE("drive control: a snapshot larger than one pipe message crosses the lease") {
    // A long session's journal is bigger than the pipe's 8 MiB message cap; the snapshot must cross in chunks.
    app::DriveRelay relay(platform::make_local_pipe_session());
    net::DriveSnapshot big;
    big.character = "Gennro";
    const std::vector<std::byte> payload(48 * 1024, std::byte{0x5a});
    for (int i = 0; i < 512; ++i)
        big.packets.push_back(net::AppPacket(static_cast<std::uint16_t>(0x1234), payload));
    std::string error;
    auto client = app::DriveControlClient::connect(relay.session(), 10000, error);
    REQUIRE_MESSAGE(client != nullptr, error);
    REQUIRE(pump([] {}, [&] { return relay.viewer_waiting(); }, std::chrono::milliseconds(2000)));
    relay.begin_lease(big);
    CHECK_FALSE(relay.viewer_waiting());
    std::optional<net::DriveSnapshot> taken;
    for (int i = 0; i < 2000 && !taken; ++i) {
        relay.pump();
        client->pump();
        taken = client->take_snapshot();
        if (!taken) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(taken);
    CHECK(taken->packets.size() == big.packets.size());
    CHECK(taken->packets.front().data.size() == payload.size());

    client->release(big);
    REQUIRE(pump([&] { relay.pump(); }, [&] { return relay.released(); }, std::chrono::seconds(10)));
    const net::DriveSnapshot returned = relay.take_release();
    CHECK(returned.packets.size() == big.packets.size());
    // The released pipe is closed: it must not count as a viewer, or the headless would lend to it again.
    CHECK_FALSE(relay.viewer_connected());
    CHECK_FALSE(relay.viewer_waiting()); // a released lease is not a waiting viewer

    // A second viewer replaces the closed pipe, so the same character can be driven again.
    std::string error2;
    auto client2 = app::DriveControlClient::connect(relay.session(), 10000, error2);
    REQUIRE_MESSAGE(client2 != nullptr, error2);
    REQUIRE(pump([] {}, [&] { return relay.viewer_waiting(); }, std::chrono::milliseconds(2000)));
    net::DriveSnapshot again;
    again.character = "Gennro";
    again.packets.push_back(net::AppPacket(static_cast<std::uint16_t>(0x1234),
                                           std::vector<std::byte>(1024, std::byte{0x11})));
    relay.begin_lease(again);
    CHECK_FALSE(relay.viewer_waiting());
    std::optional<net::DriveSnapshot> second;
    for (int i = 0; i < 500 && !second; ++i) {
        relay.pump();
        client2->pump();
        second = client2->take_snapshot();
        if (!second) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(second);
    CHECK(second->packets.size() == again.packets.size());
}

TEST_CASE("drive control: extended targets survive the lease and the hand-back") {
    // The reported symptom was extended targets dropping when the graphical character returned; they are part of
    // the zone stream, so the snapshot journal must carry them through both hand-offs.
    MockEqServer server;
    EmptyGameDir dir;
    GameDir game(dir.path());
    net::ConnectionOptions options = quick_options();
    options.snapshot_journal = true;

    net::GameConnection owner(game, options);
    auto step = [&] {
        owner.update();
        server.poll();
    };
    owner.start(server.ticket());
    REQUIRE(pump(step, [&] { return owner.state() == ConnectionState::CharacterSelect; }));
    owner.enter_world("Gennro");
    REQUIRE(pump(step, [&] { return owner.state() == ConnectionState::InZone; }));
    REQUIRE(pump(step, [&] { return owner.player_status().max_hp != 0; }));
    owner.receive_zone_packet_for_test(xtarget_slot(0, 101, "a_rat01"));
    REQUIRE(pump(step, [&] { return owner.xtargets().slots.size() > 0 && owner.xtargets().slots[0].spawn_id == 101; }));

    const DriveSnapshot lent = owner.capture_snapshot();
    auto [world, zone] = owner.release_sessions();
    net::GameConnection viewer(game, options);
    viewer.adopt_sessions(std::move(world), std::move(zone));
    viewer.apply_snapshot(lent);
    REQUIRE(viewer.xtargets().slots.size() > 0);
    CHECK(viewer.xtargets().slots[0].spawn_id == 101);

    const DriveSnapshot returned = viewer.capture_snapshot();
    auto [world2, zone2] = viewer.release_sessions();
    net::GameConnection resumed(game, options);
    resumed.adopt_sessions(std::move(world2), std::move(zone2));
    resumed.apply_snapshot(returned);
    REQUIRE(resumed.xtargets().slots.size() > 0);
    CHECK(resumed.xtargets().slots[0].spawn_id == 101);
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

    std::string error;
    auto client = app::DriveControlClient::connect(relay.session(), 10000, error);
    REQUIRE_MESSAGE(client != nullptr, error);
    // The viewer's sessions exist before it pumps: a State message for an id it has not made yet would be dropped.
    auto mirror_world = client->make_session(); // the world, id 1
    auto mirror_zone = client->make_session();  // the zone, id 2
    // The accept thread stores the accepted pipe just after the handshake; a new viewer resets the lease state, so
    // set the snapshot after the reset (the headless loop sets it once viewer_connected() is true).
    REQUIRE(pump([] {}, [&] { return relay.viewer_connected(); }, std::chrono::milliseconds(2000)));
    relay.begin_lease(owner.capture_snapshot());
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

TEST_CASE("drive control: inventory clicks cross the lease and acked release preserves xtargets and ticket") {
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
    owner.set_xtarget_auto_haters(true);
    owner.receive_zone_packet_for_test(xtarget_slot(0, 101, "a_rat01"));
    owner.receive_zone_packet_for_test(xtarget_slot(1, 102, "a_bat02"));
    REQUIRE(pump(owner_step, [&] { return owner.xtargets().slots.size() >= 2 && owner.xtargets().slots[1].spawn_id == 102; }));

    app::DriveRelay relay(platform::make_local_pipe_session());
    std::string error;
    auto client = app::DriveControlClient::connect(relay.session(), 10000, error);
    REQUIRE_MESSAGE(client != nullptr, error);
    auto mirror_world = client->make_session();
    auto mirror_zone = client->make_session();
    REQUIRE(pump([] {}, [&] { return relay.viewer_waiting(); }, std::chrono::milliseconds(2000)));
    relay.begin_lease(owner.capture_snapshot());
    auto [world, zone] = owner.release_sessions();
    relay.adopt(1, std::move(world));
    relay.adopt(2, std::move(zone));

    std::optional<net::DriveSnapshot> snapshot;
    for (int i = 0; i < 1000 && (!snapshot || !client->session_connected(2)); ++i) {
        relay.pump();
        client->pump();
        if (!snapshot) snapshot = client->take_snapshot();
        if (!snapshot || !client->session_connected(2)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(snapshot);
    REQUIRE(client->session_connected(2));

    net::GameConnection viewer(game, options);
    viewer.adopt_sessions(std::move(mirror_world), std::move(mirror_zone));
    viewer.apply_snapshot(*snapshot);
    CHECK(viewer.xtargets().auto_haters);
    REQUIRE(viewer.xtargets().slots.size() >= 2);
    CHECK(viewer.xtargets().slots[0].spawn_id == 101);
    CHECK(viewer.xtargets().slots[1].spawn_id == 102);

    // Click general slot 1 in the viewer: OP_MoveItem must cross the lease to the server.
    const int moves_before = MockEqServer::count(server.zone_log, Op::MoveItem);
    REQUIRE(viewer.inventory().at(ItemSlot::possessions(kSlotGeneral1)) != nullptr);
    viewer.click_slot(ItemSlot::possessions(kSlotGeneral1));
    CHECK(viewer.inventory().cursor() != nullptr);
    auto step = [&] {
        relay.pump();
        viewer.update();
        server.poll();
    };
    REQUIRE(pump(step, [&] { return MockEqServer::count(server.zone_log, Op::MoveItem) > moves_before; }));

    // Release with ReleaseAck while the relay pumps on another thread (matches live two-process release).
    std::atomic<bool> stop_pump{false};
    std::thread relay_thread([&] {
        while (!stop_pump.load() && !relay.released()) {
            relay.pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    auto returned_snapshot = viewer.capture_snapshot();
    auto [vw, vz] = viewer.release_sessions();
    client->release(std::move(returned_snapshot), vw.get(), vz.get(), 2000);
    stop_pump.store(true);
    relay_thread.join();
    REQUIRE(relay.released());

    const DriveSnapshot returned = relay.take_release();
    auto back_world = relay.take_world_session();
    auto back_zone = relay.take_zone_session();
    REQUIRE(back_zone);

    net::GameConnection resumed(game, options);
    resumed.adopt_sessions(std::move(back_world), std::move(back_zone));
    resumed.adopt_ticket_from(owner);
    resumed.apply_snapshot(returned);
    CHECK(resumed.ticket().world_host == owner.ticket().world_host);
    CHECK(resumed.xtargets().auto_haters);
    REQUIRE(resumed.xtargets().slots.size() >= 2);
    CHECK(resumed.xtargets().slots[0].spawn_id == 101);
    CHECK(resumed.xtargets().slots[1].spawn_id == 102);
    CHECK(resumed.inventory().cursor() != nullptr);
}

TEST_CASE("drive control: mirror session reports disconnected when the lease pipe drops") {
    auto relay = std::make_unique<app::DriveRelay>(platform::make_local_pipe_session());
    std::string error;
    auto client = app::DriveControlClient::connect(relay->session(), 10000, error);
    REQUIRE_MESSAGE(client != nullptr, error);
    auto mirror_world = client->make_session();
    auto mirror_zone = client->make_session();
    REQUIRE(pump([] {}, [&] { return relay->viewer_waiting(); }, std::chrono::milliseconds(2000)));
    relay->begin_lease(net::DriveSnapshot{.character = "Gennro"});
    relay->pump();
    relay.reset(); // unclean relay shutdown drops the pipe
    REQUIRE(pump([&] { client->pump(); }, [&] { return client->closed(); }, std::chrono::milliseconds(2000)));
    CHECK(mirror_zone->state() == net::SessionState::Disconnected);
}
