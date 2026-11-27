// D1 of the drive plan (docs/research/drive-headless.md): a GameConnection's zone journal and local state replay
// into a fresh GameConnection, which is what lets a GUI start with a running headless character's state.

#include <doctest/doctest.h>

#include <memory>
#include <string>

#include "mock_server/harness.h"
#include "net/aa_packets.h"
#include "net/drive_snapshot.h"
#include "net/game_connection.h"
#include "net/group_packets.h"
#include "net/item_packets.h"
#include "net/merchant_packets.h"
#include "net/opcodes.h"
#include "net/spell_packets.h"
#include "net/trade_packets.h"
#include "net/zone_packets.h"
#include "plugin/hooks.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

TEST_CASE("drive snapshot: a journal over the packet bound is clamped, not thrown") {
    // A long session's zone stream can pass kDriveSnapshotMaxPackets; the encode must clamp (and the recording caps
    // it as truncated), because an exception here used to terminate the headless through the relay.
    DriveSnapshot snapshot;
    snapshot.character = "Gennro";
    const std::vector<std::byte> data(8, std::byte{0x11});
    snapshot.packets.reserve(kDriveSnapshotMaxPackets + 1);
    for (std::size_t i = 0; i <= kDriveSnapshotMaxPackets; ++i)
        snapshot.packets.push_back(net::AppPacket(static_cast<std::uint16_t>(0x1000), data));
    std::vector<std::byte> encoded;
    CHECK_NOTHROW(encoded = encode_drive_snapshot(snapshot));
    REQUIRE_FALSE(encoded.empty());
    const auto decoded = decode_drive_snapshot(encoded);
    REQUIRE(decoded);
    CHECK(decoded->packets.size() == kDriveSnapshotMaxPackets);
    CHECK(decoded->packets.size() < snapshot.packets.size());
}

TEST_CASE("drive snapshot: the journal keeps the latest state, not the whole history") {
    // An hours-long session is mostly repeated updates: the snapshot must be bounded by entities, not by time.
    // Each spawn's HP and position collapse to their latest packet; a pure log packet is not state.
    std::vector<AppPacket> packets;
    const std::vector<std::byte> log(4, std::byte{0x01});
    for (int i = 0; i < 200; ++i) {
        HpUpdate hp;
        hp.spawn_id = 100;
        hp.cur_hp = static_cast<std::uint32_t>(1000 + i);
        hp.max_hp = 2000;
        packets.push_back(encode(hp));
        SpawnPositionUpdate u;
        u.spawn_id = 100;
        u.motion.x = static_cast<float>(i);
        packets.push_back(encode(u));
        packets.push_back(AppPacket(to_wire(Op::ChannelMessage), log));
    }
    const auto kept = compact_journal_packets(std::move(packets));
    CHECK(kept.size() == 2); // the latest HP and the latest position only
    std::optional<HpUpdate> hp;
    std::optional<SpawnPositionUpdate> position;
    for (const auto& packet : kept) {
        CHECK(packet.opcode != to_wire(Op::ChannelMessage));
        if (packet.opcode == to_wire(Op::HPUpdate)) hp = decode_hp_update(packet);
        if (packet.opcode == to_wire(Op::ClientUpdate)) position = decode_spawn_position_update(packet);
    }
    REQUIRE(hp);
    CHECK(hp->cur_hp == 1199);
    REQUIRE(position);
    CHECK(position->motion.x == 199.0f);
}

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

