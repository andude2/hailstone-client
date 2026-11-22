#include "app/headless_app.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#include "app/automation.h"
#include "app/actions_ui.h"
#include "app/ability_bulk.h"
#include "app/drive_observe.h"
#include "app/lua_ui.h"
#include "core/config.h"
#include "app/navigator.h"
#include "app/player_movement.h"
#include "app/zone_move_world.h"
#include "assets/zone.h"
#include "core/character_files.h"
#include "core/game_dir.h"
#include "core/ini_file.h"
#include "core/server_profile.h"
#include "core/log.h"
#include "core/step_clock.h"
#include "core/wipe.h"
#include "game/collision.h"
#include "game/coords.h"
#include "game/mechanics/character.h"
#include "game/mechanics/collision_movement.h"
#include "game/mechanics/sheet_builder.h"
#include "game/mechanics/tables.h"
#include "game/mounts.h"
#include "game/position_tracker.h"
#include "game/zone_lines.h"
#include "game/zoning.h"
#include "nav/zone_nav.h"
#include "net/game_connection.h"
#include "net/login.h"
#include "platform/child_processes.h"
#include "platform/credentials.h"
#include "platform/paths.h"
#include "platform/private_file.h"

namespace hailstone::app {
namespace {

volatile std::sig_atomic_t stop_requested = 0;
void on_stop_signal(int) { stop_requested = 1; }
using Clock = std::chrono::steady_clock;

void stub_once(const char* what) {
    static bool face_logged = false, key_logged = false, window_logged = false;
    bool* flag = std::strcmp(what, "face") == 0 ? &face_logged : std::strcmp(what, "keypress") == 0 ? &key_logged : &window_logged;
    if (*flag) return;
    *flag = true;
    log::warn("headless: {} is not wired yet (docs/headless.md); automation that needs it will see it unavailable", what);
}

} // namespace

HeadlessArgs parse_headless_args(const std::vector<std::string_view>& args) {
    HeadlessArgs out;
    if (const char* gd = std::getenv("EQ_GAME_DIR")) out.options.game_dir = gd;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto need = [&](std::string_view name) -> std::string_view {
            if (i + 1 >= args.size()) {
                out.error = std::string(name) + " needs a value";
                return {};
            }
            return args[++i];
        };
        const std::string_view a = args[i];
        if (a == "--game-dir") {
            out.options.game_dir = need(a);
            out.options.game_dir_given = true;
        }
        else if (a == "--data-dir") out.options.data_dir = need(a);
        else if (a == "--host") {
            std::string_view h = need(a);
            if (auto c = h.rfind(':'); c != std::string_view::npos) {
                out.options.port = static_cast<std::uint16_t>(std::atoi(std::string(h.substr(c + 1)).c_str()));
                h = h.substr(0, c);
            }
            out.options.host = h;
        } else if (a == "--port") out.options.port = static_cast<std::uint16_t>(std::atoi(std::string(need(a)).c_str()));
        else if (a == "--user") out.options.user = need(a);
        else if (a == "--password") out.options.password = need(a);
        else if (a == "--character") out.options.character = need(a);
        else if (a == "--world") out.options.world = need(a);
        else if (a == "--server") out.options.server = need(a);
        else if (a == "--account") out.options.account = need(a);
        else if (a == "--crew") out.options.crew_file = need(a);
        else if (a == "--autoload") out.options.plugin_autoload = need(a);
        else if (a == "--no-chat-log") out.options.chat_log = false;
        else if (a == "--quiet") out.options.quiet = true;
        else if (a == "--list-servers") out.options.list_servers = true;
        else if (a == "--exec") out.options.exec.emplace_back(need(a));
        else if (a == "--exec-zone") out.options.exec_zone.emplace_back(need(a));
        else if (a == "--seconds") out.options.seconds = std::atoi(std::string(need(a)).c_str());
        else if (a == "--status-file") out.options.status_file = need(a);
        else if (a == "--no-collision") out.options.load_collision = false;
        else if (a == "--drive-observe") out.options.drive_observe = true;
        else if (a == "--drive-interact") out.options.drive_interact = true;
        else if (a == "--drive-file") out.options.drive_file = need(a);
        else if (!out.error.empty()) break;
        else out.error = "unknown argument: " + std::string(a);
        if (!out.error.empty()) break;
    }
    if (out.error.empty() && out.options.drive_observe && !out.options.crew_file.empty())
        out.error = "--drive-observe is for a single character; a crew sets observe=1 in [crew]";
    if (out.error.empty() && out.options.drive_observe && out.options.drive_file.empty())
        out.error = "--drive-observe needs --drive-file (the viewer reads its endpoint and token there)";
    if (out.error.empty() && out.options.drive_interact && !out.options.drive_observe)
        out.error = "--drive-interact needs --drive-observe";
    if (out.error.empty() && !out.options.drive_file.empty() && !out.options.drive_observe)
        out.error = "--drive-file needs --drive-observe";
    if (out.error.empty() && out.options.crew_file.empty() && out.options.account.empty()) {
        // A saved-account login (--account) resolves its host, user, password
        // and character from the profile at run time; a crew run likewise.
        if (out.options.host.empty()) out.error = "--host is required (or --account, or --crew)";
        else if (out.options.user.empty()) out.error = "--user is required";
        else if (out.options.password.empty()) out.error = "--password is required";
        else if (out.options.character.empty() && !out.options.list_servers) out.error = "--character is required";
        else if (out.options.game_dir.empty()) out.error = "--game-dir or EQ_GAME_DIR is required";
    }
    return out;
}

