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

TEST_CASE("drive observe: typed actions reject truncated messages and unknown kinds") {
    DriveAction action;
    action.kind = DriveActionKind::AugmentClick;
    action.slot = ItemSlot::possessions(kSlotPrimary);
    action.socket = 2;
    action.zone_generation = 3;
    action.inventory_revision = 4;
    action.item_serial = "0000000000000001";
    auto bytes = app::encode_drive_action(action);
    auto read = app::decode_drive_action(bytes);
    REQUIRE(read);
    CHECK(read->kind == action.kind);
    CHECK(read->slot.same(action.slot));
    CHECK(read->socket == action.socket);
    CHECK(read->zone_generation == action.zone_generation);
    CHECK(read->inventory_revision == action.inventory_revision);
    CHECK(read->item_serial == action.item_serial);
    for (std::size_t size = 0; size < bytes.size(); ++size)
        CHECK_FALSE(app::decode_drive_action(std::span(bytes).first(size)));
    bytes[1] = std::byte{255};
    CHECK_FALSE(app::decode_drive_action(bytes));
}

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

    REQUIRE(viewer_pipe->send_message(app::encode_drive_action({})));
    // A passive endpoint drains client input without offering it to the game thread.
    CHECK(observe.take_actions().empty());
    viewer_pipe->close();
}

