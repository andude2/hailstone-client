#pragma once

// The modern sky's volumetric clouds (docs/graphics.md "Volumetric clouds"): the maths render/cloud_renderer.cpp runs
// them with, kept free of Vulkan so it can be tested. Hailstone's own look: the original draws painted cloud layers
// and patches (render/sky_renderer.h), which these replace over the modern atmosphere.

#include <cstdint>

#include <glm/glm.hpp>

namespace hailstone::render {

// How much of the sky the clouds hold and how thick they are, for shaders/clouds.comp.
struct CloudCover {
    float coverage = 0.0f; // 0 clear .. 1 overcast: how much of the weather map grows cloud
    float density = 0.0f;  // the clouds' extinction scale (1 the fair-weather cumulus)
    float type = 0.0f;     // 0 flat (stratus) .. 1 towering (cumulus): added to the weather map's own
    float darkness = 0.0f; // 0..1: how much of the light the storm's thick base keeps from the clouds' undersides
};

// The cover over a sky (SkyRenderer::sky_index: 1 normal, 2 desert, 4 cottony) under a storm (render::WeatherLook::
// storm, 0..1): fair-weather cumulus over the normal sky, a few over the desert, many over the cottony one; a storm
// closes the sky over, thick, flat and dark at its base.
CloudCover cloud_cover(int sky_index, float storm);

// The clouds are marched at half the scene's size on each side (rounded up), and each frame marches one pixel of
// every 2x2 block, the others carried over from the last frames turned to this frame's view (cloud_update_slot).
inline constexpr std::uint32_t kCloudDownscale = 2;
glm::uvec2 cloud_extent(std::uint32_t scene_width, std::uint32_t scene_height);

// Which pixel of a 2x2 block frame `frame` marches (0 top left, 1 top right, 2 bottom left, 3 bottom right): in the
// order of a 2x2 Bayer matrix (0, 3, 1, 2), so the block fills in from opposite corners.
std::uint32_t cloud_update_slot(std::uint64_t frame);

// The history is thrown away when the sky has moved too far between two frames to carry it over: the sun by more
// than kCloudResetSunTurn (a jump of the clock, a zone with another time), the storm by more than
// kCloudResetStorm, or the wind's offset by more than kCloudResetWind kilometres.
inline constexpr float kCloudResetSunTurn = 0.02f; // radians
inline constexpr float kCloudResetStorm = 0.1f;
inline constexpr float kCloudResetWind = 0.5f;
struct CloudHistoryKey {
    glm::vec3 to_light{0.0f, 0.0f, 1.0f};
    float storm = 0.0f;
    glm::vec2 wind{0.0f};
};
bool cloud_history_stale(const CloudHistoryKey& last, const CloudHistoryKey& now);

// How far the wind moves the cloud field in `dt_seconds` (kilometres, x north and y west as the world's axes): a
// steady drift from the west-south-west, twice as fast in a full storm. The caller adds the steps up, so a storm
// coming on speeds the clouds up without jumping them.
inline constexpr float kCloudWindSpeed = 0.012f; // km a second, fair weather (about 43 km/h at the clouds)
glm::vec2 cloud_wind_step(float dt_seconds, float storm);

} // namespace hailstone::render
