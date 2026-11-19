#include "app/headless_app.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>

#include "app/automation.h"
#include "app/navigator.h"
#include "app/player_movement.h"
#include "app/zone_move_world.h"
#include "assets/zone.h"
#include "core/character_files.h"
#include "core/game_dir.h"
#include "core/log.h"
#include "core/step_clock.h"
#include "core/wipe.h"
#include "game/collision.h"
#include "game/coords.h"
#include "game/mechanics/collision_movement.h"
#include "game/mounts.h"
#include "game/position_tracker.h"
#include "game/zone_lines.h"
#include "game/zoning.h"
#include "nav/zone_nav.h"
#include "net/game_connection.h"
#include "net/login.h"
#include "platform/paths.h"

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
        if (a == "--game-dir") out.options.game_dir = need(a);
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
        else if (a == "--list-servers") out.options.list_servers = true;
        else if (a == "--exec") out.options.exec.emplace_back(need(a));
        else if (a == "--exec-zone") out.options.exec_zone.emplace_back(need(a));
        else if (a == "--seconds") out.options.seconds = std::atoi(std::string(need(a)).c_str());
        else if (a == "--status-file") out.options.status_file = need(a);
        else if (a == "--no-collision") out.options.load_collision = false;
        else if (!out.error.empty()) break;
        else out.error = "unknown argument: " + std::string(a);
        if (!out.error.empty()) break;
    }
    if (out.error.empty()) {
        if (out.options.host.empty()) out.error = "--host is required";
        else if (out.options.user.empty()) out.error = "--user is required";
        else if (out.options.password.empty()) out.error = "--password is required";
        else if (out.options.character.empty() && !out.options.list_servers) out.error = "--character is required";
        else if (out.options.game_dir.empty()) out.error = "--game-dir or EQ_GAME_DIR is required";
    }
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

int run_headless(HeadlessOptions options) {
    stop_requested = 0;
    std::signal(SIGINT, on_stop_signal);
    std::signal(SIGTERM, on_stop_signal);
    // Share a graphical client's folder when asked: its plugins, its
    // per-character plugin configs and its servers/ profiles are the crew's,
    // and an automation's config (e.g. HSMono's sentinel.json, which starts
    // its broker comms) lives there, not beside a worktree binary.
    if (!options.data_dir.empty()) platform::use_data_dir(options.data_dir, "--data-dir");
    if (std::string problem = platform::data_dir_problem(); !problem.empty()) {
        log::error("{}", problem);
        return 1;
    }
    log::open_file(platform::logs_dir() / "headless.log");
    GameDir game(options.game_dir);

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
    env.autoload = "*";
    env.server_name = [&server_label] { return server_label; };
    env.server_profile = [&server_label] { return profile_id_for_world(server_label); };
    env.profile_name = [&server_label] { return server_label; };
    env.connection = [&conn] { return conn.get(); };
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

    net::ConnectionOptions copts;
    copts.hooks = &automation.hooks();
    copts.plugins = automation.extensions();
    conn = std::make_unique<net::GameConnection>(game, copts);
    conn->start(ticket);

    core::StepClock step_clock;
    step_clock.set_fixed(true);
    const auto start = Clock::now();
    bool entered = false, first_zone = true, consent_logged = false;
    bool movement_ready = false; // the movement body placed at a zone-in
    glm::vec3 feet{0.0f};
    float yaw = 0.0f, pitch = 0.0f, floor_offset = 6.0f;
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
            }
            return ok;
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
    Clock::time_point in_zone_at{}, last_status{};
    std::string zone_seen;
    net::ConnectionState last = net::ConnectionState::Idle;
    int result = 1;
    while (!stop_requested) {
        const auto now_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
        auto steps = step_clock.advance(now_us);
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
        automation.frame();
        if (automation.plugin_consent() && !consent_logged) {
            consent_logged = true;
            log::error("headless: a server plugin waits for consent and cannot be asked here; "
                       "allow it from the graphical client on this server first (docs/headless.md)");
        }
        while (auto c = conn->poll_chat()) log::info("chat: {}", c->text);
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
                                        zone ? &zone->regions : nullptr, floor_offset, &world, nullptr,
                                        st.elapsed_ms);
                    feet = frame.feet;
                    yaw = frame.yaw;
                    pitch = std::clamp(frame.pitch, -1.5f, 1.5f);
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
            result = conn->error() == "Camped out." ? 0 : 1;
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
