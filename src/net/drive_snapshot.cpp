#include "net/drive_snapshot.h"

#include "core/byte_io.h"
#include "net/group_packets.h"
#include "net/item_packets.h"
#include "net/spell_packets.h"
#include "net/zone_packets.h"

#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace hailstone::net {
namespace {

constexpr std::uint32_t kMagic = 0x31534448; // HDS1
constexpr std::uint16_t kVersion = 2;
constexpr std::size_t kMaxPackets = kDriveSnapshotMaxPackets;
constexpr std::size_t kMaxXTargetSlots = 64;

// A zone journal is a history; a snapshot is a state. Zone and entity construction stay in full (they define the
// world). Updates that carry the current value of one thing collapse to their latest occurrence, keyed by the
// entity and slot they change. Pure log, visual, and transient interaction packets are not state and go.
enum class JournalPolicy : std::uint8_t { KeepAll, Latest, Drop };

JournalPolicy journal_policy(Op op) {
    switch (op) {
    case Op::SpecialMesg:
    case Op::FormattedMessage:
    case Op::SimpleMessage:
    case Op::ChannelMessage:
    case Op::Emote:
    case Op::Action:
    case Op::Damage:
    case Op::Animation:
    case Op::SomeItemPacketMaybe:
    case Op::BeginCast:
    case Op::InterruptCast:
    case Op::SpellEffect:
    case Op::ColoredText:
    case Op::Sound:
    case Op::CashReward:
    case Op::Consider:
    case Op::RandomReply:
    case Op::WhoAllResponse:
    case Op::LootRequest:
    case Op::LootItem:
    case Op::LootComplete:
    case Op::MoneyOnCorpse:
    case Op::ShopRequest:
    case Op::ShopEnd:
    case Op::ShopEndConfirm:
    case Op::ShopPlayerBuy:
    case Op::ShopPlayerSell:
    case Op::ShopDelItem:
    case Op::TradeRequest:
    case Op::TradeRequestAck:
    case Op::TradeAcceptClick:
    case Op::TradeCoins:
    case Op::TradeBusy:
    case Op::CancelTrade:
    case Op::FinishTrade:
    case Op::FinishWindow:
    case Op::FinishWindow2:
    case Op::RezzRequest:
    case Op::Translocate:
    case Op::SharedTaskInvite:
    case Op::ItemLinkResponse:
    case Op::ItemVerifyReply:
    case Op::RequestClientZoneChange:
    case Op::ZoneChange:
        return JournalPolicy::Drop;
    case Op::ClientUpdate:
    case Op::NPCMoveUpdate:
    case Op::HPUpdate:
    case Op::SpawnAppearance:
    case Op::Buff:
    case Op::Death:
    case Op::ManaChange:
    case Op::Stamina:
    case Op::TimeOfDay:
    case Op::Weather:
    case Op::ExpUpdate:
    case Op::AAExpUpdate:
    case Op::RespondAA:
    case Op::LevelAppearance:
    case Op::PlayerProfile:
    case Op::CharInventory:
    case Op::XTargetResponse:
    case Op::TargetCommand:
    case Op::TargetHoTT:
    case Op::PetHoTT:
        return JournalPolicy::Latest;
    default:
        return JournalPolicy::KeepAll;
    }
}

bool is_buff_icons_op(Op op) {
    return op == Op::BuffCreate || op == Op::TargetBuffs || op == Op::PetBuffWindow;
}

bool drop_transient_item_packet(const AppPacket& p, Op op) {
    if (op != Op::ItemPacket) return false;
    auto ip = decode_item_packet(p);
    if (!ip) return false;
    switch (ip->type) {
    case kItemPacketViewLink:
    case kItemPacketMerchant:
    case kItemPacketTradeView:
    case kItemPacketLoot:
    case kItemPacketParcel:
        return true;
    default:
        return false;
    }
}

// The entity/slot an update belongs to. A failed decode returns nullopt, and the packet is kept (never lose state
// because a layout was not what this compaction expected).
std::optional<std::uint64_t> journal_key(const AppPacket& p, Op op) {
    const auto key = [&](std::uint32_t a, std::uint32_t b) {
        return (static_cast<std::uint64_t>(p.opcode) << 48) | (static_cast<std::uint64_t>(a & 0xffffffu) << 24) |
               (b & 0xffffffu);
    };
    switch (op) {
    case Op::ClientUpdate:
        if (auto u = decode_spawn_position_update(p)) return key(u->spawn_id, 0);
        return std::nullopt;
    case Op::NPCMoveUpdate:
        if (auto u = decode_npc_move_update(p)) return key(u->spawn_id, 0);
        return std::nullopt;
    case Op::HPUpdate:
        if (auto h = decode_hp_update(p)) return key(static_cast<std::uint16_t>(h->spawn_id), 0);
        return std::nullopt;
    case Op::SpawnAppearance:
        if (auto a = decode_spawn_appearance(p)) return key(a->spawn_id, a->type);
        return std::nullopt;
    case Op::Buff:
        if (auto b = decode_buff(p)) return key(b->entity_id, b->slot);
        return std::nullopt;
    case Op::Death:
        if (auto d = decode_death(p)) return key(d->spawn_id, 0);
        return std::nullopt;
    case Op::XTargetResponse:
        // EQEmu sends per-slot deltas (entries.size() == 1); collapsing all responses into one key drops every other
        // slot. Collapse only single-slot updates by slot index; keep multi-slot updates in full.
        if (auto x = decode_xtarget_response(p); x && x->entries.size() == 1 && x->entries[0].slot >= 0)
            return key(static_cast<std::uint32_t>(x->entries[0].slot), 1);
        return std::nullopt;
    default:
        return key(0, 0); // one value for the whole connection
    }
}

void put_packet(ByteWriter& w, const AppPacket& packet) {
    w.le(packet.opcode);
    w.le(static_cast<std::uint32_t>(packet.data.size()));
    w.bytes(packet.data);
}

AppPacket read_packet(ByteReader& r) {
    AppPacket packet;
    packet.opcode = r.le<std::uint16_t>();
    const auto size = r.le<std::uint32_t>();
    if (size > kDriveSnapshotMaxBytes) throw std::out_of_range("drive snapshot packet is too large");
    const auto data = r.bytes(size);
    packet.data.assign(data.begin(), data.end());
    return packet;
}

void put_string(ByteWriter& w, const std::string& text) {
    if (text.size() > std::numeric_limits<std::uint16_t>::max())
        throw std::out_of_range("drive snapshot text is too long");
    w.le(static_cast<std::uint16_t>(text.size()));
    w.bytes(std::span(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

std::string read_string(ByteReader& r) {
    const auto size = r.le<std::uint16_t>();
    const auto data = r.bytes(size);
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

} // namespace

std::vector<AppPacket> compact_journal_packets(std::vector<AppPacket> packets) {
    std::unordered_map<std::uint64_t, std::size_t> latest;
    latest.reserve(packets.size() / 4 + 1);
    // BuffCreate / TargetBuffs / PetBuffWindow carry either a full list (all_buffs == true) or a delta for specific
    // slots (all_buffs == false). Track the latest full list per (opcode, entity_id), and collapse single-slot deltas
    // after it per (opcode, entity_id, slot) so incremental updates never wipe out earlier buffs.
    std::unordered_map<std::uint64_t, std::size_t> latest_full_buff;
    std::unordered_map<std::uint64_t, std::size_t> latest_buff_slot;
    const auto buff_entity_key = [](std::uint16_t opcode, std::uint32_t entity) {
        return (static_cast<std::uint64_t>(opcode) << 48) | static_cast<std::uint64_t>(entity);
    };
    const auto buff_slot_key = [](std::uint16_t opcode, std::uint32_t entity, std::uint32_t slot) {
        return (static_cast<std::uint64_t>(opcode) << 48) | (static_cast<std::uint64_t>(entity & 0xffffffu) << 24) |
               (slot & 0xffffffu);
    };
    for (std::size_t i = 0; i < packets.size(); ++i) {
        const Op op = from_wire(packets[i].opcode);
        if (is_buff_icons_op(op)) {
            if (auto bi = decode_buff_icons(packets[i])) {
                if (bi->all_buffs) latest_full_buff[buff_entity_key(packets[i].opcode, bi->entity_id)] = i;
                else if (bi->entries.size() == 1)
                    latest_buff_slot[buff_slot_key(packets[i].opcode, bi->entity_id, bi->entries[0].slot)] = i;
            }
            continue;
        }
        if (journal_policy(op) != JournalPolicy::Latest) continue;
        if (auto key = journal_key(packets[i], op)) latest[*key] = i;
    }
    std::vector<AppPacket> kept;
    kept.reserve(latest.size() + 64);
    for (std::size_t i = 0; i < packets.size(); ++i) {
        const Op op = from_wire(packets[i].opcode);
        if (is_buff_icons_op(op)) {
            if (auto bi = decode_buff_icons(packets[i])) {
                const auto ek = buff_entity_key(packets[i].opcode, bi->entity_id);
                if (auto it = latest_full_buff.find(ek); it != latest_full_buff.end() && i < it->second) continue;
                if (!bi->all_buffs && bi->entries.size() == 1) {
                    const auto sk = buff_slot_key(packets[i].opcode, bi->entity_id, bi->entries[0].slot);
                    if (latest_buff_slot[sk] != i) continue;
                }
            }
            kept.push_back(std::move(packets[i]));
            continue;
        }
        const JournalPolicy policy = journal_policy(op);
        if (policy == JournalPolicy::Drop || drop_transient_item_packet(packets[i], op)) continue;
        if (policy == JournalPolicy::Latest) {
            if (auto key = journal_key(packets[i], op); key && latest[*key] != i) continue;
        }
        kept.push_back(std::move(packets[i]));
    }
    return kept;
}

std::vector<std::byte> encode_drive_snapshot(const DriveSnapshot& snapshot) {
    // Clamp rather than throw: the recording cap sets `truncated`, and an exception here used to terminate the
    // headless through the relay's uncaught path.
    const std::size_t count = std::min<std::size_t>(snapshot.packets.size(), kMaxPackets);
    ByteWriter w;
    w.le(kMagic);
    w.le(kVersion);
    put_string(w, snapshot.character);
    w.le(static_cast<std::uint32_t>(count));
    for (std::size_t i = 0; i < count; ++i) put_packet(w, snapshot.packets[i]);
    put_packet(w, snapshot.inventory_packet);
    w.f32(snapshot.position.x);
    w.f32(snapshot.position.y);
    w.f32(snapshot.position.z);
    w.f32(snapshot.heading);
    w.f32(snapshot.velocity.x);
    w.f32(snapshot.velocity.y);
    w.f32(snapshot.velocity.z);
    w.le(static_cast<std::uint8_t>(snapshot.moving ? 1 : 0));
    w.le(snapshot.target);
    w.le(static_cast<std::uint8_t>(snapshot.truncated ? 1 : 0));
    // Version 2: authoritative local vitals/status and extended target list.
    w.le(static_cast<std::uint8_t>(snapshot.has_vitals ? 1 : 0));
    if (snapshot.has_vitals) {
        w.le(snapshot.stand_state);
        w.le(snapshot.owner_status);
        const std::uint8_t flags = static_cast<std::uint8_t>(
            (snapshot.dead ? 1u : 0u) | (snapshot.auto_attack ? 2u : 0u) | (snapshot.auto_fire ? 4u : 0u) |
            (snapshot.running ? 8u : 0u));
        w.le(flags);
        w.le(snapshot.hp);
        w.le(snapshot.max_hp);
        w.le(snapshot.mana);
        w.le(snapshot.max_mana);
        w.le(snapshot.endurance);
        w.le(snapshot.max_endurance);
        w.le(snapshot.position_sequence);
        w.le(snapshot.position_entries);
    }
    w.le(static_cast<std::uint8_t>(snapshot.has_xtargets ? 1 : 0));
    if (snapshot.has_xtargets) {
        w.le(static_cast<std::uint8_t>(snapshot.xtarget_auto_haters ? 1 : 0));
        w.le(snapshot.xtarget_zone);
        const std::size_t xt_count = std::min<std::size_t>(snapshot.xtarget_slots.size(), kMaxXTargetSlots);
        w.le(static_cast<std::uint32_t>(xt_count));
        for (std::size_t i = 0; i < xt_count; ++i) {
            const auto& slot = snapshot.xtarget_slots[i];
            w.le(slot.type);
            w.le(slot.status);
            w.le(slot.spawn_id);
            put_string(w, slot.name);
        }
    }
    return w.take();
}

std::optional<DriveSnapshot> decode_drive_snapshot(std::span<const std::byte> bytes) {
    try {
        ByteReader r(bytes);
        if (r.le<std::uint32_t>() != kMagic) return std::nullopt;
        const auto version = r.le<std::uint16_t>();
        if (version != 1 && version != kVersion) return std::nullopt;
        DriveSnapshot snapshot;
        snapshot.character = read_string(r);
        const auto count = r.le<std::uint32_t>();
        if (count > kMaxPackets) return std::nullopt;
        snapshot.packets.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) snapshot.packets.push_back(read_packet(r));
        snapshot.inventory_packet = read_packet(r);
        snapshot.position.x = r.f32();
        snapshot.position.y = r.f32();
        snapshot.position.z = r.f32();
        snapshot.heading = r.f32();
        snapshot.velocity.x = r.f32();
        snapshot.velocity.y = r.f32();
        snapshot.velocity.z = r.f32();
        snapshot.moving = r.le<std::uint8_t>() != 0;
        snapshot.target = r.le<std::uint32_t>();
        snapshot.truncated = r.le<std::uint8_t>() != 0;
        if (version >= 2) {
            snapshot.has_vitals = r.le<std::uint8_t>() != 0;
            if (snapshot.has_vitals) {
                snapshot.stand_state = r.le<std::uint8_t>();
                snapshot.owner_status = r.le<std::int8_t>();
                const std::uint8_t flags = r.le<std::uint8_t>();
                snapshot.dead = (flags & 1u) != 0;
                snapshot.auto_attack = (flags & 2u) != 0;
                snapshot.auto_fire = (flags & 4u) != 0;
                snapshot.running = (flags & 8u) != 0;
                snapshot.hp = r.le<std::int64_t>();
                snapshot.max_hp = r.le<std::int64_t>();
                snapshot.mana = r.le<std::uint32_t>();
                snapshot.max_mana = r.le<std::uint32_t>();
                snapshot.endurance = r.le<std::uint32_t>();
                snapshot.max_endurance = r.le<std::uint32_t>();
                snapshot.position_sequence = r.le<std::uint16_t>();
                snapshot.position_entries = r.le<std::uint16_t>();
            }
            snapshot.has_xtargets = r.le<std::uint8_t>() != 0;
            if (snapshot.has_xtargets) {
                snapshot.xtarget_auto_haters = r.le<std::uint8_t>() != 0;
                snapshot.xtarget_zone = r.le<std::uint32_t>();
                const auto xt_count = r.le<std::uint32_t>();
                if (xt_count > kMaxXTargetSlots) return std::nullopt;
                snapshot.xtarget_slots.reserve(xt_count);
                for (std::uint32_t i = 0; i < xt_count; ++i) {
                    DriveXTargetSlot slot;
                    slot.type = r.le<std::uint32_t>();
                    slot.status = r.le<std::uint32_t>();
                    slot.spawn_id = r.le<std::uint32_t>();
                    slot.name = read_string(r);
                    snapshot.xtarget_slots.push_back(std::move(slot));
                }
            }
        }
        if (!r.empty()) return std::nullopt;
        return snapshot;
    } catch (...) {
        return std::nullopt;
    }
}

bool drive_forward_allowed(const AppPacket& packet, bool world) {
    const Op op = from_wire(packet.opcode);
    if (op == Op::Unknown) return true; // server extension or custom opcode: safe to forward in control mode
    if (world) {
        switch (op) {
        case Op::SendLoginInfo:
        case Op::EnterWorld:
        case Op::PostEnterWorld:
        case Op::WorldClientReady:
        case Op::WorldLogout:
            return false;
        default:
            return true;
        }
    }
    switch (op) {
    // Session / zone lifecycle: owned exclusively by the headless connection.
    case Op::ZoneEntry:
    case Op::ReqClientSpawn:
    case Op::ClientReady:
    case Op::ZoneChange:
    case Op::Camp:
    case Op::Logout:
    case Op::DeleteSpawn:
    // Position & movement history: owned by the headless's PositionTracker.
    case Op::ClientUpdate:
    case Op::FloatListThing:
    // Inventory & local combat/stance state: executed semantically via DriveAction so headless stays authoritative.
    case Op::MoveItem:
    case Op::MoveCoin:
    case Op::DeleteItem:
    case Op::AugmentItem:
    case Op::ItemVerifyRequest:
    case Op::TargetMouse:
    case Op::TargetCommand:
    case Op::AutoAttack:
    case Op::AutoAttack2:
    case Op::AutoFire:
    case Op::CastSpell:
    case Op::ChannelMessage:
    case Op::SpawnAppearance:
    case Op::LootRequest:
    case Op::LootItem:
    case Op::EndLootRequest:
    case Op::TradeRequest:
    case Op::TradeRequestAck:
    case Op::TradeBusy:
    case Op::TradeAcceptClick:
    case Op::CancelTrade:
    case Op::ShopRequest:
    case Op::ShopPlayerBuy:
    case Op::ShopPlayerSell:
    case Op::ShopEnd:
        return false;
    default:
        return true;
    }
}

} // namespace hailstone::net
