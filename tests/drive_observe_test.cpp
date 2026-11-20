// D2's data path: a viewer attaches to the headless's observe server, gets the snapshot, and then follows the live
// zone packets and the local position/target/inventory state. The GUI window around this is the remaining D2 piece.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>

#include "app/drive_observe.h"
#include "core/byte_io.h"
#include "mock_server/harness.h"
#include "net/game_connection.h"
#include "platform/local_pipe.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace {

std::optional<DriveSnapshot> read_snapshot(platform::LocalPipeConnection& pipe) {
    const auto message = pipe.receive_message(5000);
    if (!message) return std::nullopt;
    const std::span<const std::byte> bytes(message->data(), message->size());
    ByteReader r(bytes);
    if (static_cast<app::DriveObserveMessage>(r.le<std::uint8_t>()) != app::DriveObserveMessage::Snapshot)
        return std::nullopt;
    return decode_drive_snapshot(bytes.subspan(1));
}

// Applies every message waiting on the pipe; returns after the queue is empty.
bool apply_waiting(platform::LocalPipeConnection& pipe, GameConnection& viewer) {
    bool applied = false;
    while (auto message = pipe.receive_message(0)) {
        const std::span<const std::byte> bytes(message->data(), message->size());
        ByteReader r(bytes);
        const auto kind = static_cast<app::DriveObserveMessage>(r.le<std::uint8_t>());
        const auto body = bytes.subspan(1);
        if (kind == app::DriveObserveMessage::Packet) {
            if (auto packet = app::decode_drive_packet(body)) viewer.receive_zone_packet_for_test(*packet);
        } else if (kind == app::DriveObserveMessage::State) {
            if (auto state = app::decode_drive_state(body)) {
                DriveSnapshot local;
                local.character = viewer.character_name();
                local.position = state->position;
                local.heading = state->heading;
                local.velocity = state->velocity;
                local.moving = state->moving;
                local.target = state->target;
                if (state->inventory_packet) local.inventory_packet = *state->inventory_packet;
                viewer.apply_snapshot(local);
            }
        }
        applied = true;
    }
    return applied;
}

bool has_chat_containing(GameConnection& conn, std::string_view text) {
    while (auto line = conn.poll_chat())
        if (line->text.find(text) != std::string::npos) return true;
    return false;
}

} // namespace

TEST_CASE("drive observe: a viewer follows the headless snapshot and live state") {
    MockEqServer server;
    EmptyGameDir dir;
    GameDir game(dir.path());

    app::DriveObserveServer observe(platform::make_local_pipe_session());
    net::ConnectionOptions options = quick_options();
    options.snapshot_journal = true;
    options.zone_packet_tap = [&observe](const AppPacket& packet) { observe.publish_zone_packet(packet); };
    GameConnection source(game, std::move(options));

    auto viewer_pipe = platform::LocalPipeConnection::connect(observe.session(), 5000);
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
    observe.set_snapshot(source.capture_snapshot());

    const auto snapshot = read_snapshot(*viewer_pipe);
    REQUIRE(snapshot);
    GameConnection viewer(game, quick_options());
    viewer.apply_snapshot(*snapshot);
    CHECK(viewer.state() == ConnectionState::InZone);
    CHECK(viewer.character_name() == "Gennro");
    CHECK(viewer.world().spawns.count(101) == 1);
    CHECK(viewer.inventory().items().size() == 2);

    // The source walks; the local state message carries what the server does not echo.
    source.set_player_motion(glm::vec3(15.0f, 25.0f, 3.5f), 256.0f, glm::vec3(0.0f), true);
    auto local = source.capture_snapshot();
    app::DriveLocalState state;
    state.position = local.position;
    state.heading = local.heading;
    state.velocity = local.velocity;
    state.moving = local.moving;
    state.target = 101;
    state.inventory_packet = local.inventory_packet;
    observe.publish_state(state);
    REQUIRE(pump([&] { apply_waiting(*viewer_pipe, viewer); }, [&] {
        return viewer.world().player() && viewer.world().player()->position.x == doctest::Approx(15.0f);
    }));
    CHECK(viewer.target() == 101);

    // A live zone packet: the server's chat reaches the source, the tap forwards it, the viewer applies it.
    server.chat("A_Rat", 8, "hello viewer");
    REQUIRE(pump(step, [&] { return has_chat_containing(source, "hello viewer"); }));
    REQUIRE(pump([&] { apply_waiting(*viewer_pipe, viewer); }, [&] {
        return has_chat_containing(viewer, "hello viewer");
    }));

    viewer_pipe->close();
}
