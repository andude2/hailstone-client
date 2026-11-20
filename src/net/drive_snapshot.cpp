#include "net/drive_snapshot.h"

#include "core/byte_io.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace hailstone::net {
namespace {

constexpr std::uint32_t kMagic = 0x31534448; // HDS1
constexpr std::uint16_t kVersion = 1;
constexpr std::uint32_t kMaxPackets = 100000;

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

std::vector<std::byte> encode_drive_snapshot(const DriveSnapshot& snapshot) {
    if (snapshot.packets.size() > kMaxPackets) throw std::invalid_argument("drive snapshot has too many packets");
    ByteWriter w;
    w.le(kMagic);
    w.le(kVersion);
    put_string(w, snapshot.character);
    w.le(static_cast<std::uint32_t>(snapshot.packets.size()));
    for (const auto& packet : snapshot.packets) put_packet(w, packet);
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
