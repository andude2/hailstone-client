// The headless host's argument and zone-in command schedule (app/headless_app.h):
// pure, so it is tested without a server.
#include <doctest/doctest.h>

#include "app/headless_app.h"
#include "core/character_files.h"
#include "core/game_dir.h"
#include "core/log.h"
#include "net/game_connection.h"
#include "net/login.h"
#include "plugin/hooks.h"
#include "scratch_dir.h"

#include <fstream>
#include <iterator>

using namespace hailstone;

TEST_CASE("headless: diagnostic chat logging preserves chat delivery") {
    bool enabled = true;
    SUBCASE("enabled") {}
    SUBCASE("disabled") { enabled = false; }

    test::ScratchDir scratch("headless_chat");
    const auto file = scratch.path() / "diagnostic.log";
    GameDir game(scratch.path());
    plugin::Hooks hooks;
    int delivered = 0;
    hooks.on_chat(hooks.new_owner("test"), [&](plugin::ChatLine& line) {
        ++delivered;
        line.text = "headless chat test";
        return plugin::Flow::Continue;
    });
    net::ConnectionOptions options;
    options.hooks = &hooks;
    options.diagnostic_chat_log = enabled;
    net::GameConnection conn(game, options);

    REQUIRE(log::open_file(file));
    conn.system_message(1);
    log::close_file();

    CHECK(delivered == 1);
    const auto event = conn.poll_chat();
    REQUIRE(event.has_value());
    CHECK(event->text == "headless chat test");
    std::ifstream log_file(file);
    REQUIRE(log_file.is_open());
    const std::string contents{std::istreambuf_iterator<char>(log_file), {}};
    const std::string_view entry = "[info] chat: headless chat test";
    const auto pos = contents.find(entry);
    if (enabled) {
        REQUIRE(pos != std::string::npos);
        CHECK(contents.find(entry, pos + entry.size()) == std::string::npos);
    } else {
        CHECK(pos == std::string::npos);
    }
}

TEST_CASE("headless: arguments parse, host port split, defaults") {
    auto a = app::parse_headless_args({"--host", "login.example:5999", "--user", "u", "--password", "p",
                                       "--character", "Cleric", "--game-dir", "/games/eq", "--exec",
                                       "/mono load Sentinel", "--exec-zone", "/say ready", "--seconds", "60"});
    CHECK(a.error.empty());
    CHECK(a.options.host == "login.example");
    CHECK(a.options.port == 5999);
    CHECK(a.options.character == "Cleric");
    CHECK(a.options.seconds == 60);
    CHECK(a.options.load_collision);
    REQUIRE(a.options.exec.size() == 1);
}

TEST_CASE("headless: missing and unknown arguments are refused") {
    CHECK_FALSE(app::parse_headless_args({"--host", "h"}).error.empty());
    CHECK_FALSE(app::parse_headless_args({"--host", "h", "--user", "u", "--password", "p", "--character", "c",
                                          "--game-dir", "/g", "--bogus"})
                    .error.empty());
    CHECK_FALSE(app::parse_headless_args({"--host"}).error.empty());
}

TEST_CASE("headless: --world picks a server by id, exact name, or unique part of the name") {
    const std::vector<hailstone::net::ServerInfo> servers{{101, "Alpha"}, {202, "EZ Server"}, {303, "EZ Test"}};
    CHECK(app::headless_server_id(servers, "") == 101u);          // empty: the first
    CHECK(app::headless_server_id(servers, "202") == 202u);       // a numeric id
    CHECK(app::headless_server_id(servers, "ez server") == 202u); // exact name, any case
    CHECK(app::headless_server_id(servers, "Alpha") == 101u);
    CHECK(app::headless_server_id(servers, "Test") == 303u); // a unique part of the name
    CHECK_FALSE(app::headless_server_id(servers, "EZ").has_value());   // part of two names: refused
    CHECK_FALSE(app::headless_server_id(servers, "Nope").has_value()); // no such server
    CHECK_FALSE(app::headless_server_id({}, "").has_value());
    auto a = app::parse_headless_args({"--host", "h", "--user", "u", "--password", "p", "--character", "c",
                                       "--game-dir", "/g", "--world", "EZ Server", "--data-dir", "/crew/hailstone"});
    CHECK(a.error.empty());
    CHECK(a.options.world == "EZ Server");
    CHECK(a.options.data_dir == "/crew/hailstone");
}

TEST_CASE("headless: --exec runs only at the first zone-in, --exec-zone at every one") {
    app::HeadlessOptions o;
    o.exec = {"/mono load Sentinel"};
    o.exec_zone = {"/say ready"};
    auto first = app::headless_zone_commands(o, true);
    REQUIRE(first.size() == 2);
    CHECK(first[0] == "/say ready");
    CHECK(first[1] == "/mono load Sentinel");
    auto later = app::headless_zone_commands(o, false);
    REQUIRE(later.size() == 1);
    CHECK(later[0] == "/say ready");
}

