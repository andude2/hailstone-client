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

    app::DriveObserveServer observe(platform::make_local_pipe_session(), true);
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
    file.interactive = true;
    std::string error;
    auto client = app::DriveClient::connect(file, error);
    REQUIRE_MESSAGE(client != nullptr, error);
    CHECK(client->info().character == "Gennro");

    net::ConnectionOptions viewer_options = quick_options();
    viewer_options.session_factory = [&client] { return client->make_session(); };
    GameConnection viewer(game, std::move(viewer_options));
    viewer.start_observe(client->make_session());
    viewer.set_drive_input([&client](const DriveAction& action) { client->send_action(action); });
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
    state.ui = source.capture_drive_ui();
    observe.publish_state(state);
    REQUIRE(pump([&] {
        viewer.update();
        if (auto incoming = client->take_local_state()) viewer.apply_local_state(*incoming);
    }, [&] {
        return viewer.world().player() && viewer.world().player()->position.x == doctest::Approx(15.0f);
    }));
    CHECK(viewer.target() == 101);

    auto publish_ui = [&] {
        state.ui = source.capture_drive_ui();
        state.inventory_packet = source.capture_local_state().inventory_packet;
        observe.publish_state(state);
    };
    auto viewer_step = [&] {
        viewer.update();
        if (auto incoming = client->take_local_state()) viewer.apply_local_state(*incoming);
    };
    auto next_action = [&]() -> std::optional<DriveAction> {
        std::vector<DriveAction> actions;
        if (!pump([&] { actions = observe.take_actions(); }, [&] { return !actions.empty(); }))
            return std::nullopt;
        CHECK(actions.size() == 1);
        return actions.front();
    };
    viewer.click_slot(ItemSlot::possessions(kSlotGeneral1));
    CHECK_FALSE(viewer.quantity_request());
    auto click = next_action();
    REQUIRE(click);
    REQUIRE(source.apply_drive_action(*click));
    REQUIRE(source.quantity_request());
    publish_ui();
    REQUIRE(pump(viewer_step, [&] { return viewer.quantity_request().has_value(); }));
    viewer.finish_quantity(2);
    auto quantity = next_action();
    REQUIRE(quantity);
    REQUIRE(source.apply_drive_action(*quantity));
    REQUIRE(source.inventory().cursor());
    CHECK(source.inventory().cursor()->stack_count == 2);
    CHECK_FALSE(source.apply_drive_action(*click)); // an older view cannot pick up the changed stack
    publish_ui();
    state.inventory_packet.reset();
    observe.publish_state(state); // a later position-only message must retain the inventory update
    REQUIRE(pump(viewer_step, [&] { return viewer.inventory().cursor() != nullptr; }));
    CHECK(viewer.inventory().cursor()->stack_count == 2);
    CHECK_FALSE(viewer.quantity_request());

    source.ask_disable_name_reminder(0);
    publish_ui();
    REQUIRE(pump(viewer_step, [&] { return viewer.confirmation().has_value(); }));
    viewer.answer_confirmation(false);
    auto answer = next_action();
    REQUIRE(answer);
    REQUIRE(source.apply_drive_action(*answer));
    CHECK_FALSE(source.confirmation());
    source.ask_disable_name_reminder(0);
    CHECK_FALSE(source.apply_drive_action(*answer)); // same dialog kind, different question
    publish_ui();
    REQUIRE(pump(viewer_step, [&] {
        return viewer.confirmation() && viewer.confirmation()->drive_id != answer->dialog;
    }));

    source.answer_confirmation(false);
    auto host_item = make_item(1003, "Socketed item", kSlotGeneral1);
    host_item.serial = "0000000000000001";
    host_item.instance_id = 777;
    host_item.aug_sockets[0] = {1, 1, 0};
    auto augment = make_item(1004, "Augment", kSlotCursor);
    augment.instance_id = 778;
    augment.item_type = 54;
    augment.aug_type = 1;
    augment.aug_distiller = 47009;
    source.receive_zone_packet_for_test(encode_char_inventory({host_item, augment}));
    publish_ui();
    REQUIRE(pump(viewer_step, [&] {
        const auto* item = viewer.inventory().at(host_item.slot);
        return item && item->id == host_item.id && !viewer.confirmation();
    }));
    viewer.augment_click(*viewer.inventory().at(host_item.slot), 0);
    auto socket = next_action();
    REQUIRE(socket);
    auto stale_socket = *socket;
    stale_socket.item_serial = "0000000000000002";
    CHECK_FALSE(source.apply_drive_action(stale_socket));
    REQUIRE(source.apply_drive_action(*socket));
    REQUIRE(source.confirmation());
    publish_ui();
    REQUIRE(pump(viewer_step, [&] { return viewer.confirmation().has_value(); }));
    viewer.answer_confirmation(false);
    auto cancel_augment = next_action();
    REQUIRE(cancel_augment);
    REQUIRE(source.apply_drive_action(*cancel_augment));
    CHECK_FALSE(source.confirmation());

    publish_ui();
    const auto newer_item = make_item(1005, "Newer inventory", kSlotGeneral1);
    observe.publish_zone_packet(encode_char_inventory({newer_item}));
    REQUIRE(pump(viewer_step, [&] {
        const auto* item = viewer.inventory().at(newer_item.slot);
        return item && item->id == newer_item.id;
    }));

    // A live zone packet: chat reaches the source, the tap forwards it, the viewer's GameConnection handles it.
    server.chat("A_Rat", 8, "hello viewer");
    REQUIRE(pump(source_step, [&] { return has_chat_containing(source, "hello viewer"); }));
    REQUIRE(pump([&] { viewer.update(); }, [&] { return has_chat_containing(viewer, "hello viewer"); }));
}
