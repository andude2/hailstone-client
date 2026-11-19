// The headless host's argument and zone-in command schedule (app/headless_app.h):
// pure, so it is tested without a server.
#include <doctest/doctest.h>

#include "app/headless_app.h"
#include "net/login.h"

using namespace hailstone;

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
