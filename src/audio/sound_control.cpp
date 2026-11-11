#include "audio/sound_control.h"

#include <algorithm>
#include <cmath>

namespace hailstone::audio {

namespace {
// msssoft.m3d's constants: 0.0001 (0x2240c788), 1/pi (0x2240c774), the behind factor 0.75 (0x2240c770).
constexpr double kEpsilon = 9.999999747378752e-05; // the float 0.0001
constexpr double kInvPi = 0.31830987334251404;
constexpr double kBehind = 0.75;
constexpr double kRolloff = 1.0; // 0x2240e03c, M3D_set_3D_rolloff_factor's default
} // namespace

MilesLevels miles_fast2d_levels(const glm::vec3& offset, float min_distance, float max_distance, const glm::vec3& face,
                                const glm::vec3& right) {
    MilesLevels out;
    double dx = offset.x, dy = offset.y, dz = offset.z;
    double d = std::sqrt(dx * dx + dy * dy + dz * dz);
    // "Mute at maximum": past the max distance (an unordered compare counts as not past).
    if (d > static_cast<double>(max_distance)) {
        out.muted = true;
        return out;
    }
    double gain = 1.0;
    double min = std::max(static_cast<double>(min_distance), kEpsilon);
    if (!(min + kEpsilon >= d)) gain *= min / ((d - min) * kRolloff + min);
    double ux = 1.0, uy = 0.0, uz = 0.0;
    if (d != 0.0) {
        double inv = 1.0 / d;
        ux = dx * inv;
        uy = dy * inv;
        uz = dz * inv;
    }
    double front = face.x * ux + face.y * uy + face.z * uz;
    double pan = 0.5;
    if (d > kEpsilon) {
        double c = std::clamp(right.x * ux + right.y * uy + right.z * uz, -1.0, 1.0);
        pan = std::acos(c) * kInvPi;
    }
    if (front < 0.0) gain *= kBehind;
    out.left = static_cast<float>(pan * gain);
    out.right = static_cast<float>((1.0 - pan) * gain);
    return out;
}

float option_volume_factor(int percent, bool positional) {
    float level = static_cast<float>(std::clamp(percent, 0, 100)) / 100.0f;
    if (!positional || level <= 0.0f) return level;
    return static_cast<float>(std::pow(static_cast<double>(level), 3.0 / 5.0));
}

float miles_volume_curve(float volume) {
    if (volume <= 0.0f) return 0.0f;
    return static_cast<float>(std::pow(static_cast<double>(volume), 5.0 / 3.0));
}

} // namespace hailstone::audio