TEST_CASE("drive observe: control actions, move, forward filter, chat, and extended state round-trip") {
    DriveAction cmd;
    cmd.kind = DriveActionKind::Command;
    cmd.zone_generation = 3;
    cmd.text = "/say hello crew";
    auto cmd_bytes = app::encode_drive_action(cmd);
    auto cmd_out = app::decode_drive_action(cmd_bytes);
    REQUIRE(cmd_out);
    CHECK(cmd_out->kind == DriveActionKind::Command);
    CHECK(cmd_out->zone_generation == 3);
    CHECK(cmd_out->text == "/say hello crew");

    DriveMove move;
    move.position = glm::vec3(12.5f, -45.0f, 3.25f);
    move.heading = 192.0f;
    move.velocity = glm::vec3(0.5f, 0.25f, 0.0f);
    move.speed_run = 0.7f;
    move.moving = true;
    move.stand_state = kStandStanding;
    move.standing_on = 42;
    move.active = true;
    auto move_bytes = app::encode_drive_move(move);
    auto move_out = app::decode_drive_move(move_bytes);
    REQUIRE(move_out);
    CHECK(move_out->position.x == doctest::Approx(12.5f));
    CHECK(move_out->heading == doctest::Approx(192.0f));
    CHECK(move_out->moving);
    CHECK(move_out->active);
    CHECK(move_out->standing_on == 42);

    // Filtered packet forwarding: session/position/inventory opcodes are blocked; AA/task/etc. are allowed.
    CHECK_FALSE(drive_forward_allowed(AppPacket(to_wire(Op::ClientUpdate), {}), false));
    CHECK_FALSE(drive_forward_allowed(AppPacket(to_wire(Op::MoveItem), {}), false));
    CHECK_FALSE(drive_forward_allowed(AppPacket(to_wire(Op::Camp), {}), false));
    CHECK_FALSE(drive_forward_allowed(AppPacket(to_wire(Op::LootRequest), {}), false));
    CHECK_FALSE(drive_forward_allowed(AppPacket(to_wire(Op::LootItem), {}), false));
    CHECK_FALSE(drive_forward_allowed(AppPacket(to_wire(Op::EndLootRequest), {}), false));
    CHECK_FALSE(drive_forward_allowed(AppPacket(to_wire(Op::TradeRequest), {}), false));
    CHECK_FALSE(drive_forward_allowed(AppPacket(to_wire(Op::TradeRequestAck), {}), false));
    CHECK_FALSE(drive_forward_allowed(AppPacket(to_wire(Op::TradeBusy), {}), false));
    CHECK_FALSE(drive_forward_allowed(AppPacket(to_wire(Op::TradeAcceptClick), {}), false));
    CHECK_FALSE(drive_forward_allowed(AppPacket(to_wire(Op::CancelTrade), {}), false));
    CHECK_FALSE(drive_forward_allowed(AppPacket(to_wire(Op::ShopRequest), {}), false));
    CHECK_FALSE(drive_forward_allowed(AppPacket(to_wire(Op::ShopPlayerBuy), {}), false));
    CHECK_FALSE(drive_forward_allowed(AppPacket(to_wire(Op::ShopPlayerSell), {}), false));
    CHECK_FALSE(drive_forward_allowed(AppPacket(to_wire(Op::ShopEnd), {}), false));
    CHECK_FALSE(drive_forward_allowed(AppPacket(to_wire(Op::SendLoginInfo), {}), true));
    CHECK(drive_forward_allowed(AppPacket(to_wire(Op::AAAction), {}), false));

    DriveAction loot_all;
    loot_all.kind = DriveActionKind::LootAll;
    loot_all.zone_generation = 2;
    auto loot_all_out = app::decode_drive_action(app::encode_drive_action(loot_all));
    REQUIRE(loot_all_out);
    CHECK(loot_all_out->kind == DriveActionKind::LootAll);
    CHECK(loot_all_out->zone_generation == 2);

    for (DriveActionKind kind :
         {DriveActionKind::AutoInventory, DriveActionKind::DropOnDoll, DriveActionKind::AutoBank,
          DriveActionKind::AATrain, DriveActionKind::AATrainButton, DriveActionKind::AAActivate,
          DriveActionKind::AASetPercent}) {
        DriveAction act;
        act.kind = kind;
        act.argument = 502;
        act.socket = 1;
        act.zone_generation = 5;
        act.inventory_revision = 11;
        auto out = app::decode_drive_action(app::encode_drive_action(act));
        REQUIRE(out);
        CHECK(out->kind == kind);
        CHECK(out->argument == 502);
        CHECK(out->socket == 1);
        CHECK(out->zone_generation == 5);
        CHECK(out->inventory_revision == 11);
    }

    app::DriveForwardedPacket fwd{AppPacket(to_wire(Op::AAAction), {std::byte{1}, std::byte{2}}), true, false};
    auto fwd_out = app::decode_drive_forward(app::encode_drive_forward(fwd));
    REQUIRE(fwd_out);
    CHECK(fwd_out->packet.opcode == to_wire(Op::AAAction));
    CHECK(fwd_out->packet.data.size() == 2);
    CHECK(fwd_out->reliable);
    CHECK_FALSE(fwd_out->world);

    app::DriveChatLine line{2, 0x112233, 275, "Sender", "hello from ucs"};
    auto chat_out = app::decode_drive_chat(app::encode_drive_chat(line));
    REQUIRE(chat_out);
    CHECK(chat_out->kind == 2);
    CHECK(chat_out->rgb == 0x112233);
    CHECK(chat_out->color == 275);
    CHECK(chat_out->peer == "Sender");
    CHECK(chat_out->text == "hello from ucs");

    app::DriveLocalState state;
    state.position = glm::vec3(1.0f, 2.0f, 3.0f);
    state.target = 77;
    state.ui = DriveUiState{
        .zone_generation = 4,
        .inventory_revision = 9,
        .loot_corpse_id = 4001,
        .trade = DriveTradeState{
            .partner_id = 101,
            .partner_name = "Quest Giver",
            .with_npc = true,
            .my_ready = true,
            .their_ready = false,
            .my_coins = {1, 2, 3, 4},
            .their_coins = {5, 6, 7, 8},
        },
    };
    state.has_status = true;
    state.stand_state = kStandStanding;
    state.auto_attack = true;
    state.hp = 900;
    state.max_hp = 1000;
    state.has_xtargets = true;
    state.xtarget_auto_haters = true;
    state.xtarget_zone = 4;
    state.xtarget_slots.push_back({1, 1, 55, "a_rat00"});
    auto state_out = app::decode_drive_state(app::encode_drive_state(state));
    REQUIRE(state_out);
    REQUIRE(state_out->ui);
    CHECK(state_out->ui->loot_corpse_id == 4001u);
    REQUIRE(state_out->ui->trade);
    CHECK(state_out->ui->trade->partner_id == 101u);
    CHECK(state_out->ui->trade->partner_name == "Quest Giver");
    CHECK(state_out->ui->trade->with_npc);
    CHECK(state_out->ui->trade->my_ready);
    CHECK_FALSE(state_out->ui->trade->their_ready);
    CHECK(state_out->ui->trade->my_coins == std::array<std::uint32_t, 4>{1, 2, 3, 4});
    CHECK(state_out->ui->trade->their_coins == std::array<std::uint32_t, 4>{5, 6, 7, 8});
    CHECK(state_out->has_status);
    CHECK(state_out->auto_attack);
    CHECK(state_out->hp == 900);
    CHECK(state_out->has_xtargets);
    CHECK(state_out->xtarget_auto_haters);
    REQUIRE(state_out->xtarget_slots.size() == 1);
    CHECK(state_out->xtarget_slots[0].spawn_id == 55);
    CHECK(state_out->xtarget_slots[0].name == "a_rat00");
}

