#ifndef HAILSTONE_CLASSIC_LIGHT_GLSL
#define HAILSTONE_CLASSIC_LIGHT_GLSL

// The classic lighting's maths, shared by zone.vert, zone.frag and post.frag. Also compiled by the CPU tests with GLM's
// float operations (tests/lighting_test.cpp), so it keeps to what both languages read alike.

// The classic region light (RenderEffects/SPL/RegionC1 and RegionOldA, vs_1_1), in display values:
// rgb = C.rgb + a Ambient + SpecialAmbient + a (Bounce max(N.Dn, 0) + Directional max(N.-Dn, 0)), saturated, `a` the
// vertex alpha (1 for the detail passes, RegionOldDetailSingle / RegionOldDetailPalette). The original runs it per
// vertex; the smooth classic lighting (SceneLighting::ambient.w) runs it per pixel in zone.frag.
vec3 classic_region_light(vec3 c, float a, vec3 n, vec3 ambient, vec3 special, vec3 bounce, vec3 directional,
                          vec3 dn) {
    vec3 rgb = c + a * ambient + special +
               a * (bounce * max(dot(n, dn), 0.0f) + directional * max(dot(n, -dn), 0.0f));
    return clamp(rgb, 0.0f, 1.0f);
}

// The smooth classic lighting's tone curve (render::Tonemap::Soft), Hailstone's own: the original's frame buffer
// clips, so what is drawn past white (the flames of the cities' braziers and torches, additive glows over a lit
// surface, a wall the lights.wld term lifts past its saturated region light) turns flat white. Display values up to
// the knee stay as they are; above it they roll off towards white, display 1 landing at about 0.93 and 1.5 at 0.99.
// Worked in display values (a 2.2 power, close enough for a curve's shape), as the classic light is.
const float kSoftKnee = 0.8f;

// One channel, linear in and out: below the knee exactly as it came.
float soft_highlight(float c) {
    float d = pow(max(c, 0.0f), 1.0f / 2.2f);
    if (d <= kSoftKnee) return c;
    return pow(kSoftKnee + (1.0f - kSoftKnee) * (1.0f - exp((kSoftKnee - d) / (1.0f - kSoftKnee))), 2.2f);
}

vec3 soft_highlights(vec3 c) { return vec3(soft_highlight(c.x), soft_highlight(c.y), soft_highlight(c.z)); }

#endif
