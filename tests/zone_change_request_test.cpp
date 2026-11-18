// OP_RequestClientZoneChange goes to DoTeleportB (CEverQuest::HandleWorldMessage @0x4d13cd) when the app gives the
// connection a teleport handler: a #goto in the player's own zone moves the player instead of zoning.
#include <doctest/doctest.h>

#include "core/byte_io.h"
#include "net/packet_io.h"
#include "net/zone_packets.h"
#include "mq_test_world.h"

using namespace hailstone;

TEST_CASE("OP_RequestClientZoneChange hands DoTeleportB the zone dword, world x / y, z, heading, type and +4") {
    mq_test::World w("hailstone_zone_change_request_test");
    net::GameConnection& c = *w.conn;
    std::optional<game::TeleportBArgs> got;
    c.set_teleport_handler([&](const game::TeleportBArgs& a) { got = a; });

    ByteWriter b;
    b.le<std::uint16_t>(1);            // zone
    b.le<std::uint16_t>(2);            // instance
    b.le<std::uint32_t>(0xffffffffu);  // +4
    b.f32(-186.0f);                    // y (the server's)
    b.f32(-371.0f);                    // x
    b.f32(4.0f);
    b.f32(128.0f);
    b.le<std::uint32_t>(1);            // type
    for (int i = 0; i < 144 / 4 + 1; ++i) b.le<std::uint32_t>(0);
    c.receive_zone_packet_for_test(net::make_packet(net::Op::RequestClientZoneChange, std::move(b)));

    REQUIRE(got);
    CHECK(got->zone == (1 | (2 << 16)));
    CHECK(got->x == doctest::Approx(-186.0f)); // world x is the server's y
    CHECK(got->y == doctest::Approx(-371.0f));
    CHECK(got->z == doctest::Approx(4.0f));
    CHECK(got->heading == doctest::Approx(128.0f));
    CHECK(got->reason == 1);
    CHECK(got->arg8 == -1);
}

TEST_CASE("The zone dword carries the player profile's instance: a summon inside an instance moves, not zones") {
    mq_test::World w("hailstone_zone_change_instance_test");
    net::GameConnection& c = *w.conn;
    // OP_NewZone as stock EQEmu sends it: the zone id, the instance left at 0.
    ByteWriter nz;
    for (std::size_t i = 0; i < 852; ++i) nz.le<std::uint8_t>(0);
    nz.le<std::uint16_t>(89); // zone id
    nz.le<std::uint16_t>(0);  // instance
    while (nz.data().size() < net::kNewZoneSize) nz.le<std::uint8_t>(0);
    c.receive_zone_packet_for_test(net::make_packet(net::Op::NewZone, std::move(nz)));
    REQUIRE(c.zone_id() == 89);
    CHECK(c.current_zone_dword() == 89u);
    // The profile says which instance (PlayerProfile zoneInstance).
    net::PlayerProfile pp;
    pp.zone_id = 89;
    pp.zone_instance = 968;
    c.set_profile_for_test(pp);
    CHECK(c.zone_instance() == 968);
    CHECK(c.current_zone_dword() == (89u | (968u << 16)));
    // A profile for another zone (left from before) does not count.
    pp.zone_id = 22;
    c.set_profile_for_test(pp);
    CHECK(c.current_zone_dword() == 89u);
}

TEST_CASE("OP_NewZone's suspend_buffs (+876) says whether the zone holds the suspendable buffs") {
    mq_test::World w("hailstone_zone_suspend_buffs_test");
    net::GameConnection& c = *w.conn;
    auto new_zone = [](std::uint32_t suspend) {
        ByteWriter nz;
        for (std::size_t i = 0; i < 852; ++i) nz.le<std::uint8_t>(0);
        nz.le<std::uint16_t>(202); // zone id
        nz.le<std::uint16_t>(0);
        while (nz.data().size() < 876) nz.le<std::uint8_t>(0);
        nz.le<std::uint32_t>(suspend);
        while (nz.data().size() < net::kNewZoneSize) nz.le<std::uint8_t>(0);
        return net::make_packet(net::Op::NewZone, std::move(nz));
    };
    CHECK_FALSE(c.spell_state().zone_suspends_buffs);
    c.receive_zone_packet_for_test(new_zone(1));
    CHECK(c.spell_state().zone_suspends_buffs);
    c.receive_zone_packet_for_test(new_zone(0));
    CHECK_FALSE(c.spell_state().zone_suspends_buffs);
}

TEST_CASE("A new connection has none of the app's hooks, even where the freed one was allocated") {
    // The app gives each connection its line of sight, teleport and slash-command hooks once, when the connection
    // has none (client_app_player.cpp): a test on the connection's address skipped a new one put where the old was.
    // This checks only the condition the app tests; the installation in ClientApp::update_player is not unit-tested
    // (ClientApp needs a window).
    mq_test::World w("hailstone_zone_change_request_hooks_test");
    CHECK_FALSE(w.conn->has_line_of_sight());
    w.conn->set_line_of_sight([](const glm::vec3&, const glm::vec3&) { return true; });
    CHECK(w.conn->has_line_of_sight());
    w.conn.reset();
    w.conn = std::make_unique<net::GameConnection>(*w.game, net::ConnectionOptions{});
    CHECK_FALSE(w.conn->has_line_of_sight());
}

TEST_CASE("OP_NewZone: a short name that is a path is not used for the zone's files") {
    // It names the zone's archives, map layers and navmesh cache (core/zone_name.h).
    mq_test::World w("hailstone_new_zone_name_test");
    net::GameConnection& c = *w.conn;
    net::NewZone nz;
    nz.zone_long_name = "Nowhere";
    for (const char* bad : {"../../../EverQuest/spells_us", "/home/user/notes", "C:\\EQ\\spells_us", "nul"}) {
        nz.zone_short_name = bad;
        c.receive_zone_packet_for_test(net::encode(nz));
        CHECK(c.world().zone_short_name.empty());
        CHECK(c.take_zone_to_load() == std::optional<std::string>(""));
    }
    nz.zone_short_name = "qeynos2";
    c.receive_zone_packet_for_test(net::encode(nz));
    CHECK(c.world().zone_short_name == "qeynos2");
    CHECK(c.take_zone_to_load() == std::optional<std::string>("qeynos2"));
}