TEST_CASE("drive observe: remote imgui input, frame, texture, and crew command round-trip") {
    ui::RemoteImGuiInput in;
    in.display_w = 1920.0f;
    in.display_h = 1080.0f;
    in.fb_scale_x = 1.5f;
    in.fb_scale_y = 1.5f;
    in.ui_scale = 1.25f;
    in.dt = 1.0f / 60.0f;
    in.mouse_x = 420.5f;
    in.mouse_y = 310.25f;
    in.wheel_x = 0.0f;
    in.wheel_y = -2.0f;
    in.mods = 3u;
    in.mouse_down_mask = 0x05u;
    in.has_mouse_down_mask = true;
    in.mouse_valid = true;
    in.focused = true;
    in.mouse_buttons.push_back({0, true});
    in.mouse_buttons.push_back({0, false});
    in.keys.push_back({513, true});
    in.chars.push_back('A');
    auto in_bytes = app::encode_drive_imgui_input(in);
    auto in_out = app::decode_drive_imgui_input(in_bytes);
    REQUIRE(in_out);
    CHECK(in_out->display_w == doctest::Approx(1920.0f));
    CHECK(in_out->ui_scale == doctest::Approx(1.25f));
    CHECK(in_out->mouse_x == doctest::Approx(420.5f));
    CHECK(in_out->wheel_y == doctest::Approx(-2.0f));
    CHECK(in_out->mods == 3u);
    CHECK(in_out->has_mouse_down_mask);
    CHECK(in_out->mouse_down_mask == 0x05u);
    CHECK(in_out->mouse_valid);
    CHECK(in_out->focused);
    REQUIRE(in_out->mouse_buttons.size() == 2);
    CHECK(in_out->mouse_buttons[0].down);
    CHECK_FALSE(in_out->mouse_buttons[1].down);
    REQUIRE(in_out->keys.size() == 1);
    CHECK(in_out->keys[0].key == 513);
    REQUIRE(in_out->chars.size() == 1);
    CHECK(in_out->chars[0] == 'A');
    CHECK_FALSE(app::decode_drive_imgui_input(std::span(in_bytes).first(in_bytes.size() - 1)));
    in_bytes[0] = std::byte{1};
    CHECK_FALSE(app::decode_drive_imgui_input(in_bytes));

    ui::RemoteImGuiFrame frame;
    frame.display_w = 1920.0f;
    frame.display_h = 1080.0f;
    frame.fb_scale_x = 1.0f;
    frame.fb_scale_y = 1.0f;
    frame.want_capture_mouse = true;
    frame.want_capture_keyboard = false;
    frame.want_text_input = true;
    frame.mouse_cursor = 2;
    ui::RemoteDrawList dl;
    dl.vertices.push_back({10.0f, 20.0f, 0.25f, 0.5f, 0xffffffffu});
    dl.vertices.push_back({30.0f, 20.0f, 0.75f, 0.5f, 0xff00ff00u});
    dl.vertices.push_back({30.0f, 40.0f, 0.75f, 0.75f, 0xffff0000u});
    dl.indices = {0, 1, 2};
    dl.cmds.push_back({0.0f, 0.0f, 200.0f, 100.0f, 7u, 0u, 0u, 3u});
    frame.lists.push_back(std::move(dl));
    auto frame_bytes = app::encode_drive_imgui_frame(frame);
    auto frame_out = app::decode_drive_imgui_frame(frame_bytes);
    REQUIRE(frame_out);
    CHECK(frame_out->want_capture_mouse);
    CHECK_FALSE(frame_out->want_capture_keyboard);
    CHECK(frame_out->want_text_input);
    CHECK(frame_out->mouse_cursor == 2);
    REQUIRE(frame_out->lists.size() == 1);
    REQUIRE(frame_out->lists[0].vertices.size() == 3);
    CHECK(frame_out->lists[0].vertices[1].col == 0xff00ff00u);
    REQUIRE(frame_out->lists[0].indices.size() == 3);
    REQUIRE(frame_out->lists[0].cmds.size() == 1);
    CHECK(frame_out->lists[0].cmds[0].tex_id == 7u);
    CHECK_FALSE(app::decode_drive_imgui_frame(std::span(frame_bytes).first(frame_bytes.size() - 1)));

    ui::RemoteImGuiTexture tex;
    tex.id = 7u;
    tex.tex_w = 16;
    tex.tex_h = 8;
    tex.x = 2;
    tex.y = 1;
    tex.w = 4;
    tex.h = 2;
    tex.full = false;
    tex.destroy = false;
    tex.rgba.assign(4u * 2u * 4u, std::byte{0xab});
    auto tex_bytes = app::encode_drive_imgui_texture(tex);
    auto tex_out = app::decode_drive_imgui_texture(tex_bytes);
    REQUIRE(tex_out);
    CHECK(tex_out->id == 7u);
    CHECK(tex_out->tex_w == 16);
    CHECK(tex_out->tex_h == 8);
    CHECK(tex_out->w == 4);
    CHECK(tex_out->h == 2);
    CHECK_FALSE(tex_out->full);
    CHECK_FALSE(tex_out->destroy);
    CHECK(tex_out->rgba.size() == 32u);

    // Reject out-of-bounds texture rect, empty update rect, and non-empty destroy payload.
    ui::RemoteImGuiTexture bad = tex;
    bad.x = 15;
    CHECK_FALSE(app::decode_drive_imgui_texture(app::encode_drive_imgui_texture(bad)));
    ui::RemoteImGuiTexture empty_rect = tex;
    empty_rect.h = 0;
    empty_rect.rgba.clear();
    CHECK_FALSE(app::decode_drive_imgui_texture(app::encode_drive_imgui_texture(empty_rect)));
    ui::RemoteImGuiTexture destroy_tex;
    destroy_tex.id = 7u;
    destroy_tex.destroy = true;
    auto destroy_out = app::decode_drive_imgui_texture(app::encode_drive_imgui_texture(destroy_tex));
    REQUIRE(destroy_out);
    CHECK(destroy_out->destroy);
    CHECK(destroy_out->rgba.empty());
    destroy_tex.rgba.push_back(std::byte{1});
    CHECK_FALSE(app::decode_drive_imgui_texture(app::encode_drive_imgui_texture(destroy_tex)));

    auto crew_bytes = app::encode_drive_crew_command("release");
    auto crew_out = app::decode_drive_crew_command(crew_bytes);
    REQUIRE(crew_out);
    CHECK(*crew_out == "release");
    CHECK_FALSE(app::decode_drive_crew_command(std::span(crew_bytes).first(1)));
}

