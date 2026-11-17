#pragma once
// core/lua_keep.h under the names the scripts' and the app's Lua bindings use (plugin::lua_keep, cxx_guard,
// push_made). The helpers are in core so the UI's Lua host (ui/lua/) needs nothing from plugin/.
#include "core/lua_keep.h"

namespace hailstone::plugin {
using core::cxx_guard;
using core::lua_keep;
using core::push_made;
} // namespace hailstone::plugin
