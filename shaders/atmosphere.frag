#version 450

// The modern sky (render/sky_renderer.h SkyFrame::atmosphere): single scattering in an Earth-like atmosphere,
// Rayleigh and Mie, marched along the view ray with the sunlight marched in to each step (after Nishita et al. 1993;
// the constants are Bruneton and Neyret's). By night a dark sky and stars turning with the sun's path. Near and
// below the horizon it goes into the haze, as the classic layers do (shaders/sky.frag). The volumetric clouds
// (shaders/clouds.comp, marched before the scene) are blended over it; without them their image is clear.

layout(push_constant) uniform Push {
    mat4 inverse_view_proj; // clip to the sky's directions (view rotation only)
    vec4 to_sun;            // w: haze in the air (Mie scale)
    vec4 params;            // x: seconds, y: the stars' turn about the north-south axis (radians), z: storm, w: flash
    vec4 haze;              // the horizon haze: linear rgb, a: 0 off, else 1 + the northern lights (0..1)
    vec4 to_moon;           // render::moon_direction, w: the blood moon (0..1)
} pc;

// The volumetric clouds at half the scene's size: rgb their light, a how much of the sky behind shows (1 clear).
layout(set = 0, binding = 2) uniform sampler2D u_clouds;

layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 o_color;

const float kPi = 3.14159265;
const float kPlanet = 6360e3;
const float kTop = 6420e3;
const vec3 kRayleigh = vec3(5.8e-6, 13.5e-6, 33.1e-6);
// Ozone's absorption, carried with the Rayleigh density (render/atmosphere.cpp kOzone): without it single scattering
// leaves the horizon and the twilight sky green-yellow.
const vec3 kOzone = vec3(2.0e-6, 5.6e-6, 0.25e-6);
// The pole the sun, moon and stars turn about (render::sky_pole: 35 degrees over the northern horizon, +X).
const vec3 kPole = vec3(0.819152, 0.0, 0.573576);
const float kRayleighHeight = 8000.0;
const float kMie = 21e-6;
const float kMieHeight = 1200.0;
const float kMieG = 0.76;
// The sun's strength, scaled so the midday sky overhead is about the modern light's sky ambient
// (render/scene_lighting.cpp daylight).
const float kSun = 12.0;

// Distance along the ray (from inside the sphere) to where it leaves a sphere about the planet's centre.
float exit_sphere(vec3 o, vec3 d, float r) {
    float b = dot(o, d);
    float c = dot(o, o) - r * r;
    return -b + sqrt(max(b * b - c, 0.0));
}

// Whether the ray meets the planet in front of it.
bool hits_planet(vec3 o, vec3 d) {
    float b = dot(o, d);
    float c = dot(o, o) - kPlanet * kPlanet;
    return b < 0.0 && b * b - c > 0.0;
}

vec3 scatter(vec3 dir, vec3 sun, float mie_scale) {
    vec3 o = vec3(0.0, 0.0, kPlanet + 50.0);
    float len = exit_sphere(o, dir, kTop);
    const int kSteps = 16;
    const int kLightSteps = 6;
    float seg = len / float(kSteps);
    float mu = dot(dir, sun);
    float phase_r = 3.0 / (16.0 * kPi) * (1.0 + mu * mu);
    float g2 = kMieG * kMieG;
    float phase_m = 3.0 / (8.0 * kPi) * ((1.0 - g2) * (1.0 + mu * mu)) /
                    ((2.0 + g2) * pow(1.0 + g2 - 2.0 * kMieG * mu, 1.5));
    vec3 sum_r = vec3(0.0), sum_m = vec3(0.0);
    float depth_r = 0.0, depth_m = 0.0;
    for (int i = 0; i < kSteps; ++i) {
        vec3 p = o + dir * (seg * (float(i) + 0.5));
        float h = length(p) - kPlanet;
        float hr = exp(-h / kRayleighHeight) * seg;
        float hm = exp(-h / kMieHeight) * seg;
        depth_r += hr;
        depth_m += hm;
        if (hits_planet(p, sun)) continue; // in the planet's shadow
        float light_len = exit_sphere(p, sun, kTop);
        float lseg = light_len / float(kLightSteps);
        float light_r = 0.0, light_m = 0.0;
        for (int j = 0; j < kLightSteps; ++j) {
            vec3 q = p + sun * (lseg * (float(j) + 0.5));
            float hq = max(length(q) - kPlanet, 0.0);
            light_r += exp(-hq / kRayleighHeight) * lseg;
            light_m += exp(-hq / kMieHeight) * lseg;
        }
        vec3 tau = (kRayleigh + kOzone) * (depth_r + light_r) + kMie * mie_scale * 1.1 * (depth_m + light_m);
        vec3 att = exp(-tau);
        sum_r += att * hr;
        sum_m += att * hm;
    }
    return kSun * (sum_r * kRayleigh * phase_r + sum_m * kMie * mie_scale * phase_m);
}

