#pragma once

// The drive relay's state transfer (docs/research/drive-headless.md, D1): the zone stream and the local state a
// fresh GameConnection needs to look like the character it is attached to. Encoded with core/byte_io.h, not a
// memory image; the reader bounds every section.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "net/packet.h"
#include "net/commerce.h"

namespace hailstone::net {

enum class DriveActionKind : std::uint8_t { InventoryClick, AugmentClick, AugmentMenu, Answer, Quantity };

struct DriveAction {
    DriveActionKind kind{};
    ItemSlot slot;
    std::uint32_t argument = 0;
    std::int32_t socket = 0;
    std::uint32_t zone_generation = 0;
    std::uint64_t inventory_revision = 0;
    std::uint64_t dialog = 0;
    std::string item_serial;
};

struct DriveQuestion {
    std::uint64_t id = 0;
    std::uint32_t kind = 0;
    std::string text;
    bool plain_text = false;
    bool ok_only = false;
};

struct DriveUiState {
    std::uint32_t zone_generation = 0;
    std::uint64_t inventory_revision = 0;
    std::optional<DriveQuestion> question;
    std::optional<QuantityRequest> quantity;
};

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
    std::optional<DriveUiState> ui;
};

// The journal's bound: a zone-in burst is a few MiB; a long session's journal can grow, and a truncated snapshot
// says so rather than pretending to be complete.
inline constexpr std::size_t kDriveSnapshotMaxBytes = std::size_t{64} << 20;

std::vector<std::byte> encode_drive_snapshot(const DriveSnapshot& snapshot);
std::optional<DriveSnapshot> decode_drive_snapshot(std::span<const std::byte> bytes);

} // namespace hailstone::net
