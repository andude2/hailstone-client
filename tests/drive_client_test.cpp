// D2's GUI-side data path: the production DriveClient attaches to the headless observe server, creates a normal
// GameConnection from the snapshot, and follows live zone packets and local state. The window around it is next.

#include <doctest/doctest.h>

#include <chrono>
#include <string>

#include "app/drive_client.h"
#include "app/drive_observe.h"
#include "mock_server/harness.h"
#include "net/game_connection.h"
#include "platform/local_pipe.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace {

bool has_chat_containing(GameConnection& conn, std::string_view text) {
    while (auto line = conn.poll_chat())
        if (line->text.find(text) != std::string::npos) return true;
    return false;
}

} // namespace

TEST_CASE("drive client: a GameConnection follows the observe server") {
    MockEqServer server;
    EmptyGameDir dir;
    GameDir game(dir.path());

    app::DriveObserveServer observe(platform::make_local_pipe_session());
    net::ConnectionOptions source_options = quick_options();
    source_options.snapshot_journal = true;
    source_options.zone_packet_tap = [&observe](const AppPacket& packet) { observe.publish_zone_packet(packet); };
    GameConnection source(game, std::move(source_options));
    auto source_step = [&] {
        source.update();
        server.poll();
    };
    source.start(server.ticket());
    REQUIRE(pump(source_step, [&] { return source.state() == ConnectionState::CharacterSelect; }));
    source.enter_world("Gennro");
    REQUIRE(pump(source_step, [&] { return source.state() == ConnectionState::InZone; }));
    REQUIRE(pump(source_step, [&] { return source.player_status().max_hp != 0; }));
    source.set_player_motion(glm::vec3(10.0f, 20.0f, 3.0f), 128.0f, glm::vec3(0.0f), false);
    source.set_target(101);
    observe.set_snapshot(source.capture_snapshot());

    app::DriveObserveFile file;
    file.endpoint = observe.session().endpoint;
    file.token = observe.session().token;
    file.character = "Gennro";
    std::string error;
    auto client = app::DriveClient::connect(file, error);
    REQUIRE_MESSAGE(client != nullptr, error);
    CHECK(client->info().character == "Gennro");

    net::ConnectionOptions viewer_options = quick_options();
    viewer_options.session_factory = [&client] { return client->make_session(); };
    GameConnection viewer(game, std::move(viewer_options));
    viewer.start_observe(client->make_session());
    viewer.apply_snapshot(client->take_snapshot());
    CHECK(viewer.state() == ConnectionState::InZone);
    CHECK(viewer.character_name() == "Gennro");
    CHECK(viewer.world().spawns.count(101) == 1);
    CHECK(viewer.inventory().items().size() == 2);

    // Move: the server does not echo the source's own position; the State message carries it.
    source.set_player_motion(glm::vec3(15.0f, 25.0f, 3.5f), 256.0f, glm::vec3(0.0f), true);
    auto local = source.capture_local_state();
    app::DriveLocalState state;
    state.position = local.position;
    state.heading = local.heading;
    state.velocity = local.velocity;
    state.moving = local.moving;
    state.target = 101;
    state.inventory_packet = local.inventory_packet;
    observe.publish_state(state);
    REQUIRE(pump([&] {
        viewer.update();
        if (auto incoming = client->take_local_state()) viewer.apply_local_state(*incoming);
    }, [&] {
        return viewer.world().player() && viewer.world().player()->position.x == doctest::Approx(15.0f);
    }));
    CHECK(viewer.target() == 101);

    // A live zone packet: chat reaches the source, the tap forwards it, the viewer's GameConnection handles it.
    server.chat("A_Rat", 8, "hello viewer");
    REQUIRE(pump(source_step, [&] { return has_chat_containing(source, "hello viewer"); }));
    REQUIRE(pump([&] { viewer.update(); }, [&] { return has_chat_containing(viewer, "hello viewer"); }));
}
