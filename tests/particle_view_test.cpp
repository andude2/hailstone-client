// render/particle_renderer.h: which particles the renderer lights and builds (particle_in_view), and the modern light
// on them with the frame's part worked out once (ParticleLight).
#include <doctest/doctest.h>

#include "render/camera.h"
#include "render/culling.h"
#include "render/particle_renderer.h"
#include "render/scene_lighting.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace hailstone::render;

namespace preview_alpha_test {
using glm::vec3;
using glm::vec4;
using glm::clamp;
using glm::max;
#include "../shaders/preview_alpha.glsl"
}

TEST_CASE("Preview resolve preserves particle coverage when ImGui applies straight alpha") {
    using preview_alpha_test::preview_straight_alpha;
    // A half-transparent spark outside the item's opaque mesh, already blended into transparent black.
    const glm::vec4 source(0.4f, 0.2f, 0.1f, 0.5f);
    const glm::vec4 straight = preview_straight_alpha(source);
    CHECK(straight == glm::vec4(0.8f, 0.4f, 0.2f, 0.5f));
    CHECK(glm::vec3(straight) * straight.a == glm::vec3(source));
    const glm::vec4 layered(0.6f, 0.3f, 0.15f, 0.75f);
    const glm::vec4 resolved = preview_straight_alpha(layered);
    CHECK(resolved.a == 0.75f);
    CHECK(glm::length(glm::vec3(resolved) * resolved.a - glm::vec3(layered)) < 1e-6f);
    CHECK(preview_straight_alpha(glm::vec4(0.2f, 0.4f, 0.6f, 1.0f)) == glm::vec4(0.2f, 0.4f, 0.6f, 1.0f));
    CHECK(preview_straight_alpha(glm::vec4(0.0f)) == glm::vec4(0.0f));
    CHECK(preview_straight_alpha(glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)) == glm::vec4(0.0f));
    // HDR emission stays linear until the sRGB attachment encodes it.
    CHECK(preview_straight_alpha(glm::vec4(1.0f, 0.5f, 0.0f, 0.5f)).r == 2.0f);
    // Additive black sprite texels retain the checkerboard, while coloured texels keep their emitted RGB.
    CHECK(preview_alpha_test::preview_additive_particle(glm::vec3(0.0f)) == glm::vec4(0.0f));
    const glm::vec3 emission(0.125f, 0.03125f, 0.0f);
    const auto additive = preview_alpha_test::preview_additive_particle(emission);
    CHECK(additive.a == 0.125f);
    CHECK(glm::vec3(additive) * additive.a == emission);
}

namespace {

// particle_light as it was worked out per particle before ParticleLight (render/particle_renderer.cpp's to_linear and
// light_pool, the shader's).
glm::vec3 linear(const glm::vec3& c) {
    auto one = [](float v) {
        v = std::max(v, 0.0f);
        return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    };
    return {one(c.r), one(c.g), one(c.b)};
}
float pool(float d2, float r) {
    float window = 1.0f - std::min(d2 / (r * r), 1.0f);
    float core = std::clamp(r * 0.2f, 8.0f, 16.0f);
    return window * core * core / (core * core + d2);
}
glm::vec3 per_particle_light(const SceneLighting& l, const glm::vec3& p) {
    glm::vec3 light = glm::mix(glm::vec3(l.ground), glm::vec3(l.sky), 0.65f) + glm::vec3(l.light) * 0.3f +
                      linear(glm::vec3(l.special_ambient));
    const int count = std::min(static_cast<int>(l.params.y + 0.5f), static_cast<int>(kMaxSceneLights));
    for (int i = 0; i < count; ++i) {
        const GpuPointLight& pl = l.lights[static_cast<std::size_t>(i)];
        float r = pl.position_radius.w;
        if (r <= 0.0f) continue;
        glm::vec3 d = glm::vec3(pl.position_radius) - p;
        float d2 = glm::dot(d, d);
        if (d2 >= r * r) continue;
        light += linear(glm::vec3(pl.color)) * (2.5f * 0.5f * pool(d2, r));
    }
    return glm::max(light, glm::vec3(l.params.w));
}

} // namespace