TEST_CASE("drive snapshot: compaction preserves multi-slot xtargets and incremental buffs") {
    auto xtarget_one = [](std::uint32_t slot, std::uint8_t status, std::uint32_t id, const std::string& name) {
        ByteWriter w;
        w.le<std::uint32_t>(5);
        w.le<std::uint32_t>(1);
        w.le<std::uint32_t>(slot);
        w.le<std::uint8_t>(status);
        w.le<std::uint32_t>(id);
        w.fixed_string(name, name.size() + 1);
        return AppPacket(to_wire(Op::XTargetResponse), w.take());
    };
    auto buff_icons = [](std::uint32_t entity, bool all, std::uint32_t slot, std::uint32_t spell) {
        ByteWriter w;
        w.le(entity);
        w.le<std::uint32_t>(1);
        w.le<std::uint8_t>(all ? 1 : 0);
        w.le<std::uint32_t>(slot);
        w.le<std::uint32_t>(spell);
        w.le<std::uint32_t>(60);
        w.le<std::uint32_t>(0);
        w.le<std::uint32_t>(0);
        w.le<std::uint32_t>(0);
        w.le<std::uint8_t>(0); // empty caster cstring
        return AppPacket(to_wire(Op::BuffCreate), w.take());
    };

    std::vector<AppPacket> packets;
    // Three single-slot xtarget additions followed by clearing slot 0 must not wipe slots 1 and 2.
    packets.push_back(xtarget_one(0, 1, 101, "mob_a"));
    packets.push_back(xtarget_one(1, 1, 102, "mob_b"));
    packets.push_back(xtarget_one(2, 1, 103, "mob_c"));
    packets.push_back(xtarget_one(0, 0, 0, ""));
    // A full buff list followed by an incremental single-slot update must keep both.
    packets.push_back(buff_icons(10, true, 0, 200));
    packets.push_back(buff_icons(10, false, 1, 201));

    const auto kept = compact_journal_packets(std::move(packets));
    int xt_count = 0, buff_count = 0;
    for (const auto& p : kept) {
        if (p.opcode == to_wire(Op::XTargetResponse)) ++xt_count;
        if (p.opcode == to_wire(Op::BuffCreate)) ++buff_count;
    }
    CHECK(xt_count == 3);   // latest for slots 0, 1, and 2
    CHECK(buff_count == 2); // full list + incremental slot 1
}

TEST_CASE("drive snapshot: stand state, xtargets, and transient casts/dialogs survive cleanly") {
    MockEqServer server;
    EmptyGameDir dir;
    GameDir game(dir.path());
    net::ConnectionOptions options = quick_options();
    options.snapshot_journal = true;

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

    const std::uint32_t me = source.world().player_id;
    REQUIRE(me != 0);

    // Server puts the player into a sitting/looting stand state and starts a cast; the player then stands up locally
    // (which EQEmu does not echo back). A snapshot must keep the player standing and not mid-cast.
    SpawnAppearance sit;
    sit.spawn_id = static_cast<std::uint16_t>(me);
    sit.type = kAppearanceAnimation;
    sit.parameter = kStandLooting;
    source.receive_zone_packet_for_test(encode(sit));
    REQUIRE(pump(step, [&] { return source.player_status().stand_state == kStandLooting; }));

    ByteWriter cast_w;
    cast_w.le<std::uint16_t>(static_cast<std::uint16_t>(me));
    cast_w.le<std::uint16_t>(200);
    cast_w.le<std::uint32_t>(5000);
    source.receive_zone_packet_for_test(AppPacket(to_wire(Op::BeginCast), cast_w.take()));

    source.set_stand_state(kStandStanding);
    source.set_xtarget_auto_haters(true);
    auto xtarget_one = [](std::uint32_t slot, std::uint8_t status, std::uint32_t id, const std::string& name) {
        ByteWriter w;
        w.le<std::uint32_t>(5);
        w.le<std::uint32_t>(1);
        w.le<std::uint32_t>(slot);
        w.le<std::uint8_t>(status);
        w.le<std::uint32_t>(id);
        w.fixed_string(name, name.size() + 1);
        return AppPacket(to_wire(Op::XTargetResponse), w.take());
    };
    source.receive_zone_packet_for_test(xtarget_one(0, 1, 101, "a_rat01"));
    source.receive_zone_packet_for_test(xtarget_one(1, 1, 102, "a_bat02"));
    source.receive_zone_packet_for_test(xtarget_one(0, 0, 0, ""));
    REQUIRE(pump(step, [&] { return source.xtargets().slots.size() >= 2 && source.xtargets().slots[1].spawn_id == 102; }));

    const auto encoded = encode_drive_snapshot(source.capture_snapshot());
    const auto decoded = decode_drive_snapshot(encoded);
    REQUIRE(decoded);

    net::GameConnection restored(game, quick_options());
    restored.apply_snapshot(*decoded);
    CHECK(restored.player_status().stand_state == kStandStanding);
    CHECK_FALSE(restored.player_status().dead);
    CHECK_FALSE(restored.spell_state().casting.casting());
    CHECK(restored.xtargets().auto_haters);
    CHECK(restored.xtargets().zone >= 1u);
    REQUIRE(restored.xtargets().slots.size() >= 2);
    CHECK(restored.xtargets().slots[0].spawn_id == 0);
    CHECK(restored.xtargets().slots[1].spawn_id == 102);
    // Inventory interaction works immediately on the restored connection.
    REQUIRE(restored.inventory().at(ItemSlot::possessions(kSlotGeneral1)) != nullptr);
    restored.click_slot(ItemSlot::possessions(kSlotGeneral1));
    CHECK(restored.inventory().cursor() != nullptr);
}

