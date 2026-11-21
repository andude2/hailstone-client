#pragma once

// Connect the GUI mirror to the headless snapshot and live feed. The mirror
// never forwards game packets. Crew interaction uses typed actions on the pipe.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

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
};

std::optional<DriveObserveFile> read_drive_observe_file(const std::filesystem::path& path, std::string& error);

class DriveClient {
public:
    struct Impl; // the shared pipe state, defined in drive_client.cpp
    ~DriveClient();
    DriveClient(const DriveClient&) = delete;
    DriveClient& operator=(const DriveClient&) = delete;

    static std::unique_ptr<DriveClient> connect(const DriveObserveFile& file, std::string& error);

    // One Session for the GameConnection; a zone change asks for another, and both read the same live queue.
    std::unique_ptr<net::Session> make_session();
    net::DriveSnapshot take_snapshot();
    void send_action(const net::DriveAction& action);
    // The latest local state, if a State message arrived since the last call.
    std::optional<net::DriveSnapshot> take_local_state();
    void pump();
    const DriveObserveFile& info() const;

private:
    explicit DriveClient(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

} // namespace hailstone::app
