#include "net/drive_snapshot.h"

#include "core/byte_io.h"
#include "net/spell_packets.h"
#include "net/zone_packets.h"

#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace hailstone::net {
namespace {

constexpr std::uint32_t kMagic = 0x31534448; // HDS1
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kMaxPackets = kDriveSnapshotMaxPackets;

// A zone journal is a history; a snapshot is a state. Zone and entity construction stay in full (they define the
// world). Updates that carry the current value of one thing collapse to their latest occurrence, keyed by the
// entity and slot they change. Pure log and visual packets are not state and go.
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
        return JournalPolicy::Drop;
    case Op::ClientUpdate:
    case Op::NPCMoveUpdate:
    case Op::HPUpdate:
    case Op::SpawnAppearance:
    case Op::Buff:
    case Op::Death:
    case Op::BuffCreate:
    case Op::TargetBuffs:
    case Op::PetBuffWindow:
    case Op::ManaChange:
    case Op::Stamina:
    case Op::TimeOfDay:
    case Op::Weather:
    case Op::ExpUpdate:
    case Op::LevelAppearance:
    case Op::PlayerProfile:
    case Op::CharInventory:
    case Op::XTargetResponse:
    case Op::TargetCommand:
        return JournalPolicy::Latest;
    default:
        return JournalPolicy::KeepAll;
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
    case Op::BuffCreate:
    case Op::TargetBuffs:
    case Op::PetBuffWindow:
        if (auto bi = decode_buff_icons(p)) return key(bi->entity_id, 0);
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
    for (std::size_t i = 0; i < packets.size(); ++i) {
        const Op op = from_wire(packets[i].opcode);
        if (journal_policy(op) != JournalPolicy::Latest) continue;
        if (auto key = journal_key(packets[i], op)) latest[*key] = i;
    }
    std::vector<AppPacket> kept;
    kept.reserve(latest.size() + 64);
    for (std::size_t i = 0; i < packets.size(); ++i) {
        const Op op = from_wire(packets[i].opcode);
        const JournalPolicy policy = journal_policy(op);
        if (policy == JournalPolicy::Drop) continue;
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
    return w.take();
}

std::optional<DriveSnapshot> decode_drive_snapshot(std::span<const std::byte> bytes) {
    try {
        ByteReader r(bytes);
        if (r.le<std::uint32_t>() != kMagic || r.le<std::uint16_t>() != kVersion) return std::nullopt;
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
        if (!r.empty()) return std::nullopt;
        return snapshot;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace hailstone::net