TEST_CASE("drive snapshot: existing corpses survive compaction and open loot on a control mirror") {
    EmptyGameDir dir;
    GameDir game(dir.path());

    // Build a zone journal with the local player and an NPC that died before the viewer attached.
    Spawn me;
    me.spawn_id = 10;
    me.name = "Gennro";
    me.npc = 0;
    me.size = 6.0f;
    me.motion.x = 5.0f;
    me.motion.y = 5.0f;
    me.motion.z = 0.0f;

    Spawn mob;
    mob.spawn_id = 500;
    mob.name = "a_kobold00";
    mob.npc = 1;
    mob.size = 5.0f;
    mob.motion.x = 8.0f;
    mob.motion.y = 5.0f;
    mob.motion.z = 0.0f;

    Death death;
    death.spawn_id = 500;
    death.killer_id = 10;
    death.corpse_id = 500;
    death.damage = 42;

    std::vector<AppPacket> journal;
    journal.push_back(encode(me));
    journal.push_back(encode(mob));
    journal.push_back(encode(death));

    DriveSnapshot snapshot;
    snapshot.character = "Gennro";
    snapshot.packets = compact_journal_packets(std::move(journal));
    snapshot.position = glm::vec3(5.0f, 5.0f, 0.0f);
    snapshot.has_vitals = true;
    snapshot.stand_state = kStandStanding;
    snapshot.has_xtargets = true;
    snapshot.xtarget_zone = 1;

    GameConnection mirror(game, quick_options());
    std::vector<DriveAction> relayed;
    mirror.set_drive_control(true);
    mirror.set_drive_input([&](const DriveAction& action) { relayed.push_back(action); });
    mirror.apply_snapshot(snapshot);

    // Existing corpse from before the swap is present and marked as a corpse.
    const auto corpse_it = mirror.world().spawns.find(500);
    REQUIRE(corpse_it != mirror.world().spawns.end());
    CHECK(corpse_it->second.is_corpse);

    // Right-click loot relays DriveActionKind::Loot without leaving a stuck local session before server replies.
    mirror.loot(500);
    REQUIRE(relayed.size() == 1);
    CHECK(relayed[0].kind == DriveActionKind::Loot);
    CHECK(relayed[0].argument == 500u);
    CHECK_FALSE(mirror.loot_session().has_value());

    // Mirrored server packets open the GUI loot window, populate items, and must not echo StandState back.
    MoneyOnCorpse open_msg;
    open_msg.response = 1;
    open_msg.platinum = 2;
    mirror.receive_zone_packet_for_test(encode(open_msg));

    ItemPacket loot_item;
    loot_item.type = kItemPacketLoot;
    loot_item.item.id = 1001;
    loot_item.item.name = "Rusty Short Sword";
    loot_item.item.slot = ItemSlot::corpse(23);
    loot_item.item.stack_count = 1;
    mirror.receive_zone_packet_for_test(encode_item_packet(false, loot_item));

    MoneyOnCorpse done_msg;
    done_msg.response = 6;
    mirror.receive_zone_packet_for_test(encode(done_msg));

    REQUIRE(mirror.loot_session().has_value());
    CHECK(mirror.loot_session()->open);
    CHECK(mirror.loot_session()->complete);
    CHECK(mirror.loot_session()->corpse_id == 500u);
    CHECK(mirror.loot_session()->items.count(23) == 1);
    CHECK(mirror.player_status().stand_state == kStandLooting);
    CHECK(relayed.size() == 1); // no redundant StandState echoed back to headless

    // Loot All relays semantically to headless.
    mirror.loot_all();
    REQUIRE(relayed.size() == 2);
    CHECK(relayed[1].kind == DriveActionKind::LootAll);

    // Mirrored OP_LootItem removes the item; OP_LootComplete closes the window and stands up without echoing.
    LootItem taken;
    taken.lootee = 500;
    taken.looter = 10;
    taken.slot = 23;
    taken.auto_loot = 1;
    mirror.receive_zone_packet_for_test(encode(taken));
    REQUIRE(mirror.loot_session().has_value());
    CHECK(mirror.loot_session()->items.empty());

    mirror.receive_zone_packet_for_test(AppPacket(to_wire(Op::LootComplete), {}));
    CHECK_FALSE(mirror.loot_session().has_value());
    CHECK(mirror.player_status().stand_state == kStandStanding);
    CHECK(relayed.size() == 2);

    // Headless-initiated loot (e.g. /loot or a plugin) also opens the mirror loot window via DriveUiState.
    DriveSnapshot state;
    state.character = "Gennro";
    state.position = glm::vec3(5.0f, 5.0f, 0.0f);
    state.ui = DriveUiState{.zone_generation = 1, .inventory_revision = 0, .loot_corpse_id = 500};
    mirror.apply_local_state(state, false);
    mirror.receive_zone_packet_for_test(encode(open_msg));
    REQUIRE(mirror.loot_session().has_value());
    CHECK(mirror.loot_session()->open);
    CHECK(mirror.loot_session()->corpse_id == 500u);

    // Clearing loot_corpse_id in DriveUiState closes the mirror session if OP_LootComplete was not seen.
    state.ui->loot_corpse_id = 0;
    mirror.apply_local_state(state, false);
    CHECK_FALSE(mirror.loot_session().has_value());
}

