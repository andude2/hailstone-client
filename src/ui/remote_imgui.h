#pragma once

// Lightweight, backend-independent data structures for streaming Dear ImGui input and draw data
// between a headless character process and an attached graphical client.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hailstone::ui {

struct RemoteMouseButtonEvent {
    std::uint8_t button = 0; // ImGuiMouseButton (0..4)
    bool down = false;
};

struct RemoteKeyEvent {
    std::uint16_t key = 0; // ImGuiKey value
    bool down = false;
};

struct RemoteImGuiInput {
    float display_w = 0.0f;
    float display_h = 0.0f;
    float fb_scale_x = 1.0f;
    float fb_scale_y = 1.0f;
    float ui_scale = 1.0f;
    float dt = 1.0f / 60.0f;
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    float wheel_x = 0.0f;
    float wheel_y = 0.0f;
    std::uint8_t mods = 0; // bit 0=Ctrl, 1=Shift, 2=Alt, 3=Super
    std::uint8_t mouse_down_mask = 0; // bits 0..4: mouse buttons currently held for remote input
    bool has_mouse_down_mask = false; // true when mouse_down_mask is authoritative (used to reconcile missed releases)
    bool mouse_valid = false;
    bool focused = true;
    std::vector<RemoteMouseButtonEvent> mouse_buttons;
    std::vector<RemoteKeyEvent> keys;
    std::vector<std::uint32_t> chars;
};

struct RemoteDrawVert {
    float x = 0.0f;
    float y = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    std::uint32_t col = 0;
};

struct RemoteDrawCmd {
    float clip_x = 0.0f;
    float clip_y = 0.0f;
    float clip_z = 0.0f;
    float clip_w = 0.0f;
    std::uint32_t tex_id = 0; // remote ImTextureData::UniqueID, or kRemoteSpellSheetTexBase | sheet (1..7)
    std::uint32_t vtx_offset = 0;
    std::uint32_t idx_offset = 0;
    std::uint32_t elem_count = 0;
};

// High-bit sentinel range in RemoteDrawCmd::tex_id for stock A_SpellIcons sheets (spells01.tga..spells07.tga).
// Headless processes return (kRemoteSpellSheetTexBase | sheet) from PluginHostApi::spell_icon so plugins draw
// spell/buff icons with zero texture pixel streaming; the attached GUI resolves the sheet via its local UiTextures.
inline constexpr std::uint32_t kRemoteSpellSheetTexBase = 0x80000000u;
inline constexpr std::uint32_t kRemoteSpellSheetMax = 7u;

inline constexpr std::uint32_t encode_remote_spell_sheet_tex_id(int sheet) {
    return (sheet >= 1 && sheet <= static_cast<int>(kRemoteSpellSheetMax))
               ? (kRemoteSpellSheetTexBase | static_cast<std::uint32_t>(sheet))
               : 0u;
}

inline constexpr int decode_remote_spell_sheet_tex_id(std::uint32_t tex_id) {
    if ((tex_id & kRemoteSpellSheetTexBase) == 0u) return 0;
    const std::uint32_t sheet = tex_id & ~kRemoteSpellSheetTexBase;
    return (sheet >= 1u && sheet <= kRemoteSpellSheetMax) ? static_cast<int>(sheet) : 0;
}

struct RemoteDrawList {
    std::vector<RemoteDrawVert> vertices;
    std::vector<std::uint16_t> indices;
    std::vector<RemoteDrawCmd> cmds;
};

struct RemoteImGuiFrame {
    float display_w = 0.0f;
    float display_h = 0.0f;
    float fb_scale_x = 1.0f;
    float fb_scale_y = 1.0f;
    bool want_capture_mouse = false;
    bool want_capture_keyboard = false;
    bool want_text_input = false;
    std::int8_t mouse_cursor = 0; // ImGuiMouseCursor (-1 = None, 0 = Arrow, ...)
    std::vector<RemoteDrawList> lists;
};

struct RemoteImGuiTexture {
    std::uint32_t id = 0; // remote ImTextureData::UniqueID
    std::uint16_t tex_w = 0;
    std::uint16_t tex_h = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t w = 0;
    std::uint16_t h = 0;
    bool full = false;    // true: allocate/replace (tex_w x tex_h) image, write (x,y,w,h)
    bool destroy = false; // true: retire and free this texture id
    std::vector<std::byte> rgba; // w * h * 4 bytes (RGBA8), empty when destroy==true
};

// Maximum rows per horizontal upload band for a (width x height) RGBA8 rect so each pipe message stays within
// max_band_bytes. Always returns a value in [1, height] when width > 0 and height > 0 (0 otherwise), avoiding
// uint16_t truncation when narrow rects yield >= 65536 rows per band.
inline std::uint16_t remote_texture_band_rows(std::uint16_t width, std::uint16_t height,
                                              std::size_t max_band_bytes = std::size_t{4} << 20) {
    if (width == 0 || height == 0) return 0;
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4u;
    const std::size_t rows = std::clamp<std::size_t>(max_band_bytes / row_bytes, 1u, height);
    return static_cast<std::uint16_t>(rows);
}

} // namespace hailstone::ui
