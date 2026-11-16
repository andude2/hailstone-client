#version 450
#extension GL_GOOGLE_include_directive : require

#include "preview_alpha.glsl"

layout(set = 0, binding = 0) uniform sampler2D image;
layout(location = 0) out vec4 color;

void main() {
    // Same-size targets: keep the resolved coverage and colour together without another filtering step.
    color = preview_straight_alpha(texelFetch(image, ivec2(gl_FragCoord.xy), 0));
}
