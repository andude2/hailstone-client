#pragma once
// The UI bridge's names up to 0.7: app/lua_ui.h's UiHost and UiApi were EqHost and EqApi, and this was their header.
// Kept so code outside this repository that builds against the client's sources (the compiled server plugins'
// tests, built beside it from plugins/<name>/) compiles unchanged. New code includes app/lua_ui.h.
#include "app/lua_ui.h"

namespace hailstone::app {
using EqHost = UiHost;
using EqApi = UiApi;
} // namespace hailstone::app
