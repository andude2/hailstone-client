#pragma once

// Connect the GUI mirror to the headless snapshot and live feed.
// Observe mode is read-only; Interact mode queues inventory/dialog actions;
// Control mode adds full semantic gameplay actions, human movement steering,
// and filtered forwarding of server-authoritative request packets while the
// headless process remains the sole live GameConnection.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/drive_observe.h"
#include "net/drive_snapshot.h"
#include "net/session.h"

namespace hailstone::app {

struct DriveObserveFile {
    std::string endpoint;
    std::string token;
    std::string character;
    std::string profile;
    std::string game_dir;
    bool interactive = false;
    bool control = false; // control=1: full interactive control over the headless's live GameConnection
};

std::optional<DriveObserveFile> read_drive_observe_file(const std::filesystem::path& path, std::string& error);

class DriveClient {
public:
    struct Impl; // the shared pipe state, defined in drive_client.cpp
    ~DriveClient();
    DriveClient(const DriveClient&) = delete;
    DriveClient& operator=(const DriveClient&) = delete;

    static std::unique_ptr<DriveClient> connect(const DriveObserveFile& file, std::string& error);

    // Mirror session for the GUI's GameConnection. In Control mode, outgoing server-authoritative packets permitted
    // by drive_forward_allowed() cross to the headless's live session; Observe/Interact drop all outgoing packets.
    std::unique_ptr<net::Session> make_session(bool world = false);
    net::DriveSnapshot take_snapshot();
    // Non-empty when the headless zoned while this viewer was attached and pushed a new zone snapshot.
    std::optional<net::DriveSnapshot> take_resnapshot();
    void send_action(const net::DriveAction& action);
    void send_move(const net::DriveMove& move);
    // The latest local state, if a State message arrived since the last call.
    std::optional<net::DriveSnapshot> take_local_state();
    // Chat lines forwarded from the headless (zone chat, UCS tells/channels, plugin echoes).
    std::vector<DriveChatLine> take_chat();
    void send_imgui_input(const ui::RemoteImGuiInput& input);
    std::optional<ui::RemoteImGuiFrame> take_imgui_frame();
    std::vector<ui::RemoteImGuiTexture> take_imgui_textures();
    std::vector<std::string> take_crew_commands();
    void pump();
    bool closed() const;
    bool is_control() const;
    const DriveObserveFile& info() const;

private:
    explicit DriveClient(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

} // namespace hailstone::app