float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

vec3 hash33(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yxx) * p.zyx);
}

// Stars: the sky's directions on the six faces of a cube, each face a grid with at most one star a cell (2 % of
// cells). A star's light falls off with its angle from the pixel's direction measured in pixels (`pixel`, the
// direction's change across a pixel, taken in uniform control flow), so each is a round point about a pixel wide at
// any resolution; most are faint; they twinkle.
vec3 stars(vec3 d, float pixel, float t) {
    vec3 a = abs(d);
    vec2 uv;
    float face;
    if (a.x >= a.y && a.x >= a.z) { uv = d.yz / a.x; face = d.x > 0.0 ? 0.0 : 1.0; }
    else if (a.y >= a.z) { uv = d.xz / a.y; face = d.y > 0.0 ? 2.0 : 3.0; }
    else { uv = d.xy / a.z; face = d.z > 0.0 ? 4.0 : 5.0; }
    vec2 g = uv * 160.0;
    vec2 cell = floor(g);
    float h = hash13(vec3(cell, face * 1000.0 + 17.0));
    if (h < 0.98) return vec3(0.0);
    vec2 suv = (cell + 0.15 + 0.7 * hash33(vec3(cell, face * 1000.0 + 5.0)).xy) / 160.0;
    // The star's direction, back from its face.
    vec3 s;
    if (face < 1.5) s = vec3(face < 0.5 ? 1.0 : -1.0, suv);
    else if (face < 3.5) s = vec3(suv.x, face < 2.5 ? 1.0 : -1.0, suv.y);
    else s = vec3(suv, face < 4.5 ? 1.0 : -1.0);
    s = normalize(s);
    float pixels = length(d - s) / max(pixel, 1e-6);
    float bright = pow((h - 0.98) / 0.02, 4.0);
    float twinkle = 0.7 + 0.3 * sin(t * (1.5 + h * 4.0) + h * 300.0);
    vec3 tint = mix(vec3(0.75, 0.85, 1.0), vec3(1.0, 0.88, 0.7), fract(h * 53.0));
    return tint * (0.05 + bright * 1.0) * twinkle * exp(-pixels * pixels * 1.4);
}

float noise2(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash13(vec3(i, 1.0)), hash13(vec3(i + vec2(1, 0), 1.0)), u.x),
               mix(hash13(vec3(i + vec2(0, 1), 1.0)), hash13(vec3(i + vec2(1, 1), 1.0)), u.x), u.y);
}

