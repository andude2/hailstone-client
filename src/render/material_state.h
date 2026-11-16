#pragma once

// Render state per material blend mode, shared by the zone, object and actor
// renderers. The EQG classes follow the original's Alpha_* and AddAlpha
// shaders (state flags +5..+9 applied by 0x10088a50).

#include <cstddef>
#include <cstdint>
#include <span>

#include "assets/model.h"

namespace hailstone::render {

// Fog distances that keep a draw unfogged in shaders with linear fog.
inline constexpr float kNoFogStart = 1.0e30f;
inline constexpr float kNoFogEnd = 2.0e30f;

struct MaterialState {
    bool draw = true;       // false: the original draws nothing (EqgShader::Unmapped)
    int pass = 0;           // 0 opaque (z-write), 1 alpha blended, 2 additive ONE/ONE (no z-write for 1 and 2)
    float alpha_ref = 0.0f; // alpha test, GREATEREQUAL (0 = off)
    float opacity = 1.0f;   // multiplies the texture alpha (classic translucent methods)
    bool fog = true;        // false: fog off (state flag +6)
    bool lit = true;        // false: texture only (COLOROP SELECTARG1, state flag +6)
    bool masked_texture = false; // decode palette index 0 of BMPs as transparent
    bool layer_alpha = false; // an opaque effect still reads diffuse alpha to combine its layers

    // Opaque without an alpha test: the texture's alpha is never read (EQG Opaque_* classes, docs/graphics.md;
    // classic opaque methods), so the colour stored under its alpha 0 is drawn
    // (render::TextureUse::OpaqueColor).
    bool alpha_unused() const { return draw && pass == 0 && alpha_ref == 0.0f && opacity == 1.0f && !layer_alpha; }
};

// The frame an animated texture (assets::Material::frames, frame_delay_ms) shows at `time_ms`: a classic
// SIMPLESPRITEDEF (WLD 0x04, flags 0x08 with a delay) steps through its BMINFO frames one every delay, from the
// first, and wraps (t3dDefineSimpleSprite / s3dSetMaterialDefSimpleSprite in the Mac client's names; FIRE1_SPRITE,
// the flame of every classic campfire, torch and brazier, is fire1..fire4 at 200 ms). One frame, or no delay, is a
// still texture: frame 0. Shared by the zone and object renderers so a flame on a door (a campfire placed by the
// server) runs in step with the same flame in the zone.
inline std::size_t texture_frame(std::uint64_t time_ms, std::uint32_t delay_ms, std::size_t count) {
    if (count < 2 || delay_ms == 0) return 0;
    return static_cast<std::size_t>((time_ms / delay_ms) % count);
}

// All actor colour and shadow passes pick the same animated texture, including a cut-out's current silhouette.
inline std::uint32_t animated_texture_slot(std::span<const std::uint32_t> frames, std::uint32_t fallback,
                                         std::uint64_t time_ms, std::uint32_t delay_ms) {
    return frames.empty() ? fallback : frames[texture_frame(time_ms, delay_ms, frames.size())];
}

inline MaterialState material_state(const assets::Material& m) {
    using assets::BlendMode;
    using assets::EqgShader;
    constexpr float kRef16 = 16.0f / 255.0f;   // state flag +7: ref 0x10
    constexpr float kRef192 = 192.0f / 255.0f; // state flag +8: ref 0xC0
    MaterialState s;
    switch (m.eqg_shader) {
    case EqgShader::Unmapped: s.draw = false; return s;
    // RegionWater / RegionWaterFall: SRCALPHA blend, alpha test, no z-write
    // (the reference of their alpha test is taken as state flag +7's).
    case EqgShader::Water:
    case EqgShader::LavaH:
    case EqgShader::WaterFall: s.pass = 1; s.alpha_ref = kRef16; return s;
    // RegionTerrain, RegionLava, RegionLava2 and the heightmap tiles: opaque,
    // z-write; a tile's later ecosystems blend over the first.
    case EqgShader::Terrain: return s;
    case EqgShader::Lava:
    case EqgShader::Lava2: s.layer_alpha = true; return s;
    case EqgShader::TerrainTile: s.pass = m.effect.overlay ? 1 : 0; return s;
    // A classic region's detail pass (assets/wld_detail.h): state flag +5's SRCALPHA blend, no z-write, the
    // vertex alpha as the opacity.
    case EqgShader::WldDetail: s.pass = 1; return s;
    case EqgShader::Alpha: s.pass = 1; s.alpha_ref = kRef16; s.masked_texture = true; return s;
    case EqgShader::Chroma: s.alpha_ref = kRef192; s.masked_texture = true; return s;
    case EqgShader::AddAlpha:
        s.pass = 2;
        s.alpha_ref = kRef16;
        s.fog = false;
        s.lit = false;
        s.masked_texture = true;
        return s;
    default: break;
    }
    switch (m.blend) {
    case BlendMode::Masked: s.alpha_ref = 0.5f; s.masked_texture = true; break;
    case BlendMode::Transparent25: s.pass = 1; s.opacity = 0.25f; break;
    case BlendMode::Transparent50: s.pass = 1; s.opacity = 0.5f; break;
    case BlendMode::Transparent75: s.pass = 1; s.opacity = 0.75f; break;
    case BlendMode::Additive: s.pass = 2; break;
    default: break;
    }
    return s;
}

} // namespace hailstone::render