CrewFile parse_crew_file(std::string_view text) {
    CrewFile out;
    IniFile ini;
    ini.parse(text);
    out.data_dir = ini.get_string("crew", "data_dir");
    out.game_dir = ini.get_string("crew", "game_dir");
    if (auto a = ini.get("crew", "autoload")) out.autoload = *a;
    out.chat_log = ini.get_int("crew", "chat_log", out.chat_log ? 1 : 0) != 0;
    out.stagger = ini.get_int("crew", "stagger", out.stagger);
    out.interact = ini.get_int("crew", "interact", 0) != 0;
    out.observe = ini.get_int("crew", "observe", out.observe ? 1 : 0) != 0;
    out.drive_dir = ini.get_string("crew", "drive_dir");
    for (const auto& section : ini.sections()) {
        if (section == "crew") continue;
        CrewMember m;
        m.label = section;
        m.server = ini.get_string(section, "server");
        m.account = ini.get_string(section, "account");
        m.character = ini.get_string(section, "character");
        m.world = ini.get_string(section, "world");
        m.exec = ini.get_string(section, "exec");
        m.exec_zone = ini.get_string(section, "exec_zone");
        m.game_dir = ini.get_string(section, "game_dir");
        m.autoload = ini.get_string(section, "autoload");
        m.chat_log = ini.get_int(section, "chat_log", -1);
        m.seconds = ini.get_int(section, "seconds", 0);
        m.stagger = ini.get_int(section, "stagger", -1);
        m.interact = ini.get_int(section, "interact", -1);
        m.observe = ini.get_int(section, "observe", -1);
        if (m.server.empty() || m.account.empty()) {
            out.error = "crew member [" + section + "] needs server= and account=";
            return out;
        }
        out.members.push_back(std::move(m));
    }
    if (out.error.empty() && out.members.empty()) out.error = "no crew members (a section per character)";
    return out;
}

std::vector<std::string> headless_zone_commands(const HeadlessOptions& options, bool first_zone) {
    std::vector<std::string> out = options.exec_zone;
    if (first_zone) out.insert(out.end(), options.exec.begin(), options.exec.end());
    // An --exec line names a command, not something to say: a shell that ate
    // its leading '/' must not turn it into a say in the zone.
    for (auto& line : out)
        if (!line.empty() && line.front() != '/') line.insert(line.begin(), '/');
    return out;
}

