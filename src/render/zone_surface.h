#pragma once

// The EQG surface bits of a zone draw's flags (zone.frag's push constant tex2.z), and how a material's effect class
// sets them (render/zone_renderer.cpp prepare, bind). Kept apart so the rules can be tested without a GPU.

#include <cstdint>

#include "assets/model.h"

namespace hailstone::render::zone_surface {

inline constexpr std::uint32_t kNormalMap = 64;          // modern: slot 1 is a tangent-space normal map
inline constexpr std::uint32_t kSpecular = 128;          // modern: a sun highlight (extra.x shininess, extra.y gloss)
inline constexpr std::uint32_t kReflective = 256;        // modern: the sky reflected (extra.z strength)
inline constexpr std::uint32_t kPackedNormal = 131072;   // CBGG1/CBSG1: RG normal, B glow, A gloss
inline constexpr std::uint32_t kGlow = 262144;           // slot 1's blue channel is self-illumination
inline constexpr std::uint32_t kAdditiveGlow = 524288;   // AddAlpha surfaces, HDR in modern lighting

// The surface bits of an opaque, Alpha or Chroma EQG material in the classic effect mode. `normal_map_loaded`: its
// normal map got a texture slot. A packed CBGG1/CBSG1 map gives the normal, the glow and the highlight's gloss, so a
// packed material whose map did not load is drawn as a plain one: no highlight from the packing (it keeps the one
// its name asks for), and a texture pack's maps may still apply.
inline std::uint32_t eqg_surface_flags(const assets::EffectParams& e, bool normal_map_loaded) {
    std::uint32_t flags = 0;
    if (normal_map_loaded) {
        flags |= kNormalMap;
        if (e.packed_normal) flags |= kPackedNormal | kSpecular;
        if (e.glow) flags |= kGlow;
    }
    if (e.specular) flags |= kSpecular;
    if (e.reflective && e.env_strength > 0.0f) flags |= kReflective;
    return flags;
}

// A draw's surface bits as the view draws them: the full-bright view (VertexLighting::None) shows textures as they
// are, so AddAlpha surfaces get no HDR gain there. The shader cannot tell it apart, since every surface is unlit in
// that view and AddAlpha surfaces always are.
constexpr std::uint32_t view_surface_flags(std::uint32_t surface, bool full_bright) {
    return full_bright ? surface & ~kAdditiveGlow : surface;
}

} // namespace hailstone::render::zone_surface