// The northern lights (this client's own, render::SkyFrame::aurora): curtains over the northern sky (+X, about which
// the stars turn), seen through a sheet of air marched in 16 layers. Each curtain runs east-west, folded by slow
// noise and drifting; its rays stripe it top to bottom. Green low, violet at the top; faint near the horizon.
vec3 aurora(vec3 dir, float t, float strength) {
    float north = smoothstep(-0.1, 0.45, dir.x) * smoothstep(0.02, 0.15, dir.z);
    if (strength <= 0.0 || north <= 0.0) return vec3(0.0);
    vec3 sum = vec3(0.0);
    // Each pixel starts its layers at its own offset (no banding between them).
    float jitter = hash13(vec3(gl_FragCoord.xy, 3.0));
    for (int i = 0; i < 16; ++i) {
        float f = min((float(i) + jitter) / 15.0, 1.0);
        // Where the ray crosses the layer at height 1 + 1.4 f (the curtains' foot at 1).
        vec2 p = dir.xy / dir.z * (1.0 + 1.4 * f);
        for (int c = 0; c < 2; ++c) {
            float fc = float(c);
            float fold = noise2(vec2(p.y * 0.3 + fc * 7.0, t * 0.015 + fc)) * 2.4 +
                         noise2(vec2(p.y * 1.3 - t * 0.04, fc * 3.0)) * 0.5;
            float d = p.x - (2.2 + fc * 1.6) - fold;
            // A sharp lower edge and a soft top: the curtain's brightest light is at its foot.
            float sheet = exp(-d * d * 18.0);
            float rays = 0.25 + 0.75 * pow(noise2(vec2(p.y * 9.0 + fold * 5.0, t * 0.25 + fc * 11.0)), 2.0);
            float pulse = 0.7 + 0.3 * noise2(vec2(p.y * 0.5 + fc * 5.0, t * 0.08));
            float foot = f < 0.07 ? 1.6 : 1.0 - 0.7 * f;
            sum += mix(vec3(0.15, 1.0, 0.45), vec3(0.55, 0.15, 0.75), f * f) * sheet * rays * pulse * foot;
        }
    }
    return sum * (0.06 * strength * north);
}

// Shooting stars: every 3.5 seconds a chance (55 %) of one, somewhere in the upper sky, streaking about 12 degrees
// over 0.7 s, a bright head and a fading tail a pixel or two wide.
vec3 shooting_stars(vec3 dir, float pixel, float t) {
    vec3 sum = vec3(0.0);
    const float kPeriod = 3.5;
    float slot = floor(t / kPeriod);
    for (int k = 0; k < 2; ++k) {
        float s = slot - float(k);
        vec3 h = hash33(vec3(s, 41.0, 7.0));
        if (h.x > 0.55) continue;
        float age = (t - s * kPeriod - h.y * 2.0) / 0.7;
        if (age < 0.0 || age > 1.0) continue;
        float az = h.z * 6.2831853, el = 0.35 + 0.8 * hash13(vec3(s, 3.0, 9.0));
        vec3 start = vec3(cos(az) * cos(el), sin(az) * cos(el), sin(el));
        float heading = hash13(vec3(s, 5.0, 2.0)) * 6.2831853;
        vec3 east = normalize(cross(vec3(0.0, 0.0, 1.0), start) + vec3(1e-4, 0.0, 0.0));
        vec3 north = cross(start, east);
        vec3 way = cos(heading) * east + sin(heading) * north - vec3(0.0, 0.0, 0.25);
        way = normalize(way - dot(way, start) * start);
        vec3 head = normalize(start + way * 0.21 * age);
        vec3 tail = normalize(start + way * 0.21 * max(age - 0.35, 0.0));
        // The pixel's distance from the streak, in pixels, and how far along it (0 tail .. 1 head).
        vec3 ab = head - tail;
        float along = clamp(dot(dir - tail, ab) / max(dot(ab, ab), 1e-8), 0.0, 1.0);
        float px = length(dir - (tail + ab * along)) / max(pixel, 1e-6);
        float fade = sin(age * 3.14159);
        sum += vec3(1.0, 0.95, 0.85) * exp(-px * px * 0.8) * along * along * fade * 1.5;
    }
    return sum;
}

// The sunlight left after the air between here and space along `sun` (0 below the horizon): the sun disc's colour.
vec3 sun_transmittance(vec3 sun, float mie_scale) {
    vec3 o = vec3(0.0, 0.0, kPlanet + 50.0);
    if (hits_planet(o, sun)) return vec3(0.0);
    float len = exit_sphere(o, sun, kTop);
    const int kSteps = 8;
    float seg = len / float(kSteps);
    float depth_r = 0.0, depth_m = 0.0;
    for (int i = 0; i < kSteps; ++i) {
        float h = length(o + sun * (seg * (float(i) + 0.5))) - kPlanet;
        depth_r += exp(-h / kRayleighHeight) * seg;
        depth_m += exp(-h / kMieHeight) * seg;
    }
    return exp(-((kRayleigh + kOzone) * depth_r + kMie * mie_scale * 1.1 * depth_m));
}