TEST_CASE("drive observe: viewer closing before snapshot does not wedge later viewers") {
    app::DriveObserveServer observe(platform::make_local_pipe_session(), true, true);

    // First viewer connects and disconnects before any snapshot is supplied (e.g. headless zoning or stalled).
    {
        auto abandoned = platform::LocalPipeConnection::connect(observe.session(), 5000);
        const auto wait_end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!observe.needs_snapshot() && std::chrono::steady_clock::now() < wait_end)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        REQUIRE(observe.needs_snapshot());
        abandoned->close();
    }

    // The server must notice the closed viewer, drop the pending snapshot request, and accept a new viewer.
    const auto clear_end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (observe.needs_snapshot() && std::chrono::steady_clock::now() < clear_end)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK_FALSE(observe.needs_snapshot());

    auto second = platform::LocalPipeConnection::connect(observe.session(), 5000);
    const auto second_end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!observe.needs_snapshot() && std::chrono::steady_clock::now() < second_end)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(observe.needs_snapshot());

    DriveSnapshot snap;
    snap.character = "Ashenra";
    observe.set_snapshot(snap);

    auto received = read_snapshot(*second);
    REQUIRE(received);
    CHECK(received->character == "Ashenra");
    second->close();
}

TEST_CASE("local pipe: concurrent send, receive and close terminate cleanly") {
    platform::LocalPipeServer server(platform::make_local_pipe_session());
    std::unique_ptr<platform::LocalPipeConnection> accepted;
    std::thread accept_thread([&] { accepted = server.accept(5000); });
    auto client = platform::LocalPipeConnection::connect(server.session(), 5000);
    accept_thread.join();
    REQUIRE(accepted);

    const std::vector<std::byte> payload(4096, std::byte{0x5a});
    std::thread writer([&] {
        for (int i = 0; i < 200; ++i) {
            if (!accepted->send_message(payload, 100)) break;
        }
    });
    for (int i = 0; i < 10; ++i) (void)client->receive_message(100);
    client->close();
    writer.join();
    accepted->close();
    CHECK(client->closed());
    CHECK(accepted->closed());
}

