#pragma once

// OP_EvolveItem: RoF2's experience-transfer requests and server-owned result previews. Action 0 (progress) and the
// Final Result request (action 4) are net/item_packets.h's EvolvingItemUpdate and EvolvingFinalResult.
// Layout reference: EQEmu common/patches/rof2_structs.h and ENCODE(OP_EvolveItem),
// consulted as a protocol specification, with no implementation copied.

#include "net/item_packets.h"

namespace hailstone::net {

enum class EvolvingAction : std::uint32_t { Update = 0, Open = 1, Details = 2, Transfer = 3, FinalResult = 4 };

// RoF2 puts an evolving item's persistent identifier in these two header words.
inline std::uint64_t evolving_item_id(const Item& item) {
    return item.is_evolving ? (static_cast<std::uint64_t>(item.parcel_item_id) << 32) | item.instance_id : 0;
}

struct EvolvingRequest {
    EvolvingAction action = EvolvingAction::Details;
    std::uint64_t source_id = 0, destination_id = 0;
};
// Action 1 (the server opens the transfer window) in action 0's 24-byte envelope (EQEmu's EvolveItemToggle).
struct EvolvingUpdate {
    EvolvingAction action = EvolvingAction::Open;
    std::uint64_t id = 0;
    std::uint32_t percentage = 0, activated = 0;
};
struct EvolvingTransferDetails {
    std::uint64_t source_id = 0, destination_id = 0;
    std::uint32_t compatibility = 0, max_transfer_level = 0;
    std::optional<Item> source, destination;
};

inline constexpr std::size_t kEvolvingRequestSize = 24, kEvolvingDetailsHeaderSize = 30;
AppPacket encode(const EvolvingRequest& value);
AppPacket encode(const EvolvingUpdate& value); // server direction, for tests
AppPacket encode(const EvolvingTransferDetails& value); // server direction, for tests
std::optional<EvolvingRequest> decode_evolving_request(const AppPacket& p);
std::optional<EvolvingUpdate> decode_evolving_open(const AppPacket& p); // action 1 only
std::optional<EvolvingTransferDetails> decode_evolving_details(const AppPacket& p);

} // namespace hailstone::net
