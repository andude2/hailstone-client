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
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/server_profile.h"
#include "net/login.h"

namespace hailstone::app {

struct HeadlessOptions {
    std::string game_dir; // --game-dir, else $EQ_GAME_DIR
    bool game_dir_given = false; // --game-dir was named (a saved server's own game_files only fills it in when not)
    std::string data_dir; // --data-dir: the Hailstone folder to share (plugins, configs); empty: beside the binary
    std::string host;     // login server host[:port]
    std::uint16_t port = 5997;
    std::string user;
    std::string password; // wiped by the caller after run_headless takes it
    std::string character;
    std::string world;    // --world NAME|ID: the server to play (empty: the first on the list)
    std::string server;   // --server NAME|ID: a saved server (servers/<id>), by id or name
    std::string account;  // --account USER: a saved account of --server, its password from the system keyring
    std::string crew_file; // --crew FILE: supervise one child per crew member (below) instead of one character
    std::string plugin_autoload = "*"; // --autoload LIST: the plugins to load ("*" all, else names, space/comma separated)
    bool chat_log = true; // --no-chat-log: drain chat without printing it (the hooks still see every line)
    bool quiet = false;   // --quiet: the log goes to its file only, nothing to this console (a crew child)
    bool list_servers = false; // --list-servers: print the login server list and exit
    // Command lines sent as the player's would be (through the hook bus):
    // --exec once after the first zone-in, --exec-zone at every zone-in.
    // Example: --exec "/mono load Sentinel" for the HSMono plugin.
    std::vector<std::string> exec;
    std::vector<std::string> exec_zone;
    int seconds = 0; // > 0: camp after this long in the first zone; 0: run until stopped
    bool load_collision = true; // --no-collision: no zone geometry at all
    std::string status_file;    // rewritten once a second while running; empty: none
    // --drive-observe: serve the character to an observe viewer over the local token pipe; --drive-file: write
    // its endpoint and token to a new private file for the viewer to read.
    bool drive_observe = false;
    bool drive_interact = false;
    // --drive-control: freeze this character and lend its live sessions to a control viewer (app::DriveRelay).
    bool drive_control = false;
    std::string drive_file;
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

// One member of a crew file (--crew): a saved account on a saved server,
// entered as its character, with its own command lines. Plain sections of
// an ini file (docs/headless.md "Crew mode"): the section name is the
// member's label, [crew] holds the shared keys.
struct CrewMember {
    std::string label;
    std::string server;    // a saved server, by id or name (required)
    std::string account;   // a saved account of it (required)
    std::string character; // empty: the account's saved character
    std::string world;     // empty: the account's saved world, else the first
    std::string exec;      // one command line after the first zone-in
    std::string exec_zone; // one command line at every zone-in
    std::string game_dir;  // empty: the shared one
    std::string autoload;  // plugins to load (empty: the shared one)
    int chat_log = -1;     // print chat lines (< 0: the shared one)
    int seconds = 0;       // camp after this long (0: until stopped)
    int stagger = -1;      // seconds to wait before launching this member (< 0: the shared one)
    int interact = -1;     // permit inventory and confirmation input from /crew
    int observe = -1;      // serve this member to /crew (< 0: the shared one)
    int control = -1;      // lend this member's sessions to /crew (< 0: the shared one)
};
struct CrewFile {
    std::string data_dir;  // [crew] data_dir (the shared folder)
    std::string game_dir;  // [crew] game_dir
    std::string autoload = "*"; // [crew] autoload (the plugins every member loads)
    bool chat_log = false; // [crew] chat_log: a crew's children do not print chat unless asked
    int stagger = 10;      // seconds between launches, [crew] stagger
    bool interact = false;
    bool observe = false;  // [crew] observe: every member gets --drive-observe and a file in drive_dir
    bool control = false;  // [crew] control: every member gets --drive-control and a file in drive_dir
    std::string drive_dir; // [crew] drive_dir: where those files go (empty: <data dir>/run)
    std::vector<CrewMember> members;
    std::string error; // empty: parsed
};
CrewFile parse_crew_file(std::string_view text);
std::string format_crew_file(const CrewFile& crew);
std::filesystem::path crew_drive_dir(const CrewFile& crew, const std::filesystem::path& data_dir);
void clear_stale_drive_files(const std::filesystem::path& drive_dir);
// Prepares a character's headless ImGui layout file (`ini`): creates its parent folder, migrates any legacy root
// `legacy_ini` if `ini` does not exist yet, or otherwise seeds `ini` once from the GUI's `gui_ini`.
void seed_headless_imgui_ini(const std::filesystem::path& ini, const std::filesystem::path& gui_ini,
                             const std::filesystem::path& legacy_ini = {});

// Which saved server --server names: an exact id or name (any case), then
// a part of the name or id only one profile carries. Nullopt: none, or
// ambiguous. Pure, like headless_server_id.
inline const ServerProfile* pick_headless_profile(const std::vector<ServerProfile>& profiles,
                                                  std::string_view want) {
    auto fold = [](std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };
    const std::string needle = fold(want);
    for (const auto& p : profiles)
        if (fold(p.id) == needle || fold(p.name) == needle) return &p;
    const ServerProfile* found = nullptr;
    for (const auto& p : profiles)
        if (fold(p.name).find(needle) != std::string::npos || fold(p.id).find(needle) != std::string::npos) {
            if (found) return nullptr;
            found = &p;
        }
    return found;
}

// Runs one character until it camps, the link dies, --seconds runs out or
// SIGINT/SIGTERM arrives. Returns a process exit code: 0 camped or
// stopped, 3 the link died (a crew supervisor relaunches only those), 1
// anything else. With --crew, supervises the crew instead and returns 0
// when every member is done or it is stopped.
int run_headless(HeadlessOptions options);

} // namespace hailstone::app