TEST_CASE("drive snapshot: drop_on_doll and auto_inventory relay to headless and send OP_MoveItem to server") {
    MockEqServer server;
    EmptyGameDir dir;
    GameDir game(dir.path());

    net::ConnectionOptions options = quick_options();
    options.snapshot_journal = true;
    net::GameConnection source(game, std::move(options));
    auto step = [&] {
        source.update();
        server.poll();
    };
    source.start(server.ticket());
    REQUIRE(pump(step, [&] { return source.state() == ConnectionState::CharacterSelect; }));
    source.enter_world("Gennro");
    REQUIRE(pump(step, [&] { return source.state() == ConnectionState::InZone; }));
    REQUIRE(pump(step, [&] { return source.player_status().max_hp != 0; }));
    REQUIRE(source.inventory().at(ItemSlot::possessions(kSlotGeneral1)) != nullptr);

    net::GameConnection mirror(game, quick_options());
    std::vector<DriveAction> relayed;
    mirror.set_drive_control(true);
    mirror.set_drive_input([&](const DriveAction& action) { relayed.push_back(action); });
    mirror.apply_snapshot(source.capture_snapshot());

    auto sync_mirror = [&] {
        DriveSnapshot local = source.capture_local_state();
        local.ui = source.capture_drive_ui();
        mirror.apply_local_state(local, false);
    };
    sync_mirror();

    // Pick up General1 onto the cursor via the headless authority.
    mirror.click_slot(ItemSlot::possessions(kSlotGeneral1));
    REQUIRE(relayed.size() == 1);
    CHECK(relayed.back().kind == DriveActionKind::InventoryClick);
    CHECK_FALSE(mirror.inventory().cursor()); // mirror does not mutate locally before headless responds
    REQUIRE(source.apply_drive_action(relayed.back(), true));
    REQUIRE(source.inventory().cursor() != nullptr);
    const std::uint32_t held_id = source.inventory().cursor()->id;
    REQUIRE(pump(step, [&] { return MockEqServer::count(server.zone_log, Op::MoveItem) == 1; }));
    sync_mirror();
    REQUIRE(mirror.inventory().cursor() != nullptr);
    CHECK(mirror.inventory().cursor()->id == held_id);

    // Clicking the center of the inventory UI (drop_on_doll) must relay to headless, not locally clear the mirror.
    const std::uint64_t rev_before_drop = source.inventory().revision();
    mirror.drop_on_doll();
    REQUIRE(relayed.size() == 2);
    CHECK(relayed.back().kind == DriveActionKind::DropOnDoll);
    CHECK(mirror.inventory().cursor() != nullptr); // still held until headless applies and pushes state

    REQUIRE(source.apply_drive_action(relayed.back(), true));
    CHECK_FALSE(source.inventory().cursor());
    CHECK(source.inventory().revision() > rev_before_drop);
    REQUIRE(pump(step, [&] { return MockEqServer::count(server.zone_log, Op::MoveItem) == 2; }));
    auto last_move = decode_move_item(server.zone_log.back());
    REQUIRE(last_move);
    CHECK(last_move->from.same(ItemSlot::possessions(kSlotCursor)));
    CHECK_FALSE(last_move->to.same(ItemSlot::possessions(kSlotCursor)));
    sync_mirror();
    CHECK_FALSE(mirror.inventory().cursor());

    // Direct auto_inventory (e.g. tradeskill Auto-Inventory button) also relays and moves on headless.
    const ItemSlot stow_slot = ItemSlot::possessions(kSlotGeneral1);
    REQUIRE(source.inventory().at(stow_slot) != nullptr);
    mirror.click_slot(stow_slot);
    REQUIRE(source.apply_drive_action(relayed.back(), true));
    sync_mirror();
    REQUIRE(mirror.inventory().cursor() != nullptr);

    CHECK(mirror.auto_inventory());
    REQUIRE(relayed.back().kind == DriveActionKind::AutoInventory);
    CHECK(mirror.inventory().cursor() != nullptr);
    REQUIRE(source.apply_drive_action(relayed.back(), true));
    CHECK_FALSE(source.inventory().cursor());
    REQUIRE(pump(step, [&] { return MockEqServer::count(server.zone_log, Op::MoveItem) == 4; }));
    sync_mirror();
    CHECK_FALSE(mirror.inventory().cursor());

    // auto_bank also relays instead of mutating locally.
    const std::size_t before_bank = relayed.size();
    mirror.auto_bank();
    REQUIRE(relayed.size() == before_bank + 1);
    CHECK(relayed.back().kind == DriveActionKind::AutoBank);

    // Interact mode (full_control = false) accepts DropOnDoll only with matching inventory revision and held item.
    mirror.click_slot(stow_slot);
    REQUIRE(source.apply_drive_action(relayed.back(), true));
    sync_mirror();
    REQUIRE(source.inventory().cursor() != nullptr);
    DriveAction stale;
    stale.kind = DriveActionKind::DropOnDoll;
    stale.zone_generation = source.capture_drive_ui().zone_generation;
    stale.inventory_revision = source.inventory().revision() - 1;
    CHECK_FALSE(source.apply_drive_action(stale, false));
    CHECK(source.inventory().cursor() != nullptr);
    DriveAction fresh = stale;
    fresh.inventory_revision = source.inventory().revision();
    CHECK(source.apply_drive_action(fresh, false));
    CHECK_FALSE(source.inventory().cursor());
}