// A disc of angular radius `radius` round `centre`, antialiased over a pixel: 1 inside, 0 outside; `r` the
// distance from the centre in radii.
float disc(vec3 dir, vec3 centre, float radius, float pixel, out float r) {
    float angle = length(dir - centre);
    r = angle / radius;
    return 1.0 - smoothstep(radius - pixel, radius + pixel, angle);
}

// The sun: a disc larger than the real one (0.9 degrees across), darker towards its edge (limb darkening),
// as bright as the light that gets through the air, so it reddens at the horizon and blooms.
vec3 sun_disc(vec3 dir, vec3 sun, float pixel, vec3 transmit) {
    float r;
    float inside = disc(dir, sun, 0.008, pixel, r);
    if (inside <= 0.0) return vec3(0.0);
    float mu = sqrt(max(1.0 - r * r, 0.0));
    float limb = 1.0 - 0.6 * (1.0 - mu);
    return transmit * 600.0 * limb * inside;
}

// The moon (render::moon_direction): larger than the sun (1.6 degrees across), grey with darker seas (noise on its
// face), lit on the side towards the sun, so it waxes and wanes through the month, its dark side faint with
// earthshine; reddened by the air near the horizon like the sun, and a faint halo round it as bright as it is lit.
// `cover` is how much of the pixel it covers (the stars behind it are hidden). The blood moon (`blood`) is a deep
// red, its halo wider and red.
vec3 moon_disc(vec3 dir, vec3 moon, vec3 sun, float pixel, float mie_scale, float blood, out float cover) {
    float r;
    cover = disc(dir, moon, 0.014, pixel, r);
    float lit = 0.5 * (1.0 - dot(moon, sun));
    vec3 halo = mix(vec3(0.6, 0.7, 0.9) * exp(-length(dir - moon) * 60.0) * 0.02,
                    vec3(0.9, 0.2, 0.1) * exp(-length(dir - moon) * 35.0) * 0.05, blood) * lit * lit;
    if (cover <= 0.0) return halo;
    // The face's own frame: two axes across the disc, and the sphere's normal at this pixel (towards the eye).
    vec3 right = normalize(cross(moon, vec3(0.0, 0.0, 1.0)) + vec3(1e-4, 0.0, 0.0));
    vec3 up = cross(right, moon);
    vec2 face = vec2(dot(dir - moon, right), dot(dir - moon, up)) / 0.014;
    float mu = sqrt(max(1.0 - r * r, 0.0));
    vec3 normal = face.x * right + face.y * up - mu * moon;
    float day_side = smoothstep(-0.05, 0.12, dot(normal, sun));
    float seas = hash13(vec3(floor(face * 3.0), 7.0)) * 0.25 + hash13(vec3(floor(face * 7.0), 3.0)) * 0.15;
    float smooth_seas = smoothstep(0.15, 0.4, seas);
    vec3 surface = mix(vec3(0.85, 0.86, 0.9), vec3(0.85, 0.16, 0.07), blood) * (1.0 - 0.35 * smooth_seas) *
                   (0.75 + 0.25 * mu);
    vec3 air = mix(vec3(1.0), min(sun_transmittance(moon, mie_scale) * 1.15, vec3(1.0)), 0.7);
    return surface * 0.5 * (day_side + 0.012) * air * cover + halo;
}

