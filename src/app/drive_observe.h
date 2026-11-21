#pragma once

// The headless side of the authenticated Crew pipe. It sends a zone snapshot,
// live packets and local state. Interactive endpoints can also queue typed
// inventory and dialog actions for the headless game thread.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include "net/drive_snapshot.h"
#include "net/packet.h"
#include "platform/local_pipe.h"

namespace hailstone::app {

enum class DriveObserveMessage : std::uint8_t {
    Snapshot = 1, // encode_drive_snapshot
    Packet = 2,   // opcode, data
    State = 3,    // position, heading, velocity, moving, target, inventory
    Bye = 4,
    Action = 5, // semantic GUI action, never a raw game packet
};

// The state a server packet does not echo: the headless's own position and target, and the possessions when they
// changed. The GUI applies it to its local GameConnection.
struct DriveLocalState {
    glm::vec3 position{0.0f};
    float heading = 0.0f;
    glm::vec3 velocity{0.0f};
    bool moving = false;
    std::uint32_t target = 0;
    std::optional<net::AppPacket> inventory_packet;
    std::optional<net::DriveUiState> ui;
};

std::vector<std::byte> encode_drive_packet(const net::AppPacket& packet);
std::optional<net::AppPacket> decode_drive_packet(std::span<const std::byte> body);
std::vector<std::byte> encode_drive_action(const net::DriveAction& action);
std::optional<net::DriveAction> decode_drive_action(std::span<const std::byte> body);
std::vector<std::byte> encode_drive_state(const DriveLocalState& state);
std::optional<DriveLocalState> decode_drive_state(std::span<const std::byte> body);

// One accepted viewer at a time. publish_* may be called from the game thread; the server thread does the pipe.
class DriveObserveServer {
public:
    explicit DriveObserveServer(platform::LocalPipeSession session, bool interactive = false);
    ~DriveObserveServer();
    DriveObserveServer(const DriveObserveServer&) = delete;
    DriveObserveServer& operator=(const DriveObserveServer&) = delete;

    const platform::LocalPipeSession& session() const;
    // True once a viewer has connected and its one snapshot has not been supplied yet. The game thread calls this
    // and set_snapshot() once it is in a zone.
    bool needs_snapshot() const;
    void set_snapshot(net::DriveSnapshot snapshot);
    void publish_zone_packet(const net::AppPacket& packet);
    void publish_state(const DriveLocalState& state);
    std::vector<net::DriveAction> take_actions();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hailstone::app