TEST_CASE("drive snapshot: AA table and state survive snapshots and train/percent/activate relay in drive control") {
    MockEqServer server;
    EmptyGameDir dir;
    GameDir game(dir.path());
    net::ConnectionOptions options = quick_options();
    options.snapshot_journal = true;

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
    REQUIRE(source.profile());

    // Populate AA state on the headless connection, then simulate a post-zone snapshot whose zone journal had no
    // OP_SendAATable packets by applying a clean snapshot without AA table entries and feeding them directly.
    game::AltAbility rank1;
    rank1.id = 501;
    rank1.group = 100;
    rank1.rank = 1;
    rank1.max_rank = 2;
    rank1.cost = 2;
    rank1.total_cost = 2;
    rank1.level_req = 1;
    rank1.type = 1;
    rank1.classes = 0xffffffffu;
    rank1.next_id = 502;
    rank1.spell = 1234;
    rank1.refresh = 60;
    rank1.timers = {1};

    game::AltAbility rank2 = rank1;
    rank2.id = 502;
    rank2.rank = 2;
    rank2.cost = 3;
    rank2.total_cost = 5;
    rank2.prev_id = 501;
    rank2.next_id = -1;

    // Populate AA state on the headless connection, then clear AA packets out of its zone journal (simulating
    // zoning to a second zone where OP_SendAATable is not requested again because alt_adv_.table is already loaded).
    DriveSnapshot without_aa = source.capture_snapshot();
    DriveSnapshot seeded = without_aa;
    seeded.packets.push_back(encode_send_aa_table(rank1));
    seeded.packets.push_back(encode_send_aa_table(rank2));
    RespondAA owned;
    owned.spent = 2;
    owned.assigned = {2, 0, 0, 0, 0};
    owned.owned = {{501, 2, 0}};
    seeded.packets.push_back(encode(owned));
    seeded.packets.push_back(encode(AAExpUpdate{110, 10, 50}));
    source.apply_snapshot(seeded);
    // Re-applying the AA-free snapshot replaces snapshot_journal_ with packets containing zero OP_SendAATable/
    // OP_RespondAA/OP_AAExpUpdate entries while leaving source.alt_adv_ intact (same state as after a zone change).
    std::erase_if(without_aa.packets, [](const AppPacket& p) {
        const Op op = from_wire(p.opcode);
        return op == Op::SendAATable || op == Op::RespondAA || op == Op::AAExpUpdate || op == Op::ClearAA ||
               op == Op::AAAction;
    });
    source.apply_snapshot(without_aa);
    REQUIRE(source.alt_adv().table.size() == 2);
    REQUIRE(source.alt_adv().points == 10);
    REQUIRE(source.alt_adv().percent == 50);
    REQUIRE(source.aa_can_train(502));

    DriveSnapshot snapshot = source.capture_snapshot();
    const auto encoded = encode_drive_snapshot(snapshot);
    const auto decoded = decode_drive_snapshot(encoded);
    REQUIRE(decoded);

    net::GameConnection mirror(game, quick_options());
    std::vector<DriveAction> relayed;
    mirror.set_drive_control(true);
    mirror.set_drive_input([&](const DriveAction& action) { relayed.push_back(action); });
    mirror.apply_snapshot(*decoded);

    // Mirror sees the AA table, owned rank, points, percent, and can train rank 2.
    CHECK(mirror.alt_adv().table.size() == 2);
    CHECK(mirror.alt_adv().has(501));
    CHECK(mirror.alt_adv().points == 10);
    CHECK(mirror.alt_adv().spent == 2);
    CHECK(mirror.alt_adv().percent == 50);
    CHECK_FALSE(mirror.aa_list(1, {}).empty());
    CHECK(mirror.aa_can_train(502));

    auto sync_mirror = [&] {
        DriveSnapshot local = source.capture_local_state();
        local.ui = source.capture_drive_ui();
        mirror.apply_local_state(local);
    };
    sync_mirror();

    // Clicking Train with confirmation enabled relays AATrainButton to headless -> headless opens confirmation ->
    // viewer clicks Yes -> Answer relays to headless -> headless sends OP_AAAction(kAATrain, 502).
    const std::size_t aa_before = MockEqServer::count(server.zone_log, Op::AAAction);
    mirror.aa_train_button(502, false);
    REQUIRE(relayed.size() == 1);
    CHECK(relayed.back().kind == DriveActionKind::AATrainButton);
    CHECK(relayed.back().argument == 502u);
    CHECK_FALSE(mirror.confirmation().has_value()); // headless owns the confirmation dialog

    REQUIRE(source.apply_drive_action(relayed.back(), true));
    REQUIRE(source.confirmation().has_value());
    sync_mirror();
    REQUIRE(mirror.confirmation().has_value());

    mirror.answer_confirmation(true);
    REQUIRE(relayed.size() == 2);
    CHECK(relayed.back().kind == DriveActionKind::Answer);
    REQUIRE(source.apply_drive_action(relayed.back(), true));
    REQUIRE(pump(step, [&] { return MockEqServer::count(server.zone_log, Op::AAAction) == aa_before + 1; }));
    auto train_pkt = decode_aa_action(server.zone_log.back());
    REQUIRE(train_pkt);
    CHECK(train_pkt->action == kAATrain);
    CHECK(train_pkt->ability == 502u);

    // Changing AA exp% relays AASetPercent so headless and server stay in sync.
    mirror.set_aa_percent(80);
    REQUIRE(relayed.back().kind == DriveActionKind::AASetPercent);
    CHECK(mirror.alt_adv().percent == 80);
    REQUIRE(source.apply_drive_action(relayed.back(), true));
    CHECK(source.alt_adv().percent == 80);
    REQUIRE(pump(step, [&] { return MockEqServer::count(server.zone_log, Op::AAAction) == aa_before + 2; }));
    auto pct_pkt = decode_aa_action(server.zone_log.back());
    REQUIRE(pct_pkt);
    CHECK(pct_pkt->action == kAASetPercent);
    CHECK(pct_pkt->value == 80u);

    // Activating an AA relays AAActivate to headless.
    mirror.aa_activate(100);
    REQUIRE(relayed.back().kind == DriveActionKind::AAActivate);
    CHECK(relayed.back().argument == 100u);
    REQUIRE(source.apply_drive_action(relayed.back(), true));
    REQUIRE(pump(step, [&] { return MockEqServer::count(server.zone_log, Op::AAAction) == aa_before + 3; }));
    auto act_pkt = decode_aa_action(server.zone_log.back());
    REQUIRE(act_pkt);
    CHECK(act_pkt->action == kAAActivate);
    CHECK(act_pkt->ability == 501u);
}

