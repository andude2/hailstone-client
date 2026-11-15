// plugin/lua_keep.h: C++ objects a lua_CFunction keeps across Lua calls that raise are freed with the state's
// collection, and a C++ exception becomes a Lua error.
#include <doctest/doctest.h>

#include <cstdlib>
#include <stdexcept>
#include <string>

#include <lua.hpp>

#include "plugin/lua_keep.h"

namespace {

int g_alive = 0;

struct Tracked {
    std::string text = std::string(1000, 'x'); // on the heap: a skipped destructor would leak it
    Tracked() { ++g_alive; }
    ~Tracked() { --g_alive; }
    Tracked(const Tracked&) = delete;
    Tracked& operator=(const Tracked&) = delete;
};

// A state refusing any allocation past `limit` bytes, as the scripts' and the UI's are held to theirs.
struct Limit {
    std::size_t used = 0, limit = 0;
};
void* limited(void* ud, void* ptr, std::size_t old_size, std::size_t new_size) {
    auto* l = static_cast<Limit*>(ud);
    const std::size_t old = ptr ? old_size : 0;
    if (new_size == 0) {
        std::free(ptr);
        l->used -= old;
        return nullptr;
    }
    if (new_size > old && l->used + (new_size - old) > l->limit) return nullptr;
    void* p = std::realloc(ptr, new_size);
    if (p) l->used = l->used - old + new_size;
    return p;
}

// Keeps a Tracked, then pushes a string too big for the state: the push raises past this frame.
int keep_then_fail(lua_State* L) {
    Tracked& t = hailstone::plugin::lua_keep<Tracked>(L);
    const std::string& big = t.text;
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 0; i < 100000; ++i) luaL_addlstring(&b, big.data(), big.size());
    luaL_pushresult(&b);
    return 1;
}

int throws(lua_State* L) {
    hailstone::plugin::cxx_guard(L, [] { throw std::runtime_error("boom"); });
    return 0;
}

int pushes_made(lua_State* L) {
    hailstone::plugin::push_made(L, std::string(100, 'y'));
    return 1;
}

} // namespace

TEST_CASE("lua_keep: what a raise leaves behind is freed when the state collects it") {
    Limit lim;
    lim.limit = 4u << 20;
    lua_State* L = lua_newstate(limited, &lim);
    REQUIRE(L);
    g_alive = 0;
    for (int i = 0; i < 5; ++i) {
        lua_pushcfunction(L, keep_then_fail);
        CHECK(lua_pcall(L, 0, 1, 0) == LUA_ERRMEM);
        lua_pop(L, 1);
    }
    CHECK(g_alive == 5); // not freed by the raise, but still the state's
    lua_gc(L, LUA_GCCOLLECT);
    CHECK(g_alive == 0);
    lua_pushcfunction(L, keep_then_fail);
    CHECK(lua_pcall(L, 0, 1, 0) == LUA_ERRMEM);
    lua_close(L);
    CHECK(g_alive == 0); // and at close
}

TEST_CASE("lua_keep: cxx_guard turns an exception into a Lua error; push_made pushes the text") {
    lua_State* L = luaL_newstate();
    lua_pushcfunction(L, throws);
    REQUIRE(lua_pcall(L, 0, 0, 0) == LUA_ERRRUN);
    CHECK(std::string(lua_tostring(L, -1)) == "boom");
    lua_pop(L, 1);
    lua_pushcfunction(L, pushes_made);
    REQUIRE(lua_pcall(L, 0, 1, 0) == LUA_OK);
    CHECK(std::string(lua_tostring(L, -1)) == std::string(100, 'y'));
    lua_close(L);
}
