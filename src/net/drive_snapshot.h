#pragma once

// The drive relay's state transfer (docs/research/drive-headless.md, D1): the zone stream and the local state a
// fresh GameConnection needs to look like the character it is attached to. Encoded with core/byte_io.h, not a
// memory image; the reader bounds every section.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "net/packet.h"

namespace hailstone::net {

struct DriveSnapshot {
    std::string character;          // the name the own-spawn packets are matched against
    std::vector<AppPacket> packets; // zone stream, in arrival order
    AppPacket inventory_packet;     // OP_CharInventory with the possessions as they stand now
    glm::vec3 position{0.0f};
    float heading = 0.0f;
    glm::vec3 velocity{0.0f};
    bool moving = false;
    std::uint32_t target = 0;
    bool truncated = false;
};

// The journal's bound: a zone-in burst is a few MiB; a long session's journal can grow, and a truncated snapshot
// says so rather than pretending to be complete.
inline constexpr std::size_t kDriveSnapshotMaxBytes = std::size_t{64} << 20;

std::vector<std::byte> encode_drive_snapshot(const DriveSnapshot& snapshot);
std::optional<DriveSnapshot> decode_drive_snapshot(std::span<const std::byte> bytes);

} // namespace hailstone::net
