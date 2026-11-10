#pragma once

namespace hailstone::plugin {
// One cell of the client's A_SpellIcons sheet, passed from the app's UiTextures to the native
// plugin host table (sdk/hailstone_plugin.h HailstoneSpellIcon). `texture` is an ImTextureID.
struct SpellIcon {
    void* texture = nullptr;
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
};
} // namespace hailstone::plugin
