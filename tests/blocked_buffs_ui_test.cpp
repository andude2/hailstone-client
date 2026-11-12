// The modern buff trays' "Add to Blocked Buffs" (ui/modern/lib/bufftray.lua): the Blocked Buffs window's own edit box
// and Add over the eq API (app/lua_eq_dialogs.cpp BlockedBuffWindow), with a buff's spell id, against the mock server.
#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_eq.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "net/packet_io.h"
#include "net/spell_packets.h"

using namespace hailstone;
using namespace hailstone::test;

TEST_CASE("Blocked buffs: the character's UI ini keeps the player's and pet's lists") {
    EmptyGameDir dir;
    GameDir game(dir.path());
    net::GameConnection conn(game);
    std::map<std::string, std::string> ini;
    auto key = [](std::string_view section, std::string_view name) {
        return std::string(section) + "." + std::string(name);
    };
    conn.set_ui_ini(
        [&](std::string_view section, std::string_view name) -> std::optional<std::string> {
            auto it = ini.find(key(section, name));
            return it == ini.end() ? std::nullopt : std::optional<std::string>(it->second);
        },
        [&](std::string_view section, std::string_view name, const std::string& value) {
            ini[key(section, name)] = value;
        });
    ini[key("BlockedBuffs", "Me")] = "11, bad, 717,-5";
    ini[key("BlockedBuffs", "Pet")] = "200";
    conn.load_blocked_buffs();
    CHECK(conn.blocked_buffs(false) == std::vector<std::int32_t>{11, 717});
    CHECK(conn.blocked_buffs(true) == std::vector<std::int32_t>{200});
    conn.save_blocked_buffs();
    CHECK(ini[key("BlockedBuffs", "Me")] == "11,717");
    CHECK(ini[key("BlockedBuffs", "Pet")] == "200");
}

TEST_CASE("Blocked buffs: a buff's spell id in the window's edit box, then Add, asks the server to block it") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        // Holy Armor (11), a beneficial spell, in the game folder's spell table (read when the connection is made).
        EmptyGameDir dir;
        std::ofstream(dir.path() / "spells_us.txt")
            << mq_test::spell_line({{0, "11"}, {1, "Holy Armor"}, {13, "2000"}, {17, "270"}, {83, "1"}, {98, "5"}})
            << "\n";
        GameDir game(dir.path());
        MockEqServer server(MockConfig::standard());
        net::GameConnection conn(game, quick_options());
        auto until = [&](const std::function<bool()>& done) {
            return pump([&] { conn.update(); server.poll(); }, done);
        };
        conn.start(server.ticket());
        REQUIRE(until([&] { return conn.state() == net::ConnectionState::CharacterSelect; }));
        conn.enter_world("Gennro");
        REQUIRE(until([&] { return conn.state() == net::ConnectionState::InZone && conn.player_status().max_hp != 0; }));
        REQUIRE(conn.spell_table().get(11));
        app::EqHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        app::EqApi api(host);
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "eq");
        REQUIRE(luaL_dostring(L, "eq.SetWindowText('BlockedBuffWnd', 'BW_EditSpellToBlock', '11') "
                                 "return eq.ClickChild('BlockedBuffWnd', 'BW_AddSpellToBlock', 'left')") == LUA_OK);
        CHECK(lua_toboolean(L, -1));
        lua_close(L);
        auto sent = [&] {
            return std::find_if(server.zone_log.begin(), server.zone_log.end(),
                                [](const net::AppPacket& p) { return net::is_op(p, net::Op::BlockedBuffs); });
        };
        REQUIRE(until([&] { return sent() != server.zone_log.end(); }));
        auto b = net::decode_blocked_buffs(*sent());
        REQUIRE(b);
        CHECK_FALSE(b->pet);
        REQUIRE_FALSE(b->spells.empty());
        CHECK(b->spells[0] == 11);
    }
    ImGui::DestroyContext(ctx);
}
