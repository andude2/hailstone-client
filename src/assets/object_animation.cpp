#include "assets/object_animation.h"

#include <algorithm>
#include <cmath>

namespace hailstone::assets {

namespace {

glm::mat4 key_matrix(const glm::quat& rotation, const glm::vec3& translation, float scale) {
    glm::quat q = rotation;
    const float len = glm::length(q);
    q = std::isfinite(len) && len > 1e-6f ? q / len : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    return glm::translate(glm::mat4(1.0f), translation) * glm::mat4_cast(q) * glm::scale(glm::mat4(1.0f), glm::vec3(scale));
}

} // namespace

glm::mat4 sample_object_track(const ObjectAnimationNode& node, std::uint64_t ms, std::uint32_t period_ms) {
    if (node.keys.empty()) return glm::mat4(1.0f);
    const ObjectTrackKey& first = node.keys.front();
    if (node.keys.size() == 1 || node.interval_ms == 0 || period_ms == 0)
        return key_matrix(first.rotation, first.translation, first.scale);
    // keys.back() is the closing key; past it (a track shorter than the set's period) the track holds there, as
    // D3DX's sampler holds a clamped endpoint.
    const std::uint64_t t = ms % period_ms;
    const std::uint64_t authored = node.keys.size() - 1;
    const std::uint64_t i = t / node.interval_ms;
    if (i >= authored) {
        const ObjectTrackKey& last = node.keys.back();
        return key_matrix(last.rotation, last.translation, last.scale);
    }
    const ObjectTrackKey& a = node.keys[static_cast<std::size_t>(i)];
    const ObjectTrackKey& b = node.keys[static_cast<std::size_t>(i + 1)];
    const float u = static_cast<float>(t - i * node.interval_ms) / static_cast<float>(node.interval_ms);
    // The components blended, then normalised (key_matrix): D3DX's quaternion sampler, not a spherical blend.
    const glm::quat q(a.rotation.w + (b.rotation.w - a.rotation.w) * u, a.rotation.x + (b.rotation.x - a.rotation.x) * u,
                      a.rotation.y + (b.rotation.y - a.rotation.y) * u, a.rotation.z + (b.rotation.z - a.rotation.z) * u);
    return key_matrix(q, a.translation + (b.translation - a.translation) * u, a.scale + (b.scale - a.scale) * u);
}

void ObjectAnimation::pose(std::uint64_t ms, std::vector<glm::mat4>& out) const {
    const std::size_t n = nodes.size();
    out.assign(n, glm::mat4(1.0f));
    // World matrices, parents first (the loader checked the hierarchy has no cycle).
    std::vector<glm::mat4> world(n);
    std::vector<std::uint8_t> done(n, 0);
    std::vector<std::size_t> stack;
    for (std::size_t i = 0; i < n; ++i) {
        if (done[i]) continue;
        stack.push_back(i);
        while (!stack.empty()) {
            const std::size_t k = stack.back();
            const std::int32_t p = nodes[k].parent;
            if (p >= 0 && static_cast<std::size_t>(p) < n && !done[static_cast<std::size_t>(p)]) {
                stack.push_back(static_cast<std::size_t>(p));
                continue;
            }
            const glm::mat4 local = sample_object_track(nodes[k], ms, period_ms);
            world[k] = p >= 0 && static_cast<std::size_t>(p) < n ? world[static_cast<std::size_t>(p)] * local : local;
            done[k] = 1;
            stack.pop_back();
        }
    }
    for (std::size_t i = 0; i < n && i < rest_inverse.size(); ++i)
        if (nodes[i].moves) out[i] = world[i] * rest_inverse[i];
}

std::uint16_t moving_triangle_node(const std::vector<std::uint16_t>& vertex_node, std::uint32_t a, std::uint32_t b,
                                   std::uint32_t c) {
    for (std::uint32_t v : {a, b, c})
        if (v < vertex_node.size() && vertex_node[v] != kStillVertex) return vertex_node[v];
    return kStillVertex;
}

bool ObjectAnimation::any_moving_vertex() const {
    return std::any_of(vertex_node.begin(), vertex_node.end(), [](std::uint16_t v) { return v != kStillVertex; });
}

} // namespace hailstone::assets
