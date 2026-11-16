#include "render/clouds.h"

#include <algorithm>
#include <cmath>

namespace hailstone::render {

CloudCover cloud_cover(int sky_index, float storm) {
    storm = std::clamp(storm, 0.0f, 1.0f);
    CloudCover fair;
    switch (sky_index) {
    case 2: fair = {0.18f, 0.8f, 0.3f, 0.0f}; break; // desert: a few small cumulus
    case 4: fair = {0.55f, 1.0f, 0.6f, 0.0f}; break; // cottony: heaped cumulus
    default: fair = {0.36f, 1.0f, 0.5f, 0.0f}; break;
    }
    // A full storm: the whole sky, thicker, flatter (nimbostratus), its base dark.
    const CloudCover overcast{1.0f, 2.2f, 0.15f, 0.75f};
    const auto mix = [storm](float a, float b) { return a + (b - a) * storm; };
    return {mix(fair.coverage, overcast.coverage), mix(fair.density, overcast.density), mix(fair.type, overcast.type),
            mix(fair.darkness, overcast.darkness)};
}

glm::uvec2 cloud_extent(std::uint32_t scene_width, std::uint32_t scene_height) {
    const auto half = [](std::uint32_t v) { return std::max<std::uint32_t>(1u, (v + kCloudDownscale - 1) / kCloudDownscale); };
    return {half(scene_width), half(scene_height)};
}

std::uint32_t cloud_update_slot(std::uint64_t frame) {
    constexpr std::uint32_t kOrder[4] = {0, 3, 1, 2};
    return kOrder[frame % 4];
}

bool cloud_history_stale(const CloudHistoryKey& last, const CloudHistoryKey& now) {
    const float cos_turn = glm::dot(glm::normalize(last.to_light), glm::normalize(now.to_light));
    if (!(cos_turn >= std::cos(kCloudResetSunTurn))) return true; // NaN counts as a jump
    if (!(std::abs(now.storm - last.storm) <= kCloudResetStorm)) return true;
    const glm::vec2 moved = now.wind - last.wind;
    return !(glm::dot(moved, moved) <= kCloudResetWind * kCloudResetWind);
}

glm::vec2 cloud_wind_step(float dt_seconds, float storm) {
    if (!(dt_seconds > 0.0f)) return glm::vec2(0.0f);
    // From the west-south-west: towards the east (-Y) and a little north (+X); game/coords.h.
    const glm::vec2 towards = glm::normalize(glm::vec2(0.35f, -1.0f));
    const float speed = kCloudWindSpeed * (1.0f + std::clamp(storm, 0.0f, 1.0f));
    return towards * (speed * std::min(dt_seconds, 1.0f));
}

} // namespace hailstone::render