TEST_CASE("headless: --server/--account parse without host or password; --crew stands alone") {
    auto a = app::parse_headless_args({"--server", "ez", "--account", "crew1"});
    CHECK(a.error.empty());
    CHECK(a.options.server == "ez");
    CHECK(a.options.account == "crew1");
    auto c = app::parse_headless_args({"--crew", "crew.ini"});
    CHECK(c.error.empty());
    CHECK(c.options.crew_file == "crew.ini");
    auto q = app::parse_headless_args({"--host", "h", "--user", "u", "--password", "p", "--character", "c",
                                       "--game-dir", "/g", "--autoload", "HSMono", "--no-chat-log", "--quiet"});
    CHECK(q.error.empty());
    CHECK(q.options.plugin_autoload == "HSMono");
    CHECK_FALSE(q.options.chat_log);
    CHECK(q.options.quiet);
}

TEST_CASE("headless: a saved server is picked by id, name, or a unique part") {
    const std::vector<hailstone::ServerProfile> profiles{{"ez-server", "EZ Server"}, {"peq", "ProjectEQ"}};
    REQUIRE(app::pick_headless_profile(profiles, "ez-server"));
    CHECK(app::pick_headless_profile(profiles, "ez-server")->name == "EZ Server");
    CHECK(app::pick_headless_profile(profiles, "projecteq")->id == "peq");
    CHECK(app::pick_headless_profile(profiles, "EZ")->id == "ez-server");
    CHECK_FALSE(app::pick_headless_profile(profiles, "e")); // in both names/ids
    CHECK_FALSE(app::pick_headless_profile(profiles, "nope"));
}

TEST_CASE("headless: a crew file parses members and shared keys; a member without an account is refused") {
    auto crew = app::parse_crew_file("[crew]\n"
                                     "data_dir=D:/Hailstone\n"
                                     "stagger=5\n"
                                     "autoload=HSMono\n"
                                     "observe=1\n"
                                     "drive_dir=run\n"
                                     "\n"
                                     "[Okhealz]\n"
                                     "server=ez-server\n"
                                     "account=crew1\n"
                                     "exec=/mono load Sentinel\n"
                                     "\n"
                                     "[Tankguy]\n"
                                     "server=EZ Server\n"
                                     "account=crew2\n"
                                     "character=Tankguy\n"
                                     "seconds=600\n"
                                     "observe=0\n");
    CHECK(crew.error.empty());
    CHECK(crew.data_dir == "D:/Hailstone");
    CHECK(crew.stagger == 5);
    CHECK(crew.autoload == "HSMono");
    CHECK(crew.observe);
    CHECK(crew.drive_dir == "run");
    CHECK_FALSE(crew.chat_log); // a crew's children do not print chat unless asked
    REQUIRE(crew.members.size() == 2);
    CHECK(crew.members[0].observe == -1); // the shared [crew] value
    CHECK(crew.members[1].observe == 0);
    CHECK(crew.members[0].label == "Okhealz");
    CHECK(crew.members[0].exec == "/mono load Sentinel");
    CHECK(crew.members[1].character == "Tankguy");
    CHECK(crew.members[1].seconds == 600);
    CHECK_FALSE(app::parse_crew_file("[Lone]\nserver=ez\n").error.empty());
    CHECK_FALSE(app::parse_crew_file("[crew]\nstagger=5\n").error.empty());
}

TEST_CASE("headless: --drive-control parses, needs a file, and is exclusive with observe") {
    auto c = app::parse_headless_args({"--host", "h", "--user", "u", "--password", "p", "--character", "c",
                                       "--game-dir", "/g", "--drive-control", "--drive-file", "run/drive-L.txt"});
    CHECK(c.error.empty());
    CHECK(c.options.drive_control);
    CHECK_FALSE(c.options.drive_observe);
    CHECK(c.options.drive_file == "run/drive-L.txt");
    CHECK_FALSE(app::parse_headless_args({"--host", "h", "--user", "u", "--password", "p", "--character", "c",
                                          "--game-dir", "/g", "--drive-control"})
                    .error.empty());
    CHECK_FALSE(app::parse_headless_args({"--host", "h", "--user", "u", "--password", "p", "--character", "c",
                                          "--game-dir", "/g", "--drive-observe", "--drive-control",
                                          "--drive-file", "f"})
                    .error.empty());
}

TEST_CASE("headless: a crew's control key parses like observe") {
    auto crew = app::parse_crew_file(
        "[crew]\ncontrol=1\n\n[A]\nserver=s\naccount=a\n\n[B]\nserver=s\naccount=b\ncontrol=0\n");
    CHECK(crew.error.empty());
    CHECK(crew.control);
    REQUIRE(crew.members.size() == 2);
    CHECK(crew.members[0].control == -1); // the shared [crew] value
    CHECK(crew.members[1].control == 0);
}

TEST_CASE("headless: an --exec line without its slash is still a command, not a say") {
    // A shell may eat the leading '/' of a quoted argument; --exec names a
    // command line, so the slash is put back rather than said in the zone.
    app::HeadlessOptions o;
    o.exec = {"mono load Sentinel"};
    o.exec_zone = {"say ready"};
    auto first = app::headless_zone_commands(o, true);
    REQUIRE(first.size() == 2);
    CHECK(first[0] == "/say ready");
    CHECK(first[1] == "/mono load Sentinel");
}