namespace {
// One saved login, resolved from the profiles under the data folder: the
// profile, the account in it, and whether its keyring password is there.
struct SavedLogin {
    const ServerProfile* profile = nullptr;
    const SavedAccount* account = nullptr;
    std::string problem; // empty: resolved (account may still be null without --account)
};
SavedLogin resolve_saved(const std::vector<ServerProfile>& profiles, std::string_view server,
                         std::string_view account) {
    SavedLogin out;
    if (!server.empty()) {
        out.profile = pick_headless_profile(profiles, server);
        if (!out.profile) {
            out.problem = "no saved server matches \"" + std::string(server) + "\" (the saved servers are:";
            for (const auto& p : profiles) out.problem += " " + p.id;
            out.problem += ")";
            return out;
        }
    } else if (!account.empty()) {
        for (const auto& p : profiles)
            if (find_account(p, account)) {
                if (out.profile) {
                    out.problem = "account \"" + std::string(account) + "\" is saved on more than one server; name --server";
                    return out;
                }
                out.profile = &p;
            }
        if (!out.profile) {
            out.problem = "no saved server has account \"" + std::string(account) + "\"";
            return out;
        }
    }
    if (!account.empty() && out.profile) {
        out.account = find_account(*out.profile, account);
        if (!out.account) out.problem = "server " + out.profile->id + " has no saved account \"" + std::string(account) + "\"";
    }
    return out;
}

// The crew supervisor (--crew): one hailstone_headless child per member,
// staggered, each logging in by its saved account (the child reads its
// password from the keyring itself; a password never goes on a command
// line). Every member's login is proven before any child starts, as the
// box set's start proves its passwords. A child that exits 3 (its link
// died) is relaunched after 30 s; a clean camp (0) or a failure (1)
// stays down. Stopping the supervisor ends the children (gently, then
// forcibly after 5 s).
int run_crew(const HeadlessOptions& options) {
    std::ifstream in(options.crew_file, std::ios::binary);
    if (!in) {
        log::error("headless crew: cannot read {}", options.crew_file);
        return 2;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CrewFile crew = parse_crew_file(text);
    if (!crew.error.empty()) {
        log::error("headless crew: {}: {}", options.crew_file, crew.error);
        return 2;
    }
    const std::string data_dir = !crew.data_dir.empty() ? crew.data_dir : options.data_dir;
    if (!data_dir.empty()) platform::use_data_dir(data_dir, "--crew");
    if (std::string problem = platform::data_dir_problem(); !problem.empty()) {
        log::error("{}", problem);
        return 1;
    }
    const auto profiles = list_profiles();
    const bool observe_any = crew.observe ||
                             std::any_of(crew.members.begin(), crew.members.end(),
                                         [](const CrewMember& m) { return m.observe > 0; });
    std::filesystem::path drive_dir = crew.drive_dir.empty() ? platform::data_dir() / "run"
                                                             : std::filesystem::path(crew.drive_dir);
    if (drive_dir.is_relative()) drive_dir = platform::data_dir() / drive_dir;
    if (observe_any) {
        std::error_code ec;
        std::filesystem::create_directories(drive_dir, ec);
        if (ec) log::warn("headless crew: cannot make the drive folder {}: {}", drive_dir.string(), ec.message());
    }
    auto safe_file_name = [](std::string name) {
        for (char& c : name)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
        return name;
    };
    struct Member {
        CrewMember crew;
        std::string profile_id, character, world;
        std::string drive_file;
        bool observe = false;
        enum class State { Waiting, Running, Done } state = State::Waiting;
        Clock::time_point at{}; // launch or relaunch time, while Waiting
        int restarts = 0;
    };
    std::vector<Member> members;
    bool any_problem = false;
    for (const auto& m : crew.members) {
        auto saved = resolve_saved(profiles, m.server, m.account);
        if (!saved.problem.empty()) {
            log::error("headless crew: [{}] {}", m.label, saved.problem);
            any_problem = true;
            continue;
        }
        auto pw = platform::system_credentials().load(
            platform::credential_server(saved.profile->host, saved.profile->port), saved.account->username);
        if (!pw || pw->empty()) {
            log::error("headless crew: [{}] no saved password for {} on {}: log in once in the graphical "
                       "client with Save password ticked",
                       m.label, saved.account->username, saved.profile->id);
            any_problem = true;
            continue;
        }
        core::wipe(*pw);
        Member r;
        r.crew = m;
        r.profile_id = saved.profile->id;
        r.character = !m.character.empty() ? m.character : saved.account->character;
        r.world = !m.world.empty() ? m.world : saved.account->world;
        r.observe = m.observe >= 0 ? m.observe != 0 : crew.observe;
        if (r.observe)
            r.drive_file = (drive_dir / ("drive-" + safe_file_name(m.label) + ".txt")).string();
        members.push_back(std::move(r));
    }
    if (any_problem) {
        log::error("headless crew: nothing started ({} of {} members cannot log in)",
                   crew.members.size() - members.size(), crew.members.size());
        return 1;
    }
    std::filesystem::path exe = platform::install_dir() / "hailstone_headless";
#if defined(_WIN32)
    exe += ".exe";
#endif
    platform::ChildProcesses children;
    auto launch = [&](Member& r) {
        std::vector<std::string> args{exe.string(), "--server", r.profile_id, "--account", r.crew.account};
        const std::string game_dir = !r.crew.game_dir.empty() ? r.crew.game_dir : crew.game_dir;
        if (!data_dir.empty()) args.insert(args.end(), {"--data-dir", data_dir});
        if (!game_dir.empty()) args.insert(args.end(), {"--game-dir", game_dir});
        if (!options.game_dir.empty() && game_dir.empty()) args.insert(args.end(), {"--game-dir", options.game_dir});
        if (!r.character.empty()) args.insert(args.end(), {"--character", r.character});
        if (!r.world.empty()) args.insert(args.end(), {"--world", r.world});
        if (!r.crew.exec.empty()) args.insert(args.end(), {"--exec", r.crew.exec});
        if (!r.crew.exec_zone.empty()) args.insert(args.end(), {"--exec-zone", r.crew.exec_zone});
        args.insert(args.end(), {"--autoload", !r.crew.autoload.empty() ? r.crew.autoload : crew.autoload});
        if (!(r.crew.chat_log >= 0 ? r.crew.chat_log != 0 : crew.chat_log)) args.emplace_back("--no-chat-log");
        args.emplace_back("--quiet"); // its log is its file's; this console is the supervisor's
        if (r.crew.seconds > 0) args.insert(args.end(), {"--seconds", std::to_string(r.crew.seconds)});
        if (r.observe && !r.drive_file.empty()) {
            std::error_code ec;
            std::filesystem::remove(r.drive_file, ec); // a relaunch may find the old file
            args.emplace_back("--drive-observe");
            if (r.crew.interact >= 0 ? r.crew.interact != 0 : crew.interact)
                args.emplace_back("--drive-interact");
            args.insert(args.end(), {"--drive-file", r.drive_file});
        }
        std::string error;
        if (children.start_tracked(r.crew.label, args, error)) {
            r.state = Member::State::Running;
            log::info("headless crew: started [{}] as {} on {}", r.crew.label, r.character, r.profile_id);
        } else {
            r.state = Member::State::Done;
            if (!r.drive_file.empty()) {
                std::error_code ec;
                std::filesystem::remove(r.drive_file, ec);
            }
            log::error("headless crew: [{}] could not start: {}", r.crew.label, error);
        }
    };
    auto at = Clock::now();
    for (auto& r : members) {
        r.at = at;
        at += std::chrono::seconds(r.crew.stagger >= 0 ? r.crew.stagger : crew.stagger);
    }
    log::info("headless crew: {} members on {} saved server(s); stopping this process ends them all",
              members.size(), profiles.size());
    Clock::time_point last_status = Clock::now();
    while (!stop_requested) {
        const auto now = Clock::now();
        for (auto& r : members)
            if (r.state == Member::State::Waiting && now >= r.at) launch(r);
        children.poll();
        for (const auto& e : children.take_exited()) {
            for (auto& r : members)
                if (r.crew.label == e.name && r.state == Member::State::Running) {
                    if (!r.drive_file.empty()) {
                        std::error_code ec;
                        std::filesystem::remove(r.drive_file, ec); // the endpoint is gone with the child
                    }
                    if (e.exit_code == 3) {
                        r.state = Member::State::Waiting;
                        r.at = now + std::chrono::seconds(30);
                        ++r.restarts;
                        log::warn("headless crew: [{}] link died; relaunching in 30 s (restart {})", e.name,
                                  r.restarts);
                    } else {
                        r.state = Member::State::Done;
                        log::info("headless crew: [{}] ended (exit {})", e.name, e.exit_code);
                    }
                }
        }
        if (std::all_of(members.begin(), members.end(), [](const Member& r) { return r.state == Member::State::Done; }))
            break;
        if (now - last_status > std::chrono::seconds(30)) {
            last_status = now;
            std::size_t running = 0;
            for (const auto& r : members) running += r.state == Member::State::Running;
            log::info("headless crew: {} of {} running", running, members.size());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (stop_requested) {
        log::info("headless crew: stopping the children");
        children.stop_all(false);
        const auto end = Clock::now() + std::chrono::seconds(5);
        while (Clock::now() < end && children.running() > 0) {
            children.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        children.stop_all(true);
        for (const auto& r : members)
            if (!r.drive_file.empty()) {
                std::error_code ec;
                std::filesystem::remove(r.drive_file, ec);
            }
    }
    return 0;
}
} // namespace

int run_headless(HeadlessOptions options) {
    if (!options.crew_file.empty()) {
        stop_requested = 0;
        std::signal(SIGINT, on_stop_signal);
        std::signal(SIGTERM, on_stop_signal);
        return run_crew(options);
    }
    stop_requested = 0;
    std::signal(SIGINT, on_stop_signal);
    std::signal(SIGTERM, on_stop_signal);
    if (options.quiet) log::set_console(false); // everything still goes to the log file
    // Share a graphical client's folder when asked: its plugins, its
    // per-character plugin configs and its servers/ profiles are the crew's,
    // and an automation's config (e.g. HSMono's sentinel.json, which starts
    // its broker comms) lives there, not beside a worktree binary.
    if (!options.data_dir.empty()) platform::use_data_dir(options.data_dir, "--data-dir");
    if (std::string problem = platform::data_dir_problem(); !problem.empty()) {
        log::error("{}", problem);
        return 1;
    }
    // A saved server/account (--server/--account): the profile names the
    // login host, the keyring its password, the account its character and
    // world — as the graphical client's --account does. No password is
    // ever written to a file or a command line by this path.
    std::string saved_profile_id;
    std::vector<ServerProfile> saved_profiles;
    if (!options.server.empty() || !options.account.empty()) {
        saved_profiles = list_profiles();
        auto saved = resolve_saved(saved_profiles, options.server, options.account);
        if (!saved.problem.empty()) {
            log::error("headless: {}", saved.problem);
            return 1;
        }
        saved_profile_id = saved.profile->id;
        options.host = saved.profile->host;
        options.port = static_cast<std::uint16_t>(saved.profile->port);
        if (saved.account) {
            if (options.character.empty()) options.character = saved.account->character;
            if (options.world.empty()) options.world = saved.account->world;
            auto pw = platform::system_credentials().load(
                platform::credential_server(saved.profile->host, saved.profile->port), saved.account->username);
            if (!pw || pw->empty()) {
                log::error("headless: no saved password for {} on {}: log in once in the graphical client "
                           "with Save password ticked",
                           saved.account->username, saved.profile->id);
                return 1;
            }
            options.user = saved.account->username;
            options.password = std::move(*pw);
        }
        if (!saved.profile->game_files.empty() && !options.game_dir_given) options.game_dir = saved.profile->game_files;
        log::info("headless: saved server {} ({}:{}){}", saved.profile->id, options.host, options.port,
                  saved.account ? " as " + saved.account->username : "");
    }
    if (options.host.empty() || options.user.empty() || options.password.empty() || options.character.empty()) {
        log::error("headless: a login needs --host/--user/--password/--character, or --server/--account");
        return 1;
    }
    if (options.game_dir.empty()) {
        log::error("headless: --game-dir or EQ_GAME_DIR is required");
        return 1;
    }
    log::open_file(platform::logs_dir() /
                   ("headless-" + sanitize_file_name(options.character) + ".log"));
    GameDir game(options.game_dir);
    if (!saved_profile_id.empty()) game.set_overlay(profile_assets_dir(saved_profile_id));

    // Login, then play the --world server (or the first on the list).
    net::LoginClient login;
    login.start(options.host, options.port, options.user, options.password);
    core::wipe(options.password);
    auto deadline = Clock::now() + std::chrono::seconds(30);
    while (Clock::now() < deadline && !stop_requested) {
        login.update();
        if (login.state() == net::LoginState::LoggedIn) login.request_server_list();
        if (login.state() == net::LoginState::ServerList) {
            if (login.servers().empty()) { log::error("headless: no servers"); return 1; }
            if (options.list_servers) {
                for (const auto& s : login.servers())
                    std::printf("%u\t%s\tstatus %d\t%d players\n", s.id, s.name.c_str(), s.status, s.players);
                login.close();
                return 0;
            }
            auto id = headless_server_id(login.servers(), options.world);
            if (!id) {
                log::error("headless: no server matches --world \"{}\"; the list is:", options.world);
                for (const auto& s : login.servers()) log::error("  {}  {}", s.id, s.name);
                return 1;
            }
            for (const auto& s : login.servers())
                if (s.id == *id) log::info("headless: playing on {} ({})", s.name, s.id);
            login.request_play(*id);
        }
        if (login.state() == net::LoginState::PlayAccepted || login.state() == net::LoginState::Failed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (login.state() != net::LoginState::PlayAccepted) { log::error("headless login: {}", login.error()); return 1; }
    auto ticket = *login.ticket();
    login.close();
    log::info("headless: login ok, world {}:{}", ticket.world_host, ticket.world_port);

    std::unique_ptr<net::GameConnection> conn;
    std::optional<assets::Zone> zone;
    std::optional<game::CollisionMesh> collision;
    // The world this run plays on, as the login list named it: ${EverQuest.Server}
    // for the scripts and plugins (mq::Context::server_name). Automations key
    // real state on it — Sentinel scopes its broker heartbeat by it and finds
    // the character's config folder by it — so an empty answer silently
    // disables both, as an empty profile would in the graphical client.
    const std::string server_label = ticket.server_name;
    AutomationEnv env;
    env.install_dir = platform::install_dir();
    env.data_dir = platform::data_dir();
    env.autoload = options.plugin_autoload;
    env.server_name = [&server_label] { return server_label; };
    env.server_profile = [&server_label, &saved_profile_id] {
        return !saved_profile_id.empty() ? saved_profile_id : profile_id_for_world(server_label);
    };
    env.profile_name = [&server_label] { return server_label; };
    env.connection = [&conn] { return conn.get(); };
    Config ability_config;
    ActionsUi ability_actions(ability_config, {});
    UiHost ability_host;
    ability_host.actions = &ability_actions;
    env.ability = [&](mq::AbilityKind kind, std::string_view name) {
        return ability_state(ability_host, kind, name);
    };
    env.ability_bulk = [&] { return ability_bulk_rows(ability_host); };
    env.use_ability = [&](mq::AbilityKind kind, std::string_view name) {
        return use_ability(ability_host, kind, name);
    };
    env.window_open = [](std::string_view) { stub_once("window"); return false; };
    env.face = [](double, bool) { stub_once("face"); };
    env.keypress = [](std::string_view, int) { stub_once("keypress"); return false; };
    env.raise_window = [] { log::info("headless: /foreground ignored (no window)"); };
    env.line_of_sight = [&collision](const std::array<float, 3>& a, const std::array<float, 3>& b) -> std::optional<bool> {
        if (!collision) return std::nullopt;
        return collision->line_of_sight(glm::vec3(a[0], a[1], a[2]), glm::vec3(b[0], b[1], b[2]));
    };
    env.can_see = [&conn, &collision](std::uint32_t id) -> std::optional<bool> {
        if (!conn || !collision) return std::nullopt;
        const auto& w = conn->world();
        auto me = w.player();
        auto it = w.spawns.find(id);
        if (!me || it == w.spawns.end()) return std::nullopt;
        return collision->line_of_sight(me->position, it->second.position);
    };
    ability_host.run_button = [&](const game::HotButton& button) {
        if (!conn || !ability_host.character) return;
        AbilityView view;
        if (const auto* me = conn->world().player()) {
            view.position = me->position;
            view.heading = me->heading;
        }
        view.can_see_target = env.can_see(conn->target()).value_or(false);
        ability_actions.run_button(*conn, ability_host.character, view, button);
    };
    // Navigation (app/navigator.h) as the graphical client wires it, minus the
    // doors' models (no WorldObjects headless: the mesh runs through doorways,
    // and a door click still goes out when Navigator asks for one).
    float nav_radius = 1.78f; // the movement's sweep radius, kept in step below
    Navigator::Env nenv;
    nenv.spawn = [&conn](std::uint32_t id) -> const game::Spawn* {
        if (!conn) return nullptr;
        auto it = conn->world().spawns.find(id);
        return it == conn->world().spawns.end() ? nullptr : &it->second;
    };
    nenv.door = [&conn](std::uint8_t id) -> const game::Door* { return conn ? conn->door(id) : nullptr; };
    nenv.doors = [&conn] {
        std::vector<const game::Door*> v;
        if (conn)
            for (const auto& d : conn->doors()) v.push_back(&d);
        return v;
    };
    nenv.use_door = [&conn, &nav_radius](std::uint8_t id) { return conn && conn->click_door(id, nav_radius); };
    nenv.door_reachable = [&conn, &nav_radius](std::uint8_t id) { return conn && conn->door_in_reach(id, nav_radius); };
    nenv.chat = [](const std::string& t) { log::info("nav: {}", t); };
    nenv.request_server_path = [&conn](std::uint32_t id, game::ServerPathOrigin origin) {
        return conn ? conn->request_quiet_path(id, origin) : game::ServerPathLease{};
    };
    nenv.server_path = [&conn](const game::ServerPathLease& lease) {
        return conn ? conn->take_quiet_path(lease) : std::nullopt;
    };
    nenv.cancel_server_path = [&conn](const game::ServerPathLease& lease) {
        if (conn) conn->cancel_server_path(lease);
    };
    nenv.interaction_context = [&conn] { return conn ? conn->interaction_context() : 0; };
    nenv.started = [&conn] {
        if (conn && conn->follow_target()) conn->stop_follow(false);
    };
    nenv.line_of_sight = [&collision](const glm::vec3& a, const glm::vec3& b) {
        return collision && collision->line_of_sight(a, b);
    };
    nenv.threads = 2;
    nenv.prebuild = true;
    Navigator navigator(nenv);
    env.nav = [&navigator]() -> nav::NavControl* { return &navigator; };
    PlayerMovement movement;
    game::ZoneLineTrigger zone_line;
    Automation automation(env);

    std::unique_ptr<DriveObserveServer> observe;
    if (options.drive_observe) {
        try {
            observe = std::make_unique<DriveObserveServer>(platform::make_local_pipe_session(), options.drive_interact);
        } catch (const std::exception& e) {
            log::error("headless: --drive-observe could not start: {}", e.what());
            return 1;
        }
        if (const auto parent = std::filesystem::path(options.drive_file).parent_path(); !parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }
        log::info("headless: drive observe at {} (the file is written at the first zone-in)",
                  observe->session().endpoint);
    }

    net::ConnectionOptions copts;
    copts.hooks = &automation.hooks();
    copts.diagnostic_chat_log = options.chat_log;
    copts.plugins = automation.extensions();
    if (observe) {
        copts.snapshot_journal = true;
        copts.zone_packet_tap = [server = observe.get()](const net::AppPacket& p) {
            server->publish_zone_packet(p);
        };
    }
    if (!saved_profile_id.empty()) {
        // The character's files under the saved profile, its bank snapshot,
        // and the server's client files at the first zone-in, as in the GUI.
        copts.profile_id = saved_profile_id;
        copts.server_files_dir = profile_dir(saved_profile_id);
        copts.bank_snapshot_file = true;
    }
    conn = std::make_unique<net::GameConnection>(game, copts);
    conn->start(ticket);

    // The character sheet's tables (the GUI's StatsUi loads the same):
    // without them the Character's values are 0.
    game::mechanics::GameTables tables;
    if (!tables.load(game)) log::warn("headless: BaseData/SkillCaps missing; character values will be 0");
    std::unique_ptr<game::mechanics::CharacterSheet> sheet;
    std::unique_ptr<game::mechanics::Character> character;
    core::StepClock step_clock;
    step_clock.set_fixed(true);
    const auto start = Clock::now();
    bool entered = false, first_zone = true, consent_logged = false;
    bool movement_ready = false; // the movement body placed at a zone-in
    bool nav_was_active = false; // for the walk's speed line, once per walk
    glm::vec3 feet{0.0f};
    float yaw = 0.0f, pitch = 0.0f, floor_offset = 6.0f;
    // DoTeleportB on the player (game/zoning.h), as the graphical client's
    // teleport_b runs it with only the connection and collision behind its
    // hooks: a move inside this zone places the movement body there, another
    // zone goes out through MoveToZone. Shared by the zone lines below and
    // the server's OP_RequestClientZoneChange (a summon, #goto), which the
    // connection hands to the app's teleport handler.
    auto teleport_b = [&](const game::TeleportBArgs& a) -> bool {
        game::TeleportBPlayer p;
        const glm::vec3 at = feet + glm::vec3(0, 0, floor_offset);
        p.x = at.x;
        p.y = at.y;
        p.z = at.z;
        p.heading = game::yaw_to_heading(yaw);
        p.stand_state = conn->player_status().stand_state;
        p.zone = static_cast<std::int32_t>(conn->current_zone_dword());
        if (const game::Spawn* me = conn->world().player()) p.race = static_cast<std::int32_t>(me->race);
        bool local = false;
        game::TeleportBHooks b;
        b.send_client_update = [&] { conn->send_position_now(); };
        b.position_sample = [&](int kind) {
            if (kind != game::kPositionUpdate) conn->record_position_sample(static_cast<std::uint32_t>(kind));
        };
        b.save_physics = [] {};
        b.floor_height = [&](float fx, float fy, float fz, int) -> double {
            local = true;
            auto f = collision ? collision->floor_below({fx, fy, fz}, 0.0f) : std::nullopt;
            return f ? static_cast<double>(*f) : static_cast<double>(game::kNoFloor);
        };
        b.update_actor = [] {};
        b.find_person_request = [&] { conn->find_person_request(); };
        b.rewind_reset = [] {};
        b.move_to_zone = [&](const game::MoveToZoneCall& m) { conn->move_to_zone(m); };
        bool ok = game::do_teleport_b(p, a, 5, b);
        if (ok && local) {
            feet = glm::vec3(p.x, p.y, p.z - floor_offset);
            yaw = game::heading_to_yaw(p.heading);
            movement.place(feet);
            log::info("headless: moved within the zone to ({:.1f}, {:.1f}, {:.1f})", p.x, p.y, p.z);
        }
        return ok;
    };
    // A zone line under the feet (game/zone_lines.h): DoTeleport as the
    // graphical client's region_teleport runs it, with only the connection
    // and the collision mesh behind its hooks.
    auto region_teleport = [&](const std::string& text) {
        glm::vec3 pos = feet + glm::vec3(0, 0, floor_offset);
        const auto rows = conn->zone_point_rows();
        const std::uint32_t zone_dword = conn->current_zone_dword();
        game::DoTeleportInput in;
        in.index = -1;
        in.point[0] = pos.x;
        in.point[1] = pos.y;
        in.point[2] = pos.z;
        in.waiting_for_port = conn->waiting_for_port() ? 1 : 0;
        game::DoTeleportHooks h;
        h.resolve = [&](std::int32_t index, bool& has_description) {
            has_description = true;
            float loc[3] = {pos.x, pos.y, pos.z};
            auto t = game::resolve_teleport_index(text, index, static_cast<std::int32_t>(zone_dword), loc, rows);
            feet += glm::vec3(loc[0], loc[1], loc[2]) - pos;
            pos = glm::vec3(loc[0], loc[1], loc[2]);
            log::info("headless zone line {}: {} zone {:#x} point {} ({:.1f}, {:.1f}, {:.1f})", text,
                      t.ok ? "to" : "unresolved", t.found, t.zone_point, t.x, t.y, t.z);
            return t;
        };
        h.teleport_b = [&](std::int32_t to_zone, std::uint32_t x, std::uint32_t y, std::uint32_t z,
                           std::uint32_t heading) {
            game::TeleportBArgs a;
            a.zone = to_zone;
            a.x = std::bit_cast<float>(x);
            a.y = std::bit_cast<float>(y);
            a.z = std::bit_cast<float>(z);
            a.heading = std::bit_cast<float>(heading);
            return teleport_b(a);
        };
        h.send_index_request = [&](std::int32_t zp) { conn->send_teleport_index_request(zp); };
        h.position_sample = [&](int kind, const float* point) {
            const glm::vec3 at = point ? glm::vec3(point[0], point[1], point[2]) : pos;
            conn->record_position_sample(static_cast<std::uint32_t>(kind), &at);
        };
        h.free_description = [] {};
        h.get_spell_affect = [](std::int32_t) { return 0; };
        h.remove_affect = [](std::int32_t, std::int32_t) {};
        game::do_teleport(in, h);
        if (in.waiting_for_port == 0 && conn->waiting_for_port()) conn->clear_waiting_for_port();
    };
    // The connection's app hooks the graphical client installs per connection:
    // CastSpell's line-of-sight test, and OP_RequestClientZoneChange's
    // DoTeleportB (a summon or #goto in this zone moves the player instead of
    // zoning). Without the second, a server summon is answered with a zone
    // change the server never asked for and the movement body keeps
    // reporting the old position, so the character stays where it was.
    conn->set_line_of_sight([&collision](const glm::vec3& a, const glm::vec3& b) {
        return !collision || collision->line_of_sight(a, b);
    });
    conn->set_teleport_handler([&teleport_b](const game::TeleportBArgs& a) { teleport_b(a); });
    Clock::time_point in_zone_at{}, last_status{}, last_observe_state{};
    bool drive_file_written = false, drive_file_failed = false;
    std::uint64_t last_inventory_revision = ~std::uint64_t{0};
    std::string zone_seen;
    net::ConnectionState last = net::ConnectionState::Idle;
    int result = 1;
    while (!stop_requested) {
        const auto now_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
        auto steps = step_clock.advance(now_us);
        if (step_clock.take_dropped()) {
            // The game clock drops time past a 250 ms hitch (docs/mechanics.md
            // "Frame rate"): under load the movement then lags the wall clock,
            // which reads as walking slower than a graphical client.
            static Clock::time_point last_drop_log{};
            if (Clock::now() - last_drop_log > std::chrono::seconds(10)) {
                last_drop_log = Clock::now();
                log::warn("headless: the game clock dropped steps under load; movement lags the wall clock");
            }
        }
        if (conn && !steps.empty()) {
            std::vector<std::int64_t> at;
            const std::int64_t conn_now = net::GameConnection::now_ms();
            const auto real_ms = static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
            for (const auto& st : steps)
                at.push_back(conn_now - std::max<std::int64_t>(0, real_ms - static_cast<std::int64_t>(st.ms)));
            conn->set_game_steps(std::move(at));
        }
        conn->update();
        if (observe && options.drive_interact) {
            for (const auto& action : observe->take_actions()) {
                if (!conn->apply_drive_action(action)) break;
            }
        }
        if (observe && conn->in_zone()) {
            if (!drive_file_written && !drive_file_failed) {
                const std::string character_name = !options.character.empty() ? options.character : conn->character_name();
                const std::string text = "endpoint=" + observe->session().endpoint + "\n" +
                                         "token=" + observe->session().token + "\n" +
                                         "character=" + character_name + "\n" +
                                         "profile=" + saved_profile_id + "\n" +
                                         "game_dir=" + options.game_dir + "\n" +
                                         "interactive=" + (options.drive_interact ? "1\n" : "0\n");
                if (platform::write_private_file(options.drive_file, text)) {
                    drive_file_written = true;
                    log::info("headless: drive observe file {} written", options.drive_file);
                } else {
                    drive_file_failed = true;
                    log::error("headless: --drive-file {} could not be written (it must not exist yet)",
                               options.drive_file);
                }
            }
            if (observe->needs_snapshot()) observe->set_snapshot(conn->capture_snapshot());
            const auto now = Clock::now();
            if (now - last_observe_state >= std::chrono::milliseconds(100)) {
                last_observe_state = now;
                const auto local = conn->capture_local_state();
                DriveLocalState state;
                state.position = local.position;
                state.heading = local.heading;
                state.velocity = local.velocity;
                state.moving = local.moving;
                state.target = local.target;
                const std::uint64_t revision = conn->inventory().revision();
                if (revision != last_inventory_revision) {
                    last_inventory_revision = revision;
                    state.inventory_packet = local.inventory_packet;
                }
                if (options.drive_interact) state.ui = conn->capture_drive_ui();
                observe->publish_state(state);
            }
        }
        if (auto z = conn->take_zone_to_load()) {
            navigator.zone_unloaded();
            movement_ready = false;
            zone.reset();
            collision.reset();
            if (options.load_collision) {
                try {
                    zone = assets::load_zone(game, *z);
                    collision = game::CollisionMesh::build(*zone);
                    conn->reground();
                    navigator.zone_loaded(*collision, &zone->regions,
                                          platform::cache_dir() / "navmesh" /
                                              (server_label.empty() ? std::string("default")
                                                                    : profile_id_for_world(server_label)) /
                                              nav::ZoneNav::cache_name(*z));
                    log::info("headless: collision for {} loaded (no scene)", *z);
                } catch (const std::exception& e) {
                    log::warn("headless: zone {} collision unavailable: {}; continuing without it", *z, e.what());
                    zone.reset();
                    collision.reset();
                }
            }
        }
        ability_host.conn = conn.get();
        ability_host.in_game = conn->state() == net::ConnectionState::InZone;
        ability_host.character = nullptr;
        character.reset();
        if (ability_host.in_game && conn->world().player() && conn->profile()) {
            conn->refresh_worn_stats_disabled();
            if (!sheet) sheet = std::make_unique<game::mechanics::CharacterSheet>();
            game::mechanics::build_sheet_into(
                *sheet, *conn->profile(), conn->inventory(), conn->spell_state(), conn->live_state(),
                [&conn](std::vector<game::mechanics::SheetAbility>& out) { conn->aa_sheet_abilities_into(out); },
                conn->zone_info() ? &*conn->zone_info() : nullptr, &conn->world());
            sheet->prestige_limit = conn->game_feature(0x13);
            character = std::make_unique<game::mechanics::Character>(
                *sheet, tables, game::mechanics::spell_lookup(conn->spell_table()));
            character->process_all_stats();
            ability_host.character = character.get();
            const std::int64_t items = character->stats().hp_bonus;
            conn->set_own_hp_items(sheet->vitals.cur_hp ? 0 : items, sheet->vitals.max_hp ? 0 : items);
        }
        automation.frame();
        if (automation.plugin_consent() && !consent_logged) {
            consent_logged = true;
            log::error("headless: a server plugin waits for consent and cannot be asked here; "
                       "allow it from the graphical client on this server first (docs/headless.md)");
        }
        // The connection has already dispatched the chat hooks.
        while (conn->poll_chat()) {}
        if (conn->state() != last) {
            last = conn->state();
            log::info("[{}] {}", net::to_string(last), conn->status());
            if (!conn->error().empty()) log::info("  error: {}", conn->error());
        }
        if (last == net::ConnectionState::CharacterSelect && !entered) {
            conn->enter_world(options.character);
            entered = true;
        }
        if (last == net::ConnectionState::InZone) {
            if (zone_seen != conn->world().zone_short_name) {
                zone_seen = conn->world().zone_short_name;
                in_zone_at = Clock::now();
                log::info("headless: in {} as {}", zone_seen, options.character);
                for (const auto& line : headless_zone_commands(options, first_zone)) {
                    log::info("headless: exec: {}", line);
                    conn->send_command(line);
                }
                first_zone = false;
            }
            if (!steps.empty() && collision)
                conn->smooth(static_cast<float>(steps.size()) / core::StepClock::kRate,
                             [&collision](const glm::vec3& p) { return collision->floor_below(p, 0.5f); });
            // The movement body (app/player_movement.h), placed at the zone-in
            // and driven per game step by Navigator's steering, as the
            // graphical client drives it: /nav, /moveto and /stick walk here.
            if (const game::Spawn* me = conn->world().player(); me && !movement_ready) {
                conn->set_running(true); // a background character travels at a run (the GUI's run/walk toggle)
                floor_offset = me->heights.avatar_height;
                feet = me->position - glm::vec3(0, 0, floor_offset);
                yaw = game::heading_to_yaw(me->heading);
                pitch = 0.0f;
                movement.reset(feet, yaw);
                movement_ready = true;
                zone_line.reset();
                if (zone) (void)zone_line.update(feet, zone->regions);
            }
            if (movement_ready) {
                for (const auto& st : steps) {
                    const game::Spawn* me = conn->world().player();
                    if (!me) break;
                    const float dt_step = st.elapsed_ms ? static_cast<float>(st.elapsed_ms) / 1000.0f
                                                        : 1.0f / core::StepClock::kRate;
                    Navigator::Frame nf;
                    nf.feet = feet;
                    nf.now = std::chrono::duration<double>(start.time_since_epoch()).count() +
                             static_cast<double>(st.ms) / 1000.0;
                    const std::uint8_t stand = conn->player_status().stand_state;
                    nf.can_move = !conn->interface_locked() && !conn->player_status().dead &&
                                  (stand == net::kStandStanding || stand == net::kStandCrouching);
                    nf.player_input = false; // nobody's keys here: a script's own stop ends a walk
                    nf.height = floor_offset;
                    MovementKeys mk;
                    const nav::SteerOutput steer = navigator.frame(nf);
                    if (steer.active) {
                        yaw = steer.yaw;
                        mk.forward = steer.forward;
                        mk.jump = steer.jump;
                        mk.strafe_left = steer.strafe < 0;
                        mk.strafe_right = steer.strafe > 0;
                        if (steer.pitch) pitch = *steer.pitch;
                    }
                    if (conn->player_status().dead) mk = MovementKeys{};
                    ZoneMoveWorld world(collision ? &*collision : nullptr, nullptr,
                                        zone ? &zone->regions : nullptr);
                    world.apply_rules(conn->server_rules(), conn->doors(), navigator.passes_open_doors());
                    nav_radius = game::mechanics::sweep_radius(
                        game::mechanics::SweepRadiusSpawn{floor_offset, me->race});
                    movement.set_collision_radius(nav_radius);
                    movement.set_race(me->race);
                    PlayerMovement::Frame frame =
                        movement.update(mk, yaw, pitch, dt_step * 1000.0f,
                                        static_cast<std::uint32_t>(st.ms + 1), *conn,
                                        zone ? &zone->regions : nullptr, floor_offset, &world, character.get(),
                                        st.elapsed_ms);
                    feet = frame.feet;
                    yaw = frame.yaw;
                    pitch = std::clamp(frame.pitch, -1.5f, 1.5f);
                    // Once a second of walking: our own speed in units a
                    // second (the body's velocity a tick, at 60 ticks), and
                    // the fastest the target spawn moved in that second by
                    // its own update velocity (units a 50 ms tick, x20: a
                    // position average reads low whenever it stops or turns,
                    // and position deltas spike on a teleport or summon,
                    // whose updates carry no velocity), with the spawn
                    // record's run/walk multipliers both stand on
                    // (0.7 / 0.30 for a player before buffs).
                    if (steer.active) {
                        static int nav_steps = 0;
                        static float seen_fastest = 0.0f;
                        {
                            // Sampled every step, whatever is logged later.
                            std::uint32_t want = navigator.status().stick_spawn;
                            if (!want && conn->world().target_id) want = conn->world().target_id;
                            if (!want) {
                                const std::string goal = navigator.status().goal;
                                for (const auto& [id, s] : conn->world().spawns)
                                    if (game::display_name(s.name) == goal) { want = id; break; }
                            }
                            if (const auto s = conn->world().spawns.find(want); want && s != conn->world().spawns.end())
                                seen_fastest = std::max(seen_fastest, glm::length(glm::vec2(s->second.velocity)) * 20.0f);
                        }
                        if (++nav_steps % 60 == 1) {
                            const float own = glm::length(glm::vec2(frame.velocity)) * 60.0f;
                            std::string target = "none";
                            float trun = 0.0f, twalk = 0.0f;
                            // The spawn being walked to (Navigator's stick
                            // spawn, else its goal by name), else the target:
                            // a chase rarely leaves the leader targeted.
                            std::uint32_t goal_id = navigator.status().stick_spawn;
                            if (!goal_id && conn->world().target_id) goal_id = conn->world().target_id;
                            if (!goal_id) {
                                const std::string goal = navigator.status().goal;
                                for (const auto& [id, s] : conn->world().spawns)
                                    if (game::display_name(s.name) == goal) { goal_id = id; break; }
                            }
                            if (const auto t = conn->world().spawns.find(goal_id);
                                goal_id && t != conn->world().spawns.end()) {
                                target = game::display_name(t->second.name);
                                trun = t->second.run_speed;
                                twalk = t->second.walk_speed;
                            }
                            log::info("headless nav: own {:.1f} u/s (record run {:.2f} walk {:.2f}); target {} "
                                      "fastest {:.1f} u/s (record run {:.2f} walk {:.2f})",
                                      own, me->run_speed, me->walk_speed, target, seen_fastest, trun, twalk);
                            seen_fastest = 0.0f;
                        }
                    }
                    nav_was_active = steer.active;
                    for (const auto& line : frame.messages) log::info("movement: {}", line);
                    if (frame.stand_state) conn->set_stand_state(*frame.stand_state);
                    if (frame.sit_toggle) conn->sit_toggle();
                    if (frame.close_loot) conn->end_loot();
                    conn->set_speed_run(frame.speed_run);
                    conn->set_player_motion(feet + glm::vec3(0, 0, floor_offset), game::yaw_to_heading(yaw),
                                            frame.velocity, frame.moving);
                    conn->set_air_supply(movement.air_supply());
                    conn->set_standing_on(movement.standing_on());
                    if (zone)
                        if (auto text = zone_line.update(feet, zone->regions)) region_teleport(*text);
                }
            }
            if (options.seconds > 0 && Clock::now() - in_zone_at > std::chrono::seconds(options.seconds))
                conn->camp();
        }
        if (last == net::ConnectionState::Disconnected) {
            // 3 is the crew supervisor's relaunch signal: the link died
            // under a character that was in the world. A camp is 0.
            result = conn->error() == "Camped out." ? 0 : conn->link_death() ? 3 : 1;
            break;
        }
        if (!options.status_file.empty() && Clock::now() - last_status > std::chrono::seconds(1)) {
            last_status = Clock::now();
            std::ofstream(options.status_file, std::ios::trunc)
                << "state=" << net::to_string(last) << " zone=" << zone_seen << " character=" << options.character << "\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (stop_requested) log::info("headless: stop requested; shutting down");
    automation.shutdown();
    if (conn) conn->disconnect();
    return stop_requested ? 0 : result;
}

} // namespace hailstone::app
