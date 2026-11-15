#ifndef HAILSTONE_ZONE_SURFACE_GLSL
#define HAILSTONE_ZONE_SURFACE_GLSL

// EQG CBGG1/CBSG1 surface channels: RG is the tangent direction, B self-illumination, A gloss. Not confirmed
// against the original's shader (SPL/SModelCBSG1 is the author's reading; docs/unknowns.md "EQG"). The tangent Z of
// 0.9 is the client's own choice: it keeps the glow channel out of the normal, including a dark rock's B=0.
// Also compiled by the CPU regression tests with GLM's float operations (tests/lighting_test.cpp).
vec3 zone_surface_normal(vec4 texel, bool packed_channels) {
    return packed_channels ? vec3(texel.x * 2.0f - 1.0f, texel.y * 2.0f - 1.0f, 0.9f) : vec3(texel) * 2.0f - 1.0f;
}

// The highlight's strength: the material's e_fGloss0 (`fallback`), or for a packed map its alpha in place of it.
// Whether the original replaces e_fGloss0 by the map's alpha or scales it is not confirmed (docs/unknowns.md "EQG").
float zone_surface_gloss(vec4 texel, bool packed_channels, float fallback) {
    return packed_channels ? clamp(texel.w, 0.0f, 1.0f) : fallback;
}

// Modern light sources live above display white, as additive particles do: the gain is render::kParticleGlow's
// (render/particle_renderer.h; tests/lighting_test.cpp keeps them equal). Classic keeps the authored intensity.
float zone_emission_gain(bool modern) { return modern ? 2.5f : 1.0f; }

// `albedo` is linear in modern lighting, display RGB in classic. Emission is independent of baked light, sky share
// and shadow visibility; a missing/black glow mask adds nothing. The classic intensity 1 is not confirmed.
vec3 zone_surface_glow(vec3 albedo, float mask, bool modern) {
    return albedo * (clamp(mask, 0.0f, 1.0f) * zone_emission_gain(modern));
}

#endif