void main() {
    vec4 far = pc.inverse_view_proj * vec4(v_ndc.x, v_ndc.y, 0.0, 1.0);
    vec4 near = pc.inverse_view_proj * vec4(v_ndc.x, v_ndc.y, 1.0, 1.0);
    vec3 dir = normalize(far.xyz / far.w - near.xyz / near.w);
    float pixel = max(length(dFdx(dir)), length(dFdy(dir)));
    vec3 sun = normalize(pc.to_sun.xyz);

    // The sky above the horizon; below it, the horizon's colour going darker (it is mostly hidden by the land).
    vec3 up_dir = normalize(vec3(dir.xy, max(dir.z, 0.002)));
    vec3 color = scatter(up_dir, sun, pc.to_sun.w);
    if (dir.z < 0.0) color *= exp(dir.z * 6.0);

    // The moon, in front of the stars and behind the air (so by day it is a pale ghost).
    vec3 moon_dir = normalize(pc.to_moon.xyz);
    float cover = 0.0;
    vec3 moon = vec3(0.0);
    if (dir.z > -0.02) {
        moon = moon_disc(dir, moon_dir, sun, pixel, pc.to_sun.w, pc.to_moon.w, cover);
        float moon_up = smoothstep(-0.02, 0.03, moon_dir.z);
        moon *= moon_up;
        cover *= moon_up;
    }

    // Night: a deep blue floor and the stars, which turn with the sun's path (about the pole, kPole) and fade out
    // as the sky brightens.
    float night = smoothstep(0.05, -0.2, sun.z);
    color += mix(vec3(0.0015, 0.0025, 0.006), vec3(0.006, 0.0015, 0.001), pc.to_moon.w * night) *
             (1.0 + 2.0 * (1.0 - clamp(up_dir.z * 3.0, 0.0, 1.0)));
    if (night > 0.0 && dir.z > 0.0) {
        float a = pc.params.y;
        // Rodrigues: dir turned by a about the pole.
        vec3 sd = dir * cos(a) + cross(kPole, dir) * sin(a) + kPole * dot(kPole, dir) * (1.0 - cos(a));
        color += stars(sd, pixel, pc.params.x) * night * smoothstep(0.0, 0.15, dir.z) * (1.0 - cover);
        color += shooting_stars(dir, pixel, pc.params.x) * night * smoothstep(0.05, 0.2, dir.z) * (1.0 - cover);
        color += aurora(dir, pc.params.x, max(pc.haze.a - 1.0, 0.0)) * night * (1.0 - pc.params.z);
    }

    // The sun and moon.
    if (dir.z > -0.02) color += sun_disc(dir, sun, pixel, sun_transmittance(sun, pc.to_sun.w)) * (1.0 - cover) + moon;

    // A storm greys the sky over; the lightning lights it.
    float storm = pc.params.z;
    color = mix(color, vec3(min(dot(color, vec3(0.2126, 0.7152, 0.0722)), 2.0)) * 0.6, storm * 0.8);
    // The clouds in front of it all; the lightning lights them more than the clear air. Four bilinear taps half a
    // cloud pixel off make a 3x3 tent over their image, so the 2x2 blocks they are marched in (one pixel a frame)
    // do not show at their edges as the view turns.
    vec2 uv = v_ndc * 0.5 + 0.5;
    vec2 half_texel = 0.5 / vec2(textureSize(u_clouds, 0));
    vec4 clouds = 0.25 * (textureLod(u_clouds, uv + vec2(-half_texel.x, -half_texel.y), 0.0) +
                          textureLod(u_clouds, uv + vec2(half_texel.x, -half_texel.y), 0.0) +
                          textureLod(u_clouds, uv + vec2(-half_texel.x, half_texel.y), 0.0) +
                          textureLod(u_clouds, uv + vec2(half_texel.x, half_texel.y), 0.0));
    color = color * clouds.a + clouds.rgb;
    color += vec3(0.8, 0.85, 1.0) * pc.params.w * (1.5 + 1.5 * (1.0 - clouds.a));

    if (pc.haze.a > 0.0) {
        // Into the haze at and below the horizon, clear a few degrees up (as shaders/sky.frag).
        float h = 1.0 - smoothstep(-0.02, 0.12, dir.z);
        color = mix(color, pc.haze.rgb, h * h * min(pc.haze.a, 1.0));
    }
    o_color = vec4(color, 1.0);
}
