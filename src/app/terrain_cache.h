#pragma once

// Where an outdoor zone's land's worn terrain is cached (render::ZoneRenderOptions::terrain_cache, docs/layout.md).

#include <filesystem>
#include <string>
#include <string_view>

#include "platform/paths.h"

namespace hailstone::app {

// cache/terrain/<server>/<zone>.bin; "default" for no server.
inline std::filesystem::path terrain_cache_path(std::string_view server, std::string_view zone) {
    return platform::cache_dir() / "terrain" / std::string(server.empty() ? std::string_view("default") : server) /
           (std::string(zone) + ".bin");
}

} // namespace hailstone::app