TEST_CASE("drive snapshot: NPC give, trade state sync, and merchant open work on a control mirror") {
    MockEqServer server;
    EmptyGameDir dir;
    GameDir game(dir.path());
    net::ConnectionOptions options = quick_options();
    options.snapshot_journal = true;

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
    REQUIRE(source.world().spawns.count(101) == 1);
    REQUIRE(source.inventory().at(ItemSlot::possessions(kSlotGeneral1)) != nullptr);

    // Move next to NPC 101 so give_to is within interact range.
    const auto npc_pos = source.world().spawns.at(101).position;
    source.set_player_motion(npc_pos, 0.0f, glm::vec3(0.0f), false);

    net::GameConnection mirror(game, quick_options());
    std::vector<DriveAction> relayed;
    mirror.set_drive_control(true);
    mirror.set_drive_input([&](const DriveAction& action) { relayed.push_back(action); });
    mirror.apply_snapshot(source.capture_snapshot());

    auto sync_mirror = [&] {
        DriveSnapshot local = source.capture_local_state();
        local.ui = source.capture_drive_ui();
        mirror.apply_local_state(local, false);
    };
    sync_mirror();

    // Pick up General1 onto the cursor via headless.
    mirror.click_slot(ItemSlot::possessions(kSlotGeneral1));
    REQUIRE(relayed.size() == 1);
    REQUIRE(source.apply_drive_action(relayed.back(), true));
    REQUIRE(source.inventory().cursor() != nullptr);
    const std::uint32_t held_id = source.inventory().cursor()->id;
    sync_mirror();
    REQUIRE(mirror.inventory().cursor() != nullptr);

    // Handing the item to NPC 101 relays GiveTo without pre-opening trade_ on the mirror.
    const std::size_t trade_req_before = MockEqServer::count(server.zone_log, Op::TradeRequest);
    mirror.give_to(101);
    REQUIRE(relayed.size() == 2);
    CHECK(relayed.back().kind == DriveActionKind::GiveTo);
    CHECK(relayed.back().argument == 101u);
    CHECK_FALSE(mirror.trade().has_value());

    REQUIRE(source.apply_drive_action(relayed.back(), true));
    REQUIRE(pump(step, [&] { return MockEqServer::count(server.zone_log, Op::TradeRequest) == trade_req_before + 1; }));

    // Server answers OP_TradeRequestAck; headless opens trade_ and drops the cursor item into trade slot 0, and
    // the mirrored packet immediately opens GiveWnd (trade_->open && trade_->with_npc) on the GUI without re-sending.
    const AppPacket ack = encode_trade_request_ack({101u, source.world().player_id});
    const std::size_t relayed_before_ack = relayed.size();
    source.receive_zone_packet_for_test(ack);
    mirror.receive_zone_packet_for_test(ack);

    REQUIRE(source.trade().has_value());
    CHECK(source.trade()->open);
    CHECK(source.trade()->with_npc);
    CHECK_FALSE(source.inventory().cursor());
    REQUIRE(source.inventory().at(ItemSlot{kInvTrade, 0, -1, -1, 0, 0}) != nullptr);
    CHECK(source.inventory().at(ItemSlot{kInvTrade, 0, -1, -1, 0, 0})->id == held_id);

    REQUIRE(mirror.trade().has_value());
    CHECK(mirror.trade()->open);
    CHECK(mirror.trade()->with_npc);
    CHECK(mirror.trade()->partner_id == 101u);
    CHECK_FALSE(mirror.trade()->partner_name.empty());
    CHECK(relayed.size() == relayed_before_ack); // no redundant packets/actions echoed from mirror

    // Pushing local state syncs the trade slot item and authoritative DriveUiState::trade to the mirror.
    sync_mirror();
    CHECK_FALSE(mirror.inventory().cursor());
    REQUIRE(mirror.inventory().at(ItemSlot{kInvTrade, 0, -1, -1, 0, 0}) != nullptr);
    CHECK(mirror.inventory().at(ItemSlot{kInvTrade, 0, -1, -1, 0, 0})->id == held_id);

    // Clicking Give relays TradeAccept and marks my_ready on both mirror and headless.
    const std::size_t accept_before = MockEqServer::count(server.zone_log, Op::TradeAcceptClick);
    mirror.trade_accept();
    REQUIRE(relayed.size() == relayed_before_ack + 1);
    CHECK(relayed.back().kind == DriveActionKind::TradeAccept);
    CHECK(mirror.trade()->my_ready);
    REQUIRE(source.apply_drive_action(relayed.back(), true));
    REQUIRE(pump(step, [&] { return MockEqServer::count(server.zone_log, Op::TradeAcceptClick) == accept_before + 1; }));

    // OP_FinishTrade closes the trade window on both sides.
    const AppPacket finish(to_wire(Op::FinishTrade), {});
    source.receive_zone_packet_for_test(finish);
    mirror.receive_zone_packet_for_test(finish);
    CHECK_FALSE(source.trade().has_value());
    CHECK_FALSE(mirror.trade().has_value());
    CHECK(mirror.inventory().at(ItemSlot{kInvTrade, 0, -1, -1, 0, 0}) == nullptr);

    // Mid-trade attach / headless-initiated trade and cancel via DriveUiState::trade and OP_CancelTrade.
    DriveSnapshot mid_state = source.capture_local_state();
    mid_state.ui = DriveUiState{
        .zone_generation = 1,
        .inventory_revision = source.inventory().revision(),
        .trade = DriveTradeState{
            .partner_id = 101u,
            .partner_name = "A Rat",
            .with_npc = true,
            .my_ready = false,
            .their_ready = false,
            .my_coins = {0, 0, 5, 10},
            .their_coins = {0, 0, 0, 0},
        },
    };
    mirror.apply_local_state(mid_state, false);
    REQUIRE(mirror.trade().has_value());
    CHECK(mirror.trade()->open);
    CHECK(mirror.trade()->with_npc);
    CHECK(mirror.trade()->my_coins[3] == 10u);

    // Mirrored OP_CancelTrade closes the mirror's trade without relaying a duplicate TradeCancel.
    const std::size_t relayed_before_cancel = relayed.size();
    mirror.receive_zone_packet_for_test(encode(CancelTrade{101u, source.world().player_id}));
    CHECK_FALSE(mirror.trade().has_value());
    CHECK(relayed.size() == relayed_before_cancel);

    // Clearing trade in DriveUiState also closes an open mirror trade if OP_CancelTrade/OP_FinishTrade was missed.
    mirror.apply_local_state(mid_state, false);
    REQUIRE(mirror.trade().has_value());
    mid_state.ui->trade.reset();
    mirror.apply_local_state(mid_state, false);
    CHECK_FALSE(mirror.trade().has_value());

    // Mirrored OP_ShopRequest opens MerchantWnd on the control mirror even though open_merchant only relayed.
    mirror.open_merchant(101u);
    CHECK(relayed.back().kind == DriveActionKind::OpenMerchant);
    CHECK_FALSE(mirror.merchant().has_value());
    MerchantClick shop_open;
    shop_open.npc_id = 101u;
    shop_open.player_id = source.world().player_id;
    shop_open.command = 1;
    shop_open.rate = 1.05f;
    shop_open.tab_display = 1;
    mirror.receive_zone_packet_for_test(encode(shop_open));
    REQUIRE(mirror.merchant().has_value());
    CHECK(mirror.merchant()->open);
    CHECK(mirror.merchant()->npc_id == 101u);
    mirror.close_merchant();
    CHECK(relayed.back().kind == DriveActionKind::CloseMerchant);
    CHECK_FALSE(mirror.merchant().has_value());
}
