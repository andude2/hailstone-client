#ifndef HAILSTONE_ZONE_SURFACE_GLSL
#define HAILSTONE_ZONE_SURFACE_GLSL

// MPL materials modulate the diffuse with a separately scaled coverage texture (SModel_Bump/Full).
vec3 surface_coverage_color(vec3 diffuse, vec3 coverage) {
    return clamp(2.0f * diffuse * coverage, 0.0f, 1.0f);
}

// SModelLava / SkinMeshLava: the scrolling secondary is lit by 1+bump; the primary alpha mixes in its lit colour.
vec3 surface_lava_color(vec3 primary_lit, vec3 secondary, float primary_alpha, float bump) {
    return mix(secondary * (1.0f + clamp(bump, 0.0f, 1.0f)), primary_lit, clamp(primary_alpha, 0.0f, 1.0f));
}

// EQG CBGG1/CBSG1/CBSGE1 channels: RG tangent normal with fixed Z=0.9, B added to ambient, A gloss in place of
// e_fGloss0 (checked in the player's SkinMeshCBSG1/CBSGE1 compiled shaders).
// Also compiled by the CPU regression tests with GLM's float operations (tests/lighting_test.cpp).
vec3 zone_surface_normal(vec4 texel, bool packed_channels) {
    return packed_channels ? vec3(texel.x * 2.0f - 1.0f, texel.y * 2.0f - 1.0f, 0.9f) : vec3(texel) * 2.0f - 1.0f;
}

// The highlight's strength: the material's e_fGloss0 (`fallback`), or for a packed map its alpha in place of it.
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
