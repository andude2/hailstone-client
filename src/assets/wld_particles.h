#pragma once

// Classic WLD particle clouds (0x34 PCLOUDDEF) as emitter definitions.
//
// Torches, lanterns and braziers in the S3D object archives carry their flames
// and smoke as 0x34 clouds hung on a skeleton node (bazaar's HAVSTLIGHT300..302
// lanterns: L307_PCD; FTORCH301: L301_PCD flame and CSMOKE_PCD smoke). The
// graphics DLL reads each cloud into a record and converts it into the same
// 0x1a0-byte emitter descriptor the EDD files hold (the converter at
// 0x1001c650 in the shipped EQGraphicsDX9.dll, called from the bulk 0x34
// reader at 0x1001eb65 and the on-demand one at 0x1001de7e; OpenEQ's
// docs/WLD_PARTICLE_RUNTIME.md describes the same code at 0x1001c410 of
// another build), registers it with the particle manager's WLD list, and the
// skeleton's particle node owns one emitter per placed actor. Evidence and the
// field table: docs/formats/particles.md, "WLD particle clouds".

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "assets/spell_effects.h"
#include "assets/wld.h"

namespace hailstone::assets {

// The texture a cloud's 0x26 sprite resolves to (0x26 -> 0x05 -> 0x04 -> 0x03).
struct ParticleTexture {
    std::string file;              // the first frame's file, e.g. "GENG00.DDS"
    std::uint32_t frames = 1;      // bitmaps in the 0x04
    std::uint32_t timing_ms = 0;   // the 0x04's sleep
    std::uint32_t material = 0;    // the 0x26 material word
    std::vector<std::string> files; // separate bitmap frames, including repeated frames in the authored sequence
};

// Follows a 0x26 reference; nullopt when the chain is broken.
std::optional<ParticleTexture> particle_texture(const wld::WldFile& wld, wld::Ref sprite);

// The renderer's default material alias table entry for a negative WLD
// material handle (CRender's default alias installer). 0x80000017, the one
// ordinary clouds use, resolves to 0x011b0507 (OpenEQ docs/WLD_PARTICLE_MATERIALS.md); blood's
// 0x80000018 resolves to 0x01080507 (stock default installer 0x1009abe1), 0x80000019 to 0x00000587
// (masked, default installer 0x1009aa40). nullopt for other aliases.
std::optional<std::uint32_t> particle_material_alias(std::uint32_t handle);

// The DLL's 0x34 -> emitter descriptor conversion (0x1001c650). `name` is the
// cloud's fragment name (the "TFIRE2" prefix has its own fixed values).
EmitterDefinition particle_cloud_definition(const wld::ParticleCloud& cloud, const ParticleTexture& texture,
                                            std::string_view name);

// The DLL's tangent lookup (0x100b9450): a 512-per-turn table indexed by the
// truncated angle masked to 0..127.
float particle_tan(float angle);

} // namespace hailstone::assets
