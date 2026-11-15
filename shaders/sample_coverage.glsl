// An alpha as a sample mask (coverage.glsl alpha_coverage_samples): `alpha` of `count` samples, rounded with the
// pixel's noise and turned by it, so neighbouring pixels cover their partial alpha with unalike samples. The noise is
// lod_fade_samples' shifted and transposed, so a partial alpha does not favour one level of detail in a fade. Also
// compiled by the CPU regression tests with GLM's float operations (tests/depth_prepass_test.cpp).
#ifndef HAILSTONE_SAMPLE_COVERAGE_GLSL
#define HAILSTONE_SAMPLE_COVERAGE_GLSL

int coverage_sample_mask(float alpha, vec2 frag, int count) {
    float n = fract(52.9829189f * fract(dot(vec2(frag.y, frag.x) + vec2(17.0f, 53.0f), vec2(0.06711056f, 0.00583715f))));
    int keep = clamp(int(floor(clamp(alpha, 0.0f, 1.0f) * float(count) + n)), 0, count);
    int mask = (1 << keep) - 1;
    int turn = int(fract(n * 7.0f) * float(count)) % count;
    int all = (1 << count) - 1;
    return ((mask << turn) | (mask >> (count - turn))) & all;
}

#endif
