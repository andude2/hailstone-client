#version 450
#extension GL_GOOGLE_include_directive : require

#include "lighting.glsl"

// Zone geometry into one layer of the shadow map (render/zone_renderer.cpp draw_shadow), the layer in tex2.w: a cascade,
// its matrix from the Scene block, or the cover map (render/cover_map.h), looking straight down. The plants sway and the
// animated objects' parts move as they do in the scene (zone.vert).

layout(set = 1, binding = 0) uniform Scene {
    mat4 view_proj;
    vec4 fog_color;
    vec4 fog;
    vec4 camera_pos;
    SCENE_LIGHTING_FIELDS
} scene;

// The animated objects' motions (render/zone_renderer.cpp animate_instances): motion m's rows now at m * 6, a frame ago
// at m * 6 + 3. An animated object's instance names its first motion in normal_row[1].w, and each vertex its node in
// point_light.x (1 for the first motion, 0 for a vertex that stays still); the vertex goes through that motion before
// its placement.
int vertex_motion(float first, float node) {
    const int m = int(first + 0.5), n = int(node + 0.5);
    return m > 0 && n > 0 ? m + n - 1 : 0;
}
layout(set = 1, binding = 4) readonly buffer Motions {
    vec4 motion_rows[];
};

vec3 motion_point(vec3 p, int motion, int then) {
    if (motion <= 0) return p;
    const int at = motion * 6 + then * 3;
    return instance_point(p, motion_rows[at], motion_rows[at + 1], motion_rows[at + 2]);
}
vec3 motion_normal(vec3 n, int motion) {
    if (motion <= 0) return n;
    const int at = motion * 6;
    return instance_normal(n, motion_rows[at], motion_rows[at + 1], motion_rows[at + 2]);
}

layout(push_constant) uniform Push {
    uvec4 tex;
    uvec4 tex2; // slot 4, mode, flags, cascade
    vec4 params; // alpha_ref, ...
    vec4 slide;
    vec4 extra;
    vec4 extra2;
    vec4 extra3;
} pc;

layout(location = 0) in vec3 in_position;
layout(location = 2) in vec2 in_uv;
layout(location = 6) in vec3 in_point_light; // x: an animated part's vertex's node (zone.vert)
layout(location = 7) in vec4 in_sway;
layout(location = 8) in vec4 in_row0; // an instanced model's placement (zone.vert)
layout(location = 9) in vec4 in_row1;
layout(location = 10) in vec4 in_row2;
layout(location = 11) in vec4 in_normal_row0; // w: the sway's scale
layout(location = 12) in vec4 in_normal_row1; // w: an animated part's motion (zone.vert)

layout(location = 0) out vec2 v_uv;

void main() {
    v_uv = in_uv;
    vec3 position = instance_point(motion_point(in_position, vertex_motion(in_normal_row1.w, in_point_light.x), 0), in_row0, in_row1, in_row2);
    vec4 sway = instance_sway(in_sway, in_row0, in_row1, in_normal_row0.w);
    vec3 p = position + wind_offset(position, sway, scene.wind, scene.time.x);
    if (pc.tex2.w < 4u) {
        gl_Position = scene.shadow_view_proj[pc.tex2.w] * vec4(p, 1.0);
    } else {
        vec2 uv = (p.xy - scene.cover.xy) * scene.cover.z;
        gl_Position = vec4(uv * 2.0 - 1.0, (scene.cover.w - p.z) * scene.cover_params.x, 1.0);
    }
}
