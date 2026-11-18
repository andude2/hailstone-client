#include "render/point_shadows.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "assets/model.h"
#include "render/scene_lighting.h"

namespace hailstone::render {

namespace {

// How much a light lights round the camera: its brightness and reach over its distance (to its sphere).
template <class Light> float light_score(const Light& l, const glm::vec3& camera) {
    float luma = glm::dot(l.color, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    float gap = std::max(glm::length(l.position - camera) - l.radius, 0.0f);
    return luma * l.radius / (gap + 40.0f);
}

// 1 near, 0 at kPointShadowRange (to the light's sphere).
float range_fade(const glm::vec3& position, float radius, const glm::vec3& camera) {
    float gap = std::max(glm::length(position - camera) - radius, 0.0f);
    float x = std::clamp((gap - 0.75f * kPointShadowRange) / (0.25f * kPointShadowRange), 0.0f, 1.0f);
    return 1.0f - x * x * (3.0f - 2.0f * x);
}

// Whether a light can have a shadow from `camera`: large enough that its cube holds more than the near cut, and in
// range.
bool shadowable(const glm::vec3& position, float radius, const glm::vec3& camera) {
    return radius > kPointShadowNear * 2.0f && glm::length(position - camera) - radius <= kPointShadowRange;
}

} // namespace

PointShadowTexel point_shadow_face(const glm::vec3& d) {
    glm::vec3 a = glm::abs(d);
    PointShadowTexel out;
    if (a.x >= a.y && a.x >= a.z) {
        out.face = d.x >= 0.0f ? 0u : 1u;
        out.ndc = glm::vec2(d.y, d.z) / std::max(a.x, 1e-6f);
    } else if (a.y >= a.z) {
        out.face = d.y >= 0.0f ? 2u : 3u;
        out.ndc = glm::vec2(d.x, d.z) / std::max(a.y, 1e-6f);
    } else {
        out.face = d.z >= 0.0f ? 4u : 5u;
        out.ndc = glm::vec2(d.x, d.y) / std::max(a.z, 1e-6f);
    }
    out.ndc *= kPointShadowFaceScale;
    return out;
}

bool box_reaches_face(const glm::vec3& light, std::uint32_t face, const glm::vec3& box_min, const glm::vec3& box_max) {
    int axis = static_cast<int>(face / 2);
    return face % 2 == 0 ? box_max[axis] > light[axis] : box_min[axis] < light[axis];
}

DynamicShadowSlots dynamic_shadow_slots(std::uint32_t slots) {
    DynamicShadowSlots out;
    out.carried = slots >= 3 ? 1u : 0u;
    out.spells = slots >= 7 ? 2u : slots >= 5 ? 1u : 0u;
    return out;
}

void PointShadows::resize(std::uint32_t slots) {
    total_ = std::min(slots, kMaxPointShadows);
    DynamicShadowSlots dynamic = dynamic_shadow_slots(total_);
    carried_slots_ = dynamic.carried;
    slots_.assign(total_ - dynamic.carried - dynamic.spells, Slot{});
    dynamic_.assign(dynamic.carried + dynamic.spells, DynamicSlot{});
}

void PointShadows::reset() {
    for (Slot& s : slots_) s = Slot{};
    for (DynamicSlot& s : dynamic_) s = DynamicSlot{};
}

void PointShadows::set_dynamic(const DynamicShadowLights& lights, const glm::vec3& camera) {
    for (DynamicSlot& s : dynamic_) s = DynamicSlot{};
    auto take = [&](DynamicSlot& s, const game::LightSource& l) {
        s.used = true;
        s.position = l.position;
        s.radius = l.radius;
        s.strength = range_fade(l.position, l.radius, camera);
    };
    // The carried light: its carrier's body is all round it, and would black out the world.
    if (carried_slots_ > 0 && lights.carried && shadowable(lights.carried->position, lights.carried->radius, camera)) {
        take(dynamic_[0], *lights.carried);
        dynamic_[0].skip_spawn = lights.carrier;
    }
    // The spells' brightest. They have no identity from frame to frame (an effect's emitters merge and part), and need
    // none: each cube is drawn anew. Who cast them is not known here, so the characters the light lies within (the
    // caster's hands, the target's body) are left out instead.
    const std::size_t spell_slots = dynamic_.size() - carried_slots_;
    if (spell_slots == 0) return;
    std::vector<const game::LightSource*> spells;
    for (const auto& l : lights.spells)
        if (shadowable(l.position, l.radius, camera)) spells.push_back(&l);
    std::size_t keep = std::min(spells.size(), spell_slots);
    std::partial_sort(spells.begin(), spells.begin() + static_cast<std::ptrdiff_t>(keep), spells.end(),
                      [&](const game::LightSource* a, const game::LightSource* b) {
                          return light_score(*a, camera) > light_score(*b, camera);
                      });
    for (std::size_t i = 0; i < keep; ++i) {
        DynamicSlot& s = dynamic_[carried_slots_ + i];
        take(s, *spells[i]);
        s.skip_around = true;
    }
}

bool same_shadow_light(const glm::vec4& a, const glm::vec4& b) {
    const glm::vec3 d = glm::vec3(a) - glm::vec3(b);
    return glm::dot(d, d) <= 0.01f && std::abs(a.w - b.w) <= 0.01f;
}

float point_shadow_key(std::span<const assets::PointLight> lights, std::size_t light) {
    const glm::vec4 l(lights[light].position, lights[light].radius);
    for (std::size_t j = 0; j < light; ++j)
        if (same_shadow_light(glm::vec4(lights[j].position, lights[j].radius), l)) return static_cast<float>(j + 1);
    return static_cast<float>(light + 1);
}

std::vector<float> point_shadow_keys(std::span<const assets::PointLight> lights) {
    // point_shadow_key for each, without comparing every pair: lights the same to the shadows lie within a tenth of
    // a unit in x, so only the lights near in x (sorted) are compared.
    std::vector<float> keys(lights.size());
    std::vector<std::size_t> order(lights.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(),
              [&](std::size_t a, std::size_t b) { return lights[a].position.x < lights[b].position.x; });
    constexpr float kReach = 0.11f; // a little past the tenth, for the rounding of the squared distance
    for (std::size_t k = 0; k < order.size(); ++k) {
        const std::size_t i = order[k];
        const glm::vec4 l(lights[i].position, lights[i].radius);
        std::size_t first = i;
        for (std::size_t m = k; m-- > 0 && lights[order[m]].position.x >= l.x - kReach;)
            if (order[m] < first && same_shadow_light(glm::vec4(lights[order[m]].position, lights[order[m]].radius), l))
                first = order[m];
        for (std::size_t m = k + 1; m < order.size() && lights[order[m]].position.x <= l.x + kReach; ++m)
            if (order[m] < first && same_shadow_light(glm::vec4(lights[order[m]].position, lights[order[m]].radius), l))
                first = order[m];
        keys[i] = static_cast<float>(first + 1);
    }
    return keys;
}

std::vector<std::uint32_t> PointShadows::update(std::span<const assets::PointLight> lights, const glm::vec3& camera,
                                                float dt) {
    std::vector<std::uint32_t> builds;
    if (slots_.empty()) return builds;
    // The candidates, best first; a light that has a slot counts half again, so two close ones do not trade places
    // every frame.
    struct Candidate { int light; float score; };
    std::vector<Candidate> candidates;
    for (std::size_t i = 0; i < lights.size(); ++i) {
        const auto& l = lights[i];
        if (!(l.radius > kPointShadowNear * 2.0f)) continue;
        if (glm::length(l.position - camera) - l.radius > kPointShadowRange) continue;
        float score = light_score(l, camera);
        if (std::any_of(slots_.begin(), slots_.end(), [&](const Slot& s) { return s.light == static_cast<int>(i); }))
            score *= 1.5f;
        candidates.push_back({static_cast<int>(i), score});
    }
    std::size_t keep = std::min(candidates.size(), slots_.size());
    std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(keep), candidates.end(),
                      [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    candidates.resize(keep);
    // Slots whose light is no longer chosen are freed; the chosen lights without a slot take the free ones.
    for (Slot& s : slots_)
        if (s.light >= 0 && std::none_of(candidates.begin(), candidates.end(),
                                         [&](const Candidate& c) { return c.light == s.light; }))
            s = Slot{};
    for (const Candidate& c : candidates) {
        if (std::any_of(slots_.begin(), slots_.end(), [&](const Slot& s) { return s.light == c.light; })) continue;
        for (Slot& s : slots_)
            if (s.light < 0) {
                s = Slot{};
                s.light = c.light;
                break;
            }
    }
    // The cubes to draw: the unbuilt slots, nearest light first.
    for (std::uint32_t i = 0; i < slots_.size(); ++i)
        if (slots_[i].light >= 0 && !slots_[i].built) builds.push_back(i);
    std::sort(builds.begin(), builds.end(), [&](std::uint32_t a, std::uint32_t b) {
        return glm::length(lights[static_cast<std::size_t>(slots_[a].light)].position - camera) <
               glm::length(lights[static_cast<std::size_t>(slots_[b].light)].position - camera);
    });
    if (builds.size() > kPointShadowBuildsPerFrame) builds.resize(kPointShadowBuildsPerFrame);
    // A built shadow fades in over a quarter second, so a cube arriving does not pop.
    for (Slot& s : slots_)
        s.strength = s.built ? std::min(s.strength + dt * 4.0f, 1.0f) : 0.0f;
    return builds;
}

void PointShadows::apply(SceneLighting& lighting, std::span<const assets::PointLight> lights, const glm::vec3& camera,
                         std::uint32_t size) const {
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < slots_.size(); ++i) {
        const Slot& s = slots_[i];
        if (s.light < 0 || !s.built || static_cast<std::size_t>(s.light) >= lights.size()) continue;
        const auto& l = lights[static_cast<std::size_t>(s.light)];
        float strength = s.strength * range_fade(l.position, l.radius, camera);
        if (strength <= 0.0f) continue;
        lighting.point_shadow_light[n] = glm::vec4(l.position, l.radius);
        lighting.point_shadow_slot[n] = glm::vec4(static_cast<float>(cube_of(i) * kPointShadowFaces), strength,
                                                  point_shadow_key(lights, static_cast<std::size_t>(s.light)), 0.0f);
        ++n;
    }
    for (std::uint32_t k = 0; k < dynamic_.size(); ++k) {
        const DynamicSlot& s = dynamic_[k];
        if (!s.used || s.strength <= 0.0f) continue;
        lighting.point_shadow_light[n] = glm::vec4(s.position, s.radius);
        lighting.point_shadow_slot[n] = glm::vec4(static_cast<float>(dynamic_cube(k) * kPointShadowFaces), s.strength,
                                                  0.0f, 0.0f);
        ++n;
    }
    lighting.point_shadow_params = glm::vec4(static_cast<float>(n), size ? 1.0f / static_cast<float>(size) : 0.0f,
                                             kPointShadowFaceScale, kPointShadowNear);
    tag_shadowed_lights(lighting);
}

} // namespace hailstone::render