TEST_CASE("remote imgui: texture band sizing never truncates to zero on narrow glyph rects") {
    CHECK(ui::remote_texture_band_rows(0, 13) == 0);
    CHECK(ui::remote_texture_band_rows(4, 0) == 0);
    // Narrow widths (1, 2, 4, 8, 16) divide 4 MiB into exact multiples of 65536 rows; band sizing must stay >= 1.
    for (std::uint16_t rw : {std::uint16_t{1}, std::uint16_t{2}, std::uint16_t{3}, std::uint16_t{4},
                             std::uint16_t{8}, std::uint16_t{15}, std::uint16_t{16}, std::uint16_t{17},
                             std::uint16_t{64}, std::uint16_t{1024}, std::uint16_t{4096}}) {
        for (std::uint16_t rh : {std::uint16_t{1}, std::uint16_t{13}, std::uint16_t{256}, std::uint16_t{4096},
                                 std::uint16_t{65535}}) {
            const std::size_t band = ui::remote_texture_band_rows(rw, rh);
            REQUIRE(band >= 1u);
            REQUIRE(band <= rh);
            std::size_t covered = 0;
            std::size_t bands = 0;
            for (std::size_t y_off = 0; y_off < rh; y_off += band) {
                const std::size_t bh = std::min<std::size_t>(band, rh - y_off);
                CHECK(bh >= 1u);
                CHECK(static_cast<std::size_t>(rw) * bh * 4u <= (std::size_t{4} << 20));
                covered += bh;
                ++bands;
            }
            CHECK(covered == rh);
            CHECK(bands >= 1u);
        }
    }
    CHECK(ui::remote_texture_band_rows(4, 13) == 13);
    CHECK(ui::remote_texture_band_rows(4096, 4096) == 256);
}

TEST_CASE("drive observe: coalesced remote imgui input preserves edges and latest mouse_down_mask") {
    app::DriveObserveServer observe(platform::make_local_pipe_session(), true, true);
    auto client = platform::LocalPipeConnection::connect(observe.session(), 5000);
    const auto wait_end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!observe.needs_snapshot() && std::chrono::steady_clock::now() < wait_end)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(observe.needs_snapshot());

    net::DriveSnapshot snap;
    snap.character = "Ashenra";
    observe.set_snapshot(snap);
    REQUIRE(read_snapshot(*client));

    ui::RemoteImGuiInput down;
    down.display_w = 1920.0f;
    down.display_h = 1080.0f;
    down.mouse_x = 120.0f;
    down.mouse_y = 240.0f;
    down.mouse_valid = true;
    down.focused = true;
    down.mouse_down_mask = 1u;
    down.has_mouse_down_mask = true;
    down.mouse_buttons.push_back({0, true});

    ui::RemoteImGuiInput up = down;
    up.mouse_x = 125.0f;
    up.mouse_y = 245.0f;
    up.mouse_down_mask = 0u;
    up.mouse_buttons = {{0, false}};

    REQUIRE(client->send_message(app::encode_drive_imgui_input(down), 500));
    REQUIRE(client->send_message(app::encode_drive_imgui_input(up), 500));

    std::vector<ui::RemoteMouseButtonEvent> seen_buttons;
    ui::RemoteImGuiInput latest{};
    const auto drain_end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (seen_buttons.size() < 2 && std::chrono::steady_clock::now() < drain_end) {
        if (auto got = observe.take_imgui_input()) {
            latest = *got;
            seen_buttons.insert(seen_buttons.end(), got->mouse_buttons.begin(), got->mouse_buttons.end());
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    REQUIRE(seen_buttons.size() == 2);
    CHECK(seen_buttons[0].button == 0);
    CHECK(seen_buttons[0].down);
    CHECK(seen_buttons[1].button == 0);
    CHECK_FALSE(seen_buttons[1].down);
    CHECK(latest.has_mouse_down_mask);
    CHECK(latest.mouse_down_mask == 0u);
    CHECK(latest.mouse_x == doctest::Approx(125.0f));
    client->close();
}
