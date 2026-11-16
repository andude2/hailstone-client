// The offscreen colour is premultiplied by accumulated coverage. ImGui expects straight colour and applies alpha.
// Shared with the CPU regression so translucent surfaces and particles retain their authored opacity.
vec4 preview_straight_alpha(vec4 pixel) {
    if (pixel.a <= 0.0) return vec4(0.0);
    return vec4(vec3(pixel) / pixel.a, clamp(pixel.a, 0.0f, 1.0f));
}

// An additive sprite's black texels emit no light even when their stored alpha is opaque. Coverage follows the
// emitted colour in a transparent preview; straight colour compensates so SRC_ALPHA still writes all its light.
float preview_additive_alpha(vec3 emission) {
    return clamp(max(emission.r, max(emission.g, emission.b)), 0.0f, 1.0f);
}

vec4 preview_additive_particle(vec3 emission) {
    float alpha = preview_additive_alpha(emission);
    return alpha > 0.0f ? vec4(emission / alpha, alpha) : vec4(0.0f);
}
