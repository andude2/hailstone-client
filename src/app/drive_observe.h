#pragma once

// The headless side of the authenticated Crew pipe. It sends a zone snapshot,
// live packets and local state. Interactive endpoints queue typed inventory and
// dialog actions; full-control endpoints additionally accept semantic gameplay
// actions, human movement steering, and filtered server-authoritative requests
// while the headless remains the single live GameConnection and keeps plugins
// running continuously.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "net/drive_snapshot.h"
#include "net/packet.h"
#include "platform/local_pipe.h"
#include "ui/remote_imgui.h"

namespace hailstone::app {

enum class DriveObserveMessage : std::uint8_t {
    Snapshot = 1,      // encode_drive_snapshot (single message <= 1 MiB)
    Packet = 2,        // opcode, data
    State = 3,         // position, heading, velocity, moving, target, inventory, optional ui & control status
    Bye = 4,
    Action = 5,        // semantic GUI action
    SnapshotChunk = 6, // {u32 total, u32 offset, bytes} for snapshots > 1 MiB or zone-change re-snapshots
    Move = 7,          // GUI -> headless DriveMove (human movement steering)
    ForwardZone = 8,   // GUI -> headless filtered zone AppPacket {u8 reliable, u16 opcode, u32 size, bytes}
    ForwardWorld = 9,  // GUI -> headless filtered world AppPacket {u8 reliable, u16 opcode, u32 size, bytes}
    Chat = 10,         // headless -> GUI ChatEvent {u8 kind, u32 rgb, u32 color, peer, text}
    ImGuiInput = 11,   // GUI -> headless RemoteImGuiInput
    ImGuiFrame = 12,   // headless -> GUI RemoteImGuiFrame (plugin ImGui draw lists)
    ImGuiTexture = 13, // headless -> GUI RemoteImGuiTexture (font atlas create/update/destroy)
    CrewCommand = 14,  // headless -> GUI "/crew <args>" invoked by a plugin on headless
};

struct DriveForwardedPacket {
    net::AppPacket packet;
    bool reliable = true;
    bool world = false;
};

struct DriveChatLine {
    std::uint8_t kind = 0;
    std::uint32_t rgb = 0;
    std::uint32_t color = 0;
    std::string peer;
    std::string text;
};

// The state a server packet does not echo: the headless's own position, target,
// status, extended targets, and possessions when they changed. The GUI applies
// it to its local mirror GameConnection.
struct DriveLocalState {
    glm::vec3 position{0.0f};
    float heading = 0.0f;
    glm::vec3 velocity{0.0f};
    bool moving = false;
    std::uint32_t target = 0;
    std::optional<net::AppPacket> inventory_packet;
    std::optional<net::DriveUiState> ui;
    // Control-mode status tail (optional; omitted by plain observe/interact callers).
    bool has_status = false;
    std::uint8_t stand_state = 100;
    std::int8_t owner_status = 0;
    bool dead = false;
    bool auto_attack = false;
    bool auto_fire = false;
    bool running = true;
    std::int64_t hp = 0, max_hp = 0;
    std::uint32_t mana = 0, max_mana = 0;
    std::uint32_t endurance = 0, max_endurance = 0;
    bool has_xtargets = false;
    bool xtarget_auto_haters = false;
    std::uint32_t xtarget_zone = 0;
    std::vector<net::DriveXTargetSlot> xtarget_slots;
};

std::vector<std::byte> encode_drive_packet(const net::AppPacket& packet);
std::optional<net::AppPacket> decode_drive_packet(std::span<const std::byte> body);
std::vector<std::byte> encode_drive_action(const net::DriveAction& action);
std::optional<net::DriveAction> decode_drive_action(std::span<const std::byte> body);
std::vector<std::byte> encode_drive_move(const net::DriveMove& move);
std::optional<net::DriveMove> decode_drive_move(std::span<const std::byte> body);
std::vector<std::byte> encode_drive_forward(const DriveForwardedPacket& fwd);
std::optional<DriveForwardedPacket> decode_drive_forward(std::span<const std::byte> body);
std::vector<std::byte> encode_drive_chat(const DriveChatLine& line);
std::optional<DriveChatLine> decode_drive_chat(std::span<const std::byte> body);
std::vector<std::byte> encode_drive_state(const DriveLocalState& state);
std::optional<DriveLocalState> decode_drive_state(std::span<const std::byte> body);
std::vector<std::byte> encode_drive_imgui_input(const ui::RemoteImGuiInput& input);
std::optional<ui::RemoteImGuiInput> decode_drive_imgui_input(std::span<const std::byte> body);
std::vector<std::byte> encode_drive_imgui_frame(const ui::RemoteImGuiFrame& frame);
std::optional<ui::RemoteImGuiFrame> decode_drive_imgui_frame(std::span<const std::byte> body);
std::vector<std::byte> encode_drive_imgui_texture(const ui::RemoteImGuiTexture& tex);
std::optional<ui::RemoteImGuiTexture> decode_drive_imgui_texture(std::span<const std::byte> body);
std::vector<std::byte> encode_drive_crew_command(std::string_view args);
std::optional<std::string> decode_drive_crew_command(std::span<const std::byte> body);

// One accepted viewer at a time. publish_* may be called from the game thread; the server thread does the pipe.
class DriveObserveServer {
public:
    explicit DriveObserveServer(platform::LocalPipeSession session, bool interactive = false, bool control = false);
    ~DriveObserveServer();
    DriveObserveServer(const DriveObserveServer&) = delete;
    DriveObserveServer& operator=(const DriveObserveServer&) = delete;

    const platform::LocalPipeSession& session() const;
    // True once a viewer has connected and its initial snapshot has not been supplied yet.
    bool needs_snapshot() const;
    bool viewer_connected() const;
    void set_snapshot(net::DriveSnapshot snapshot);
    // Push a fresh snapshot to an already-connected viewer (e.g. after the headless character zones).
    void publish_snapshot(net::DriveSnapshot snapshot);
    void publish_zone_packet(const net::AppPacket& packet);
    void publish_state(const DriveLocalState& state);
    void publish_chat(const DriveChatLine& line);
    void publish_imgui_frame(const ui::RemoteImGuiFrame& frame);
    void publish_imgui_texture(const ui::RemoteImGuiTexture& tex);
    void publish_crew_command(std::string_view args);
    std::vector<net::DriveAction> take_actions();
    std::optional<net::DriveMove> take_move();
    std::vector<DriveForwardedPacket> take_forwarded();
    std::optional<ui::RemoteImGuiInput> take_imgui_input();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hailstone::app
