// A server's plugin puts spawns on the maps (game/server_ext.h map_spawn, app/lua_eq_map.cpp): the minimap, the zone
// map and the map window draw the marks it gives, and nothing more without it.
#include <doctest/doctest.h>

#include <fstream>
#include <memory>
#include <optional>
#include <string>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_eq.h"
#include "game/server_ext.h"
#include "mq_test_world.h"
#include "scratch_dir.h"

using namespace hailstone;

namespace {

struct ShowRat final : game::ServerExtension {
    bool on = true;
    mutable int rat_asked = 0;
    std::optional<game::MapSpawnMark> map_spawn(const net::GameConnection&, const game::Spawn& s,
                                                const game::MapSpawnMark& defaults) const override {
        if (s.id == 10) ++rat_asked;
        if (!on || s.id != 10) return std::nullopt;
        CHECK(defaults.label == "a rat"); // the display name
        return game::MapSpawnMark{0x123456, 0, "Rat!"};
    }
};

constexpr ImU32 kMark = IM_COL32(0x12, 0x34, 0x56, 0xff);

// Draws `code` (eq.* calls) in a window and counts the vertices in the mark's colour (or with `rgb`, in that colour
// at any alpha).
int marked_vertices(lua_State* L, const std::string& code, std::optional<ImU32> rgb = std::nullopt) {
    ImGui::GetIO().DisplaySize = ImVec2(800, 600);
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(600, 500));
    ImGui::Begin("map");
    if (luaL_dostring(L, code.c_str()) != LUA_OK) {
        FAIL(lua_tostring(L, -1));
        lua_settop(L, 0);
    }
    int n = 0;
    for (const ImDrawVert& v : ImGui::GetWindowDrawList()->VtxBuffer)
        if (rgb ? (v.col & 0x00ffffffu) == *rgb : v.col == kMark) ++n;
    ImGui::End();
    ImGui::Render();
    return n;
}

// Runs `code` in a window with the mouse at `mouse` (the map hit tests use it) and returns its string answer.
std::string lua_answer(lua_State* L, const std::string& code, ImVec2 mouse) {
    ImGui::GetIO().DisplaySize = ImVec2(800, 600);
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::GetIO().MousePos = mouse;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(600, 500));
    ImGui::Begin("map");
    std::string out;
    if (luaL_dostring(L, code.c_str()) == LUA_OK) {
        if (const char* s = lua_tostring(L, -1)) out = s;
        lua_settop(L, 0);
    } else {
        FAIL(lua_tostring(L, -1));
        lua_settop(L, 0);
    }
    ImGui::End();
    ImGui::Render();
    return out;
}

} // namespace

TEST_CASE("Map spawns: a plugin's marks on the minimap, the zone map and the map window") {
    auto ext = std::make_shared<ShowRat>();
    net::ConnectionOptions opts;
    opts.plugins = ext;
    mq_test::World world("hailstone_map_spawns_test", opts);
    test::ScratchDir scratch("map_spawns_test");
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        app::EqHost host;
        host.in_game = host.ui_ready = true;
        host.game = world.game.get();
        host.conn = world.conn.get();
        host.zone_short_name = "qeynos2";
        host.maps_dir = scratch.path() / "maps";
        host.feet_valid = true; // the player at the origin; the rat (spawn 10) 50 away
        app::EqApi api(host);
        api.begin_frame();
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "eq");

        CHECK(marked_vertices(L, "eq.DrawMinimap(300, 250, 100, 500)") > 0);
        // The rat is 50 map units from the player, 10 screen pixels on this 500-unit circle; a click there names it.
        CHECK(lua_answer(L, "local _, _, id, name = eq.DrawMinimap(300, 250, 100, 500) return tostring(id) .. ' ' .. name",
                         ImVec2(294, 242)) == "10 Rat!");
        CHECK(lua_answer(L, "local _, _, id = eq.DrawMinimap(300, 250, 100, 500) return tostring(id)",
                         ImVec2(300, 250)) == "0");
        world.conn->set_target(0);
        lua_answer(L, "eq.TargetSpawn(10) return ''", ImVec2(0, 0));
        CHECK(world.conn->world().target_id == 10);
        CHECK(marked_vertices(L, "eq.DrawZoneMap(0, 0, 600, 500, 1, 0, 0, true)") > 0);
        CHECK(marked_vertices(L, "eq.DrawMap(0, 0, 600, 500)") > 0); // the map window
        // Outside the minimap's reach: not drawn.
        CHECK(marked_vertices(L, "eq.DrawMinimap(300, 250, 100, 20)") == 0);
        // ... nor is the plugin asked about it there; the zone map, a rectangle, still asks.
        ext->rat_asked = 0;
        marked_vertices(L, "eq.DrawMinimap(300, 250, 100, 20)");
        CHECK(ext->rat_asked == 0);
        marked_vertices(L, "eq.DrawMinimap(300, 250, 100, 500)");
        CHECK(ext->rat_asked == 1);
        marked_vertices(L, "eq.DrawZoneMap(0, 0, 600, 500, 1, 0, 0, true)");
        CHECK(ext->rat_asked == 2);
        ext->on = false;
        CHECK(marked_vertices(L, "eq.DrawMinimap(300, 250, 100, 500)") == 0);
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Map lines: the minimap draws the lines that cross its circle, however far their ends") {
    mq_test::World world("hailstone_map_lines_test");
    test::ScratchDir scratch("map_lines_test");
    std::filesystem::create_directories(scratch.path() / "maps");
    {
        // Through the player (at the origin), its ends 1000 away; and one 5000 away.
        std::ofstream f(scratch.path() / "maps" / "qeynos2.txt");
        f << "L -1000, 0, 0,  1000, 0, 0,  0, 255, 0\n";
        f << "L 5000, 5000, 0,  5100, 5000, 0,  255, 255, 0\n";
    }
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        app::EqHost host;
        host.in_game = host.ui_ready = true;
        host.game = world.game.get();
        host.conn = world.conn.get();
        host.zone_short_name = "qeynos2";
        host.global_maps_dir = scratch.path() / "maps"; // read by the map state (the server's comes with the ini)
        host.feet_valid = true;
        app::EqApi api(host);
        api.begin_frame();
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "eq");
        constexpr ImU32 kNear = IM_COL32(0, 255, 0, 0), kFar = IM_COL32(255, 255, 0, 0);
        CHECK(marked_vertices(L, "eq.DrawMinimap(300, 250, 100, 50)", kNear) > 0);
        CHECK(marked_vertices(L, "eq.DrawMinimap(300, 250, 100, 50, true)", kNear) > 0); // turned with the heading
        CHECK(marked_vertices(L, "eq.DrawMinimap(300, 250, 100, 50)", kFar) == 0);
        CHECK(marked_vertices(L, "eq.DrawZoneMap(0, 0, 600, 500, 1)", kFar) > 0); // the whole zone: both
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}