TEST_CASE("headless: format_crew_file round-trips and clear_stale_drive_files removes only drive-*.txt") {
    app::CrewFile crew;
    crew.data_dir = "C:/Hailstone";
    crew.game_dir = "C:/EQ";
    crew.stagger = 3;
    crew.autoload = "HSMono, ACNet";
    crew.chat_log = false;
    crew.observe = true;
    crew.interact = true;
    crew.control = true;
    crew.drive_dir = "run";
    crew.members.push_back(app::CrewMember{
        .label = "Okhealz", .server = "eqemulator", .account = "acc1", .character = "Okhealz", .world = "EZ"});
    crew.members.push_back(app::CrewMember{
        .label = "Okhealz", .server = "eqemulator", .account = "acc2", .character = "Tankguy"});

    const std::string formatted = app::format_crew_file(crew);
    const app::CrewFile parsed = app::parse_crew_file(formatted);
    CHECK(parsed.error.empty());
    CHECK(parsed.data_dir == "C:/Hailstone");
    CHECK(parsed.game_dir == "C:/EQ");
    CHECK(parsed.stagger == 3);
    CHECK(parsed.autoload == "HSMono, ACNet");
    CHECK(parsed.observe);
    CHECK(parsed.interact);
    CHECK(parsed.control);
    CHECK(parsed.drive_dir == "run");
    REQUIRE(parsed.members.size() == 2);
    CHECK(parsed.members[0].label == "Okhealz");
    CHECK(parsed.members[0].world == "EZ");
    CHECK(parsed.members[1].label == "Okhealz-2"); // duplicate label deduplicated
    CHECK(parsed.members[1].account == "acc2");

    const auto resolved = app::crew_drive_dir(parsed, std::filesystem::path("C:/Hailstone"));
    CHECK(resolved == std::filesystem::path("C:/Hailstone") / "run");

    test::ScratchDir scratch("headless_stale_drive");
    const auto run_dir = scratch.path() / "run";
    std::filesystem::create_directories(run_dir);
    std::ofstream(run_dir / "drive-Okhealz.txt") << "stale\n";
    std::ofstream(run_dir / "keep.txt") << "keep\n";
    app::clear_stale_drive_files(run_dir);
    CHECK_FALSE(std::filesystem::exists(run_dir / "drive-Okhealz.txt"));
    CHECK(std::filesystem::exists(run_dir / "keep.txt"));
}

TEST_CASE("headless: seed_headless_imgui_ini migrates legacy file, seeds from GUI once, and keeps existing") {
    test::ScratchDir scratch("headless_imgui_ini_seed");
    const auto gui_ini = scratch.path() / "hailstone-windows.ini";
    const auto char_files = character_files("eqemulator", "EZ", "Wikain", scratch.path() / "servers");
    const auto char_ini = char_files.headless_imgui_ini();
    CHECK(is_kept_character_file("imgui-headless.ini"));
    std::ofstream(gui_ini) << "[Window][SentinelHeartbeatBars]\nPos=2216,28\nSize=340,1274\n";

    // Without a legacy root file, seed from the GUI's hailstone-windows.ini inside the character's folder.
    app::seed_headless_imgui_ini(char_ini, gui_ini, scratch.path() / "imgui-headless-Wikain.ini");
    REQUIRE(std::filesystem::is_regular_file(char_ini));
    {
        std::ifstream in(char_ini);
        std::string content((std::istreambuf_iterator<char>(in)), {});
        CHECK(content.find("Pos=2216,28") != std::string::npos);
    }

    // Subsequent runs keep the character's own headless layout instead of overwriting it from the GUI ini.
    std::ofstream(char_ini, std::ios::trunc) << "[Window][SentinelHeartbeatBars]\nPos=1900,40\nSize=340,900\n";
    app::seed_headless_imgui_ini(char_ini, gui_ini);
    {
        std::ifstream in(char_ini);
        std::string content((std::istreambuf_iterator<char>(in)), {});
        CHECK(content.find("Pos=1900,40") != std::string::npos);
        CHECK(content.find("Pos=2216,28") == std::string::npos);
    }

    // A legacy root imgui-headless-<char>.ini is migrated into the character folder and removed from the root.
    const auto legacy_ini = scratch.path() / "imgui-headless-Ashenra.ini";
    const auto migrated_ini = character_files("eqemulator", "EZ", "Ashenra", scratch.path() / "servers").headless_imgui_ini();
    std::ofstream(legacy_ini) << "[Window][SentinelHeartbeatBars]\nPos=2205,347\nSize=340,859\n";
    app::seed_headless_imgui_ini(migrated_ini, gui_ini, legacy_ini);
    CHECK_FALSE(std::filesystem::exists(legacy_ini));
    REQUIRE(std::filesystem::is_regular_file(migrated_ini));
    {
        std::ifstream in(migrated_ini);
        std::string content((std::istreambuf_iterator<char>(in)), {});
        CHECK(content.find("Pos=2205,347") != std::string::npos);
    }
}
