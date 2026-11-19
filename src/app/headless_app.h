#pragma once

// The headless host (docs/headless.md): login, world and zone through
// net::GameConnection with app::Automation (compiled plugins and MQ Lua
// scripts) and no window, Vulkan device, audio device or UI pack. It reuses
// the graphical client's connection and automation layers; only the frame's
// rendering half is missing. Zone geometry is loaded only as a collision
// mesh (floor, line of sight, zone lines), never as a scene.
//
// State: the loop, plugin/script automation, collision loading and the
// player's own movement are in — Navigator (app/navigator.h) steers the
// movement body (app/player_movement.h) per game step, with zone-line
// teleports, so /nav, /moveto and /stick walk. Not wired yet: the Ability
// Book accessors the graphical client takes from its UI host (AutomationEnv
// ability / use_ability / ability_bulk) and /keypress /face; those callbacks
// log once and report "unavailable", so an automation that needs them fails
// visibly instead of silently. Doors have no models headless (no
// WorldObjects), so a walk passes a closed door's doorway on the mesh and
// Navigator's door-in-the-way clicks do not trigger; click_door still goes
// out when Navigator asks for one directly.

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/login.h"

namespace hailstone::app {

struct HeadlessOptions {
    std::string game_dir; // --game-dir, else $EQ_GAME_DIR
    std::string data_dir; // --data-dir: the Hailstone folder to share (plugins, configs); empty: beside the binary
    std::string host;     // login server host[:port]
    std::uint16_t port = 5997;
    std::string user;
    std::string password; // wiped by the caller after run_headless takes it
    std::string character;
    std::string world;    // --world NAME|ID: the server to play (empty: the first on the list)
    bool list_servers = false; // --list-servers: print the login server list and exit
    // Command lines sent as the player's would be (through the hook bus):
    // --exec once after the first zone-in, --exec-zone at every zone-in.
    // Example: --exec "/mono load Sentinel" for the HSMono plugin.
    std::vector<std::string> exec;
    std::vector<std::string> exec_zone;
    int seconds = 0; // > 0: camp after this long in the first zone; 0: run until stopped
    bool load_collision = true; // --no-collision: no zone geometry at all
    std::string status_file;    // rewritten once a second while running; empty: none
};

// The arguments for one headless run; `error` empty means they parsed.
struct HeadlessArgs {
    HeadlessOptions options;
    std::string error;
};
HeadlessArgs parse_headless_args(const std::vector<std::string_view>& args);

// The command lines due at a zone-in: every --exec-zone line, plus the
// --exec lines only at the first one. Pure, so the schedule is testable
// without a server.
std::vector<std::string> headless_zone_commands(const HeadlessOptions& options, bool first_zone);

// Which server on the login list --world names: empty picks the first, an
// all-digits value is a server id, otherwise an exact name match (any case)
// and then a part of the name that only one server carries. Nullopt: no
// server matches, or the part is ambiguous. Pure, like the schedule above.
inline std::optional<std::uint32_t> headless_server_id(const std::vector<net::ServerInfo>& servers,
                                                       std::string_view want) {
    if (servers.empty()) return std::nullopt;
    if (want.empty()) return servers.front().id;
    auto fold = [](std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };
    if (std::string_view digits = want; !digits.empty() &&
        digits.find_first_not_of("0123456789") == std::string_view::npos) {
        std::uint32_t id = 0;
        for (char c : digits) id = id * 10u + static_cast<std::uint32_t>(c - '0');
        for (const auto& s : servers)
            if (s.id == id) return s.id;
        return std::nullopt;
    }
    const std::string needle = fold(want);
    for (const auto& s : servers)
        if (fold(s.name) == needle) return s.id;
    std::optional<std::uint32_t> found;
    for (const auto& s : servers)
        if (fold(s.name).find(needle) != std::string::npos) {
            if (found) return std::nullopt; // ambiguous
            found = s.id;
        }
    return found;
}

// Runs one character until it camps, the link dies, --seconds runs out or
// SIGINT/SIGTERM arrives. Returns a process exit code (0: camped cleanly).
int run_headless(HeadlessOptions options);

} // namespace hailstone::app
