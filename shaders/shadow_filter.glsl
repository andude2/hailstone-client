// The shadow maps' soft edge in four lookups (shaders/lighting.glsl shadow_visibility, point_shadow_at). Also
// compiled by the CPU regression tests with GLM's float operations (tests/lighting_test.cpp).
//
// The edge is the average of a 3 x 3 grid of hardware-filtered comparisons one texel apart. Per axis those nine
// bilinear lookups weigh the four texels under them 1 - f, 1, 1, f (f the point's place between texel centres), out
// of 3; two bilinear lookups give the same weights, one over the first pair (2 - f in all, placed so its own
// bilinear split is 1 - f to 1) and one over the second (1 + f, split 1 to f). So four lookups, each weighted by its
// two axes' sums and the total over 9, are the nine's average, the texels the same, up to the hardware's sub-texel
// rounding (after Castano, "Shadow Mapping Summary - Part 1", 2013).
#ifndef HAILSTONE_SHADOW_FILTER_GLSL
#define HAILSTONE_SHADOW_FILTER_GLSL

struct ShadowTaps {
    vec2 near_uv; // the lookup over the texels at -1 and 0 (each axis)
    vec2 far_uv;  // the lookup over the texels at +1 and +2
    vec2 near_w;  // each axis's weight of the near lookup, out of 3
    vec2 far_w;   // and of the far one
};

// The four lookups round `uv` on a map `inv_size` (1 / its texels a side) across.
ShadowTaps shadow_taps(vec2 uv, float inv_size) {
    vec2 s = uv / inv_size - 0.5f;
    vec2 base = floor(s);
    vec2 f = s - base;
    ShadowTaps t;
    t.near_w = 2.0f - f;
    t.far_w = 1.0f + f;
    // Texel centres sit at whole numbers plus a half in uv / inv_size.
    t.near_uv = (base - 0.5f + 1.0f / t.near_w) * inv_size;
    t.far_uv = (base + 1.5f + f / t.far_w) * inv_size;
    return t;
}

#endif