TEST_CASE("particles: only those that can show in the view are drawn") {
    FlyCamera cam;
    cam.position = {100.0f, 200.0f, 30.0f}; // yaw 0: looking along +X
    Frustum f = frustum_from(cam.projection(1280, 720, 2000.0f) * cam.view());
    CHECK(particle_in_view(f, {300, 200, 30}, 2, 2, 1));        // ahead
    CHECK_FALSE(particle_in_view(f, {-100, 200, 30}, 2, 2, 1)); // behind
    CHECK_FALSE(particle_in_view(f, {300, 900, 30}, 2, 2, 1));  // far to the side
    CHECK_FALSE(particle_in_view(f, {2300, 200, 30}, 2, 2, 1)); // past the far plane
    // Behind the camera, but so large that its quad reaches into the view.
    CHECK(particle_in_view(f, {80, 200, 30}, 60, 60, 1));
    // The size factor (grow / shrink ramps) counts; a negative one too.
    CHECK_FALSE(particle_in_view(f, {80, 200, 30}, 2, 2, 1));
    CHECK(particle_in_view(f, {80, 200, 30}, 2, 2, 30));
    CHECK(particle_in_view(f, {80, 200, 30}, 2, 2, -30));
}

TEST_CASE("particles: the frame's light is the light each particle was given") {
    SceneLighting l{};
    l.ground = glm::vec4(0.1f, 0.1f, 0.1f, 0.0f);
    l.sky = glm::vec4(0.2f, 0.3f, 0.4f, 0.0f);
    l.light = glm::vec4(0.5f, 0.5f, 0.5f, 0.0f);
    l.special_ambient = glm::vec4(0.5f, 0.0f, 0.0f, 0.0f);
    l.params = glm::vec4(1.0f, 3.0f, 0.0f, 0.05f);
    l.lights[0].position_radius = glm::vec4(0.0f, 0.0f, 0.0f, 50.0f);
    l.lights[0].color = glm::vec4(1.0f, 0.5f, 0.25f, 0.0f);
    l.lights[1].position_radius = glm::vec4(10.0f, 0.0f, 0.0f, 0.0f); // no reach: left out
    l.lights[1].color = glm::vec4(1.0f);
    l.lights[2].position_radius = glm::vec4(100.0f, 0.0f, 0.0f, 20.0f);
    l.lights[2].color = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
    l.lights[3].position_radius = glm::vec4(0.0f, 0.0f, 0.0f, 500.0f); // past the count: left out
    l.lights[3].color = glm::vec4(1.0f);
    const ParticleLight frame(l);
    for (glm::vec3 p : {glm::vec3(0, 0, 0), glm::vec3(5, 5, 0), glm::vec3(10, 0, 0), glm::vec3(95, 0, 0),
                        glm::vec3(1000, 0, 0)}) {
        glm::vec3 a = frame.at(p), b = per_particle_light(l, p);
        CHECK(a.r == b.r);
        CHECK(a.g == b.g);
        CHECK(a.b == b.b);
    }
    // Out of every light's reach: the ambient alone; in the first's, more of its red than its green.
    glm::vec3 far = frame.at({1000, 0, 0});
    glm::vec3 near = frame.at({5, 0, 0});
    CHECK(near.r > far.r);
    CHECK(near.r - far.r > near.g - far.g);
    CHECK(frame.at({95, 0, 0}).g > far.g); // the third light's green
    // The floor holds in the dark.
    SceneLighting dark{};
    dark.params = glm::vec4(1.0f, 0.0f, 0.0f, 0.05f);
    CHECK(ParticleLight(dark).at({0, 0, 0}).r == doctest::Approx(0.05f));
}

TEST_CASE("Modern particle stream: the nearest kept past the cap, back to front, as a full sort kept them") {
    struct Item {
        float dist2;
        int id;
    };
    std::vector<Item> items;
    std::uint32_t seed = 12345;
    for (int i = 0; i < 500; ++i) {
        seed = seed * 1664525u + 1013904223u;
        items.push_back({static_cast<float>(seed % 100000u), i});
    }
    // The old way: everything sorted far to near, the farthest dropped.
    std::vector<Item> full = items;
    std::sort(full.begin(), full.end(), [](const Item& a, const Item& b) { return a.dist2 > b.dist2; });
    full.erase(full.begin(), full.end() - 120);
    std::vector<Item> kept = items;
    keep_nearest_back_to_front(kept, 120);
    REQUIRE(kept.size() == 120);
    for (std::size_t i = 0; i < kept.size(); ++i) CHECK(kept[i].dist2 == full[i].dist2);
    CHECK(std::is_sorted(kept.begin(), kept.end(), [](const Item& a, const Item& b) { return a.dist2 > b.dist2; }));
    // Under the cap: all of them, sorted.
    std::vector<Item> few = {{1.0f, 0}, {9.0f, 1}, {4.0f, 2}};
    keep_nearest_back_to_front(few, 120);
    REQUIRE(few.size() == 3);
    CHECK(few[0].id == 1);
    CHECK(few[1].id == 2);
    CHECK(few[2].id == 0);
}
