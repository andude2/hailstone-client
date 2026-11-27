#include "ui/imgui_layer.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName and the focused window, for the gamepad's focus requests
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include "core/log.h"
#include "platform/window.h"
#include "render/gpu.h"
#include "render/vk_context.h"
#include "render/vk_util.h"
#include "ui/imgui_vk_textures.h"
#include "ui/lua/lua_host.h"

namespace hailstone::ui {

namespace {

void check_vk(VkResult r) {
    if (r != VK_SUCCESS) log::error("imgui vulkan backend: VkResult {}", static_cast<int>(r));
}

// EQ UI fonts 1..6 (the <Font> numbers in the stock XML): Arial at these pixel sizes,
// here the Arial-class font found below.
constexpr std::array<float, 7> kUiFontSizes = {0, 10, 12, 13, 14, 16, 20};
std::array<ImFont*, 7> g_ui_fonts{};
// The font list a chat window's font indexes (/chatfontsize 0..10, CChatWindow::SetChatFont: CXWndManager +0x118):
// EQGraphicsDX9's font table @0x100b2100, Arial at these cell heights, fonts 5, 6 and 10 bold (weight 700; the
// others 100 or 0, drawn regular).
constexpr std::array<float, kChatFonts> kChatFontSizes = {10, 12, 14, 15, 16, 20, 24, 20, 24, 14, 40};
constexpr std::array<bool, kChatFonts> kChatFontBold = {false, false, false, false, false, true, true, false, false, false, true};
std::array<ImFont*, kChatFonts> g_chat_fonts{};
float g_ui_scale = 1.0f; // the display scale the fonts were built for
// The name plates' (ImGuiLayer::set_plate_font).
ImFont* g_plate_font = nullptr;
float g_plate_font_size = 15.0f;
bool g_plate_faux_bold = false;

// Metric-compatible Arial-class fonts first (plan section 2), then common sans fonts.
// These are plain file paths; a missing file is simply skipped.
constexpr std::array kFontCandidates = {
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "C:/Windows/Fonts/arial.ttf",
    "C:/Windows/Fonts/segoeui.ttf",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/Library/Fonts/Arial.ttf",
};

// Their bold faces, in the same order.
constexpr std::array kBoldFontCandidates = {
    "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf",
    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/noto/NotoSans-Bold.ttf",
    "C:/Windows/Fonts/arialbd.ttf",
    "C:/Windows/Fonts/segoeuib.ttf",
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    "/Library/Fonts/Arial Bold.ttf",
};

// A path as UTF-8, which ImGui's file functions take (they widen it on Windows). path::string() would be in the
// ANSI code page there, and throws for a name that page cannot show.
std::string utf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::filesystem::path find_font(const std::filesystem::path& wanted) {
    std::error_code ec;
    if (!wanted.empty()) {
        if (std::filesystem::is_regular_file(wanted, ec)) return wanted;
        log::warn("UI font {} not found", utf8(wanted));
    }
    for (const char* p : kFontCandidates)
        if (std::filesystem::is_regular_file(p, ec)) return p;
    return {};
}

// The bold face going with `regular` (one of kFontCandidates), if there is one.
std::filesystem::path find_bold_font(const std::filesystem::path& regular) {
    std::error_code ec;
    for (std::size_t i = 0; i < kFontCandidates.size(); ++i)
        if (regular == kFontCandidates[i] && std::filesystem::is_regular_file(kBoldFontCandidates[i], ec))
            return kBoldFontCandidates[i];
    return {};
}

} // namespace

ImFontConfig detail::ui_font_config() {
    // Since ImGui 1.92 glyphs are rasterised on demand, at the density the backend reports
    // (io.DisplayFramebufferScale), so there are no glyph ranges to give and no per-font density.
    return ImFontConfig();
}

bool detail::build_ui_font_atlas(ImFontAtlas& atlas) {
    // The built-in font and some custom faces lack curly quotes. Let those faces draw straight quotes while
    // keeping the original characters in the chat line, clipboard and log.
    static constexpr std::pair<ImWchar, ImWchar> quote_fallbacks[] = {
        {0x2018, '\''}, {0x2019, '\''}, {0x201c, '"'}, {0x201d, '"'},
    };
    for (ImFont* font : atlas.Fonts) {
        // A face the rasteriser cannot parse has no glyph for anything; the atlas no longer reports that when it is
        // built (there is no build step), so ask the font itself.
        if (!font->IsGlyphInFont('A') && !font->IsGlyphInFont(' ')) return false;
        for (const auto& [curly, straight] : quote_fallbacks) {
            if (!font->IsGlyphInFont(curly) && font->IsGlyphInFont(straight)) font->AddRemapCodepoint(curly, straight);
        }
    }
    return true;
}

namespace {
void clear_ui_interaction() {
    ImGuiContext* g = ImGui::GetCurrentContext();
    if (!g) return;
    ImGuiIO& io = ImGui::GetIO();
    io.ClearEventsQueue();
    io.ClearInputKeys();
    io.ClearInputMouse();
    ImGui::ClearActiveID();
    ImGui::ClearDragDrop();
    // Drop keyboard focus without FocusWindow's popup-closing side effect: confirmations remain open.
    ImGui::SetNavWindow(nullptr);
    g->NavId = 0;
    g->NavFocusScopeId = 0;
    ImGui::NavMoveRequestCancel();
    ImGui::SetNavCursorVisible(false);
    g->MovingWindow = nullptr;
    g->NavActivateId = g->NavActivateDownId = g->NavActivatePressedId = g->NavNextActivateId = 0;
    g->NavNextActivateFlags = ImGuiActivateFlags_None;
    g->WantCaptureMouseNextFrame = g->WantCaptureKeyboardNextFrame = -1;
    g->WantTextInputNextFrame = 0;
    g->PlatformImeData.WantVisible = false;
    io.WantCaptureMouse = io.WantCaptureKeyboard = io.WantTextInput = false;
}
void clear_ui_draw_data() {
    if (ImGuiContext* g = ImGui::GetCurrentContext())
        for (ImGuiViewportP* viewport : g->Viewports) viewport->DrawDataP.Clear();
}
} // namespace

void detail::ImGuiVisibility::set_visible(bool visible) {
    if (visible_ == visible) return;
    visible_ = visible;
    clear_ui_interaction();
    clear_ui_draw_data();
    if (ImGui::GetCurrentContext()) ImGui::GetIO().SetAppAcceptingEvents(visible);
}

void detail::ImGuiVisibility::prepare_input() {
    if (visible_ || !ImGui::GetCurrentContext()) return;
    clear_ui_interaction();
    ImGui::GetIO().SetAppAcceptingEvents(false);
}

bool detail::ImGuiVisibility::finish_frame(bool render) {
    if (visible_ && render) {
        ImGui::Render();
        return true;
    }
    ImGui::EndFrame();
    clear_ui_draw_data();
    if (!visible_) clear_ui_interaction();
    return false;
}

bool detail::ImGuiVisibility::wants_mouse() const {
    return visible_ && ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse;
}
bool detail::ImGuiVisibility::wants_keyboard(int requested) const {
    return visible_ && ImGui::GetCurrentContext() && keyboard_for_ui(requested);
}

struct ImGuiLayer::RemoteState {
    static constexpr std::uint64_t kRetireFrames = render::VkContext::kFramesInFlight + 1;

    struct TexEntry {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        std::uint16_t width = 0;
        std::uint16_t height = 0;
        bool ready = false;
    };

    struct Staging {
        render::Buffer buffer;
        void* mapped = nullptr;
    };

    struct PendingEvents {
        std::vector<RemoteMouseButtonEvent> mouse_buttons;
        std::vector<RemoteKeyEvent> keys;
        std::vector<std::uint32_t> chars;
        float wheel_x = 0.0f;
        float wheel_y = 0.0f;
        bool saw_mouse_pos = false;
        bool saw_focus = false;
    };

    render::VkContext& vk;
    bool enabled = false;
    bool want_capture_mouse = false;
    bool want_capture_keyboard = false;
    bool want_text_input = false;
    std::int8_t mouse_cursor = 0;
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    bool mouse_valid = false;
    bool focused = true;
    std::uint8_t mods = 0;
    // Raw events captured directly from SDL / pad pointer before local ImGui's InputEventsQueue filtering and
    // trickling. Eating local MouseButton/Key Down events leaves local io.MouseDown / key.Down false, which causes
    // ImGuiIO::AddMouseButtonEvent(b, false) / AddKeyEvent(k, false) to drop the matching release via Dear ImGui's
    // duplicate-event filter; capturing straight from SDL prevents dropped releases and trickle duplication.
    PendingEvents pending;
    std::uint8_t sdl_mouse_down_mask = 0;    // physical mouse buttons currently held (bits 0..4)
    std::uint8_t pad_mouse_down_mask = 0;    // gamepad pointer buttons currently held (bits 0..1)
    std::uint8_t captured_mouse_mask = 0;    // buttons pressed while remote captured mouse (latches capture during drags)
    std::uint8_t local_owned_mouse_mask = 0; // buttons pressed while local ImGui was active
    std::unordered_map<std::uint32_t, std::uint16_t> held_keys; // SDL_Scancode -> ImGuiKey for guaranteed key-up
    std::optional<RemoteImGuiInput> captured_input;
    std::optional<RemoteImGuiFrame> frame;
    std::vector<RemoteImGuiTexture> pending_uploads;
    std::unordered_map<std::uint32_t, TexEntry> textures;
    std::vector<std::pair<std::uint64_t, TexEntry>> retired;
    std::vector<Staging> staging;
    std::vector<std::unique_ptr<ImDrawList>> built_lists;

    explicit RemoteState(render::VkContext& context)
        : vk(context), staging(render::VkContext::kFramesInFlight) {}

    ~RemoteState() {
        for (auto& [id, e] : textures) destroy_tex(e);
        for (auto& [when, e] : retired) destroy_tex(e);
        for (Staging& s : staging) s.buffer.destroy(vk.allocator());
    }

    TexEntry make_tex(std::uint16_t w, std::uint16_t h) {
        TexEntry e;
        e.width = w;
        e.height = h;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = {w, h, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        render::vk_check(vmaCreateImage(vk.allocator(), &info, &ai, &e.image, &e.allocation, nullptr),
                         "create remote imgui image");
        e.view = render::create_image_view(vk.device(), e.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1);
        e.set = ui::add_imgui_texture(e.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return e;
    }

    void destroy_tex(TexEntry& e) {
        if (e.set) ui::remove_imgui_texture(e.set);
        if (e.view) vkDestroyImageView(vk.device(), e.view, nullptr);
        if (e.image) vmaDestroyImage(vk.allocator(), e.image, e.allocation);
        e = TexEntry{};
    }

    void retire(TexEntry e) {
        if (e.image || e.view || e.set) retired.emplace_back(vk.frames_begun() + kRetireFrames, e);
    }

    void collect() {
        if (retired.empty()) return;
        const std::uint64_t now = vk.frames_begun();
        std::erase_if(retired, [&](auto& r) {
            if (r.first > now) return false;
            destroy_tex(r.second);
            return true;
        });
    }

    void clear_session() {
        frame.reset();
        pending_uploads.clear();
        captured_input.reset();
        pending = {};
        sdl_mouse_down_mask = 0;
        pad_mouse_down_mask = 0;
        captured_mouse_mask = 0;
        local_owned_mouse_mask = 0;
        held_keys.clear();
        want_capture_mouse = false;
        want_capture_keyboard = false;
        want_text_input = false;
        mouse_cursor = 0;
        mouse_valid = false;
        focused = true;
        mods = 0;
        for (auto& [id, e] : textures) retire(e);
        textures.clear();
    }

    void record_uploads(const render::FrameInfo& f) {
        collect();
        if (pending_uploads.empty()) return;
        auto uploads = std::exchange(pending_uploads, {});

        VkDeviceSize total = 0;
        for (const auto& u : uploads) {
            if (!u.destroy) total += u.rgba.size();
        }
        Staging& s = staging[f.frame_slot];
        if (total > 0 && s.buffer.size < total) {
            s.buffer.destroy(vk.allocator());
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size = std::max<VkDeviceSize>(total, VkDeviceSize{2} << 20);
            info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            VmaAllocationCreateInfo ai{};
            ai.usage = VMA_MEMORY_USAGE_AUTO;
            ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo out{};
            render::vk_check(vmaCreateBuffer(vk.allocator(), &info, &ai, &s.buffer.buffer, &s.buffer.allocation, &out),
                             "create remote imgui staging buffer");
            s.buffer.size = info.size;
            s.mapped = out.pMappedData;
        }

        VkDeviceSize offset = 0;
        auto* dst = static_cast<std::byte*>(s.mapped);
        struct Op {
            std::uint32_t id = 0;
            VkBufferImageCopy region{};
        };
        std::vector<Op> ops;
        ops.reserve(uploads.size());
        for (const auto& u : uploads) {
            if (u.destroy) {
                if (auto it = textures.find(u.id); it != textures.end()) {
                    retire(it->second);
                    textures.erase(it);
                }
                continue;
            }
            if (u.tex_w == 0 || u.tex_h == 0 || u.w == 0 || u.h == 0 || u.rgba.empty()) continue;
            if (static_cast<std::uint32_t>(u.x) + u.w > u.tex_w ||
                static_cast<std::uint32_t>(u.y) + u.h > u.tex_h)
                continue;
            TexEntry& e = textures[u.id];
            if (u.full || !e.image || e.width != u.tex_w || e.height != u.tex_h) {
                if (e.image) retire(e);
                e = make_tex(u.tex_w, u.tex_h);
            }
            if (e.image && !e.set) e.set = ui::add_imgui_texture(e.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            std::memcpy(dst + offset, u.rgba.data(), u.rgba.size());
            VkBufferImageCopy c{};
            c.bufferOffset = offset;
            c.bufferRowLength = u.w;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageOffset = {u.x, u.y, 0};
            c.imageExtent = {u.w, u.h, 1};
            ops.push_back({u.id, c});
            offset += u.rgba.size();
        }
        if (ops.empty()) return;
        vmaFlushAllocation(vk.allocator(), s.buffer.allocation, 0, offset);

        for (const Op& op : ops) {
            auto it = textures.find(op.id);
            if (it == textures.end() || !it->second.image) continue;
            TexEntry& e = it->second;
            render::image_barrier(f.cmd, e.image, VK_IMAGE_ASPECT_COLOR_BIT,
                                  e.ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                  VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkCmdCopyBufferToImage(f.cmd, s.buffer.buffer, e.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &op.region);
            render::image_barrier(f.cmd, e.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            e.ready = true;
        }
    }

    void append_to_draw_data(ImDrawData* data) {
        if (!enabled || !frame || frame->lists.empty() || !data) return;
        ImDrawListSharedData* shared = ImGui::GetDrawListSharedData();
        if (!shared) return;
        const std::size_t need = frame->lists.size();
        while (built_lists.size() < need) built_lists.push_back(std::make_unique<ImDrawList>(shared));
        for (std::size_t i = 0; i < need; ++i) {
            const RemoteDrawList& src = frame->lists[i];
            ImDrawList* dst = built_lists[i].get();
            dst->CmdBuffer.resize(0);
            dst->IdxBuffer.resize(0);
            dst->VtxBuffer.resize(0);
            if (src.vertices.empty() || src.indices.empty() || src.cmds.empty()) continue;
            dst->VtxBuffer.resize(static_cast<int>(src.vertices.size()));
            for (std::size_t v = 0; v < src.vertices.size(); ++v) {
                const RemoteDrawVert& sv = src.vertices[v];
                dst->VtxBuffer[static_cast<int>(v)] = ImDrawVert{ImVec2(sv.x, sv.y), ImVec2(sv.u, sv.v), sv.col};
            }
            dst->IdxBuffer.resize(static_cast<int>(src.indices.size()));
            std::memcpy(dst->IdxBuffer.Data, src.indices.data(), src.indices.size() * sizeof(std::uint16_t));
            for (const RemoteDrawCmd& sc : src.cmds) {
                if (sc.elem_count == 0) continue;
                if (static_cast<std::size_t>(sc.idx_offset) + sc.elem_count > src.indices.size()) continue;
                if (sc.vtx_offset > src.vertices.size()) continue;
                auto it = textures.find(sc.tex_id);
                if (it == textures.end() || !it->second.ready || !it->second.set) continue;
                ImDrawCmd cmd{};
                cmd.ClipRect = ImVec4(sc.clip_x, sc.clip_y, sc.clip_z, sc.clip_w);
                cmd.TexRef = ImTextureRef(reinterpret_cast<ImTextureID>(it->second.set));
                cmd.VtxOffset = sc.vtx_offset;
                cmd.IdxOffset = sc.idx_offset;
                cmd.ElemCount = sc.elem_count;
                cmd.UserCallback = nullptr;
                dst->CmdBuffer.push_back(cmd);
            }
            if (!dst->CmdBuffer.empty()) data->AddDrawList(dst);
        }
    }
};

ImGuiLayer::ImGuiLayer(platform::Window& window, render::VkContext& vk, const ImGuiLayerDesc& desc)
    : window_(window), vk_(vk) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // The client's own windows keep their places (ImGui saves them every few seconds and at the end).
    ini_path_ = utf8(desc.ini_path);
    io.IniFilename = ini_path_.empty() ? nullptr : ini_path_.c_str();
    // No keyboard navigation: as in the original, the keyboard stays the game's after a click on a window (with it
    // on, a focused window took every key, and Space / Enter pressed its last clicked button). Only an edit box
    // being typed in or a modal dialog keeps the keys, not a widget the mouse holds (keyboard_for_ui); Tab still
    // moves between fields.

    scale_ = desc.scale_override > 0.0f ? desc.scale_override : window_.content_scale();
    g_ui_scale = scale_;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.ScaleAllSizes(scale_);

    font_size_ = desc.font_size;
    font_path_ = desc.font_path;
    build_fonts();
    // Script windows (ui/lua) rely on ImGui's error recovery to close what a failed script left open.
    io.ConfigErrorRecovery = true;
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableTooltip = true;
    configure_keys();

    if (!ImGui_ImplSDL3_InitForVulkan(window_.sdl_window())) throw std::runtime_error("ImGui_ImplSDL3_InitForVulkan failed");
    window_.set_event_listener([this](const SDL_Event& ev) { on_sdl_event(ev); });
    // The gamepad is the window's (platform::Window opens it): the backend gets it only in menu mode
    // (apply_pad_navigation), never opening pads itself.
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);

    // The backend's own textures (the font atlas, made again as it grows). The client's (item icon sheets, spell
    // icons, the stock UI art the Lua windows draw, browser tabs) come from pools of their own that grow as they fill
    // (ui/imgui_vk_textures.h).
    constexpr std::uint32_t kBackendSets = 32;

    // The UI draws in the frame's overlay pass (render::VkContext::begin_overlay):
    // the swapchain image, one sample, no depth.
    color_format_ = vk_.color_format(); // the backend keeps a pointer to it
    min_image_count_ = vk_.min_image_count();
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = vk_.instance();
    info.PhysicalDevice = vk_.physical_device();
    info.Device = vk_.device();
    info.QueueFamily = vk_.queue_family();
    info.Queue = vk_.queue();
    info.DescriptorPoolSize = kBackendSets; // the backend makes its own pool (image and sampler descriptors, split in 1.92)
    info.PipelineCache = vk_.pipeline_cache();
    info.MinImageCount = min_image_count_;
    // Vertex buffers rotate per RenderDrawData call; at least one per frame in flight.
    info.ImageCount = std::max({vk_.image_count(), render::VkContext::kFramesInFlight, min_image_count_});
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format_;
    info.CheckVkResultFn = check_vk;
    if (!ImGui_ImplVulkan_Init(&info)) throw std::runtime_error("ImGui_ImplVulkan_Init failed");
    init_imgui_textures(vk_.device());
    remote_ = std::make_unique<RemoteState>(vk_);
}

ImGuiLayer::~ImGuiLayer() {
    vk_.wait_idle();
    window_.set_event_listener({});
    remote_.reset();
    shutdown_imgui_textures();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void ImGuiLayer::on_sdl_event(const SDL_Event& ev) {
    if (!remote_ || !remote_->enabled) {
        ImGui_ImplSDL3_ProcessEvent(&ev);
        return;
    }
    constexpr std::size_t kMaxPendingEvents = 64;
    const SDL_WindowID win_id = SDL_GetWindowID(window_.sdl_window());
    auto is_mod_key = [](ImGuiKey key) {
        return key == ImGuiMod_Ctrl || key == ImGuiMod_Shift || key == ImGuiMod_Alt || key == ImGuiMod_Super;
    };
    switch (ev.type) {
    case SDL_EVENT_MOUSE_MOTION:
        if (ev.motion.windowID == win_id) {
            remote_->mouse_x = ev.motion.x;
            remote_->mouse_y = ev.motion.y;
            remote_->mouse_valid = true;
            remote_->pending.saw_mouse_pos = true;
        }
        ImGui_ImplSDL3_ProcessEvent(&ev);
        return;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (ev.button.windowID == win_id) {
            int btn = -1;
            if (ev.button.button == SDL_BUTTON_LEFT) btn = 0;
            else if (ev.button.button == SDL_BUTTON_RIGHT) btn = 1;
            else if (ev.button.button == SDL_BUTTON_MIDDLE) btn = 2;
            else if (ev.button.button == SDL_BUTTON_X1) btn = 3;
            else if (ev.button.button == SDL_BUTTON_X2) btn = 4;
            if (btn >= 0) {
                const bool down = ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                const auto bit = static_cast<std::uint8_t>(1u << btn);
                remote_->mouse_x = ev.button.x;
                remote_->mouse_y = ev.button.y;
                remote_->mouse_valid = true;
                remote_->pending.saw_mouse_pos = true;
                if (down) remote_->sdl_mouse_down_mask |= bit;
                else remote_->sdl_mouse_down_mask &= static_cast<std::uint8_t>(~bit);
                if (remote_->pending.mouse_buttons.size() < kMaxPendingEvents)
                    remote_->pending.mouse_buttons.push_back({static_cast<std::uint8_t>(btn), down});
            }
        }
        ImGui_ImplSDL3_ProcessEvent(&ev);
        return;
    case SDL_EVENT_MOUSE_WHEEL:
        if (ev.wheel.windowID == win_id) {
            remote_->pending.wheel_x += -ev.wheel.x;
            remote_->pending.wheel_y += ev.wheel.y;
        }
        ImGui_ImplSDL3_ProcessEvent(&ev);
        return;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        ImGuiContext* ctx = ImGui::GetCurrentContext();
        const int prev_size = ctx ? ctx->InputEventsQueue.Size : 0;
        ImGui_ImplSDL3_ProcessEvent(&ev);
        if (ctx && ev.key.windowID == win_id) {
            const auto sc = static_cast<std::uint32_t>(ev.key.scancode);
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                for (int i = prev_size; i < ctx->InputEventsQueue.Size; ++i) {
                    const ImGuiInputEvent& e = ctx->InputEventsQueue[i];
                    if (e.Type != ImGuiInputEventType_Key || !e.Key.Down || is_mod_key(e.Key.Key)) continue;
                    const auto key = static_cast<std::uint16_t>(e.Key.Key);
                    const bool already_held = remote_->held_keys.contains(sc);
                    remote_->held_keys[sc] = key;
                    if (!already_held && remote_->pending.keys.size() < kMaxPendingEvents)
                        remote_->pending.keys.push_back({key, true});
                }
            } else {
                if (auto it = remote_->held_keys.find(sc); it != remote_->held_keys.end()) {
                    if (remote_->pending.keys.size() < kMaxPendingEvents)
                        remote_->pending.keys.push_back({it->second, false});
                    remote_->held_keys.erase(it);
                } else {
                    for (int i = prev_size; i < ctx->InputEventsQueue.Size; ++i) {
                        const ImGuiInputEvent& e = ctx->InputEventsQueue[i];
                        if (e.Type != ImGuiInputEventType_Key || e.Key.Down || is_mod_key(e.Key.Key)) continue;
                        if (remote_->pending.keys.size() < kMaxPendingEvents)
                            remote_->pending.keys.push_back({static_cast<std::uint16_t>(e.Key.Key), false});
                    }
                }
            }
        }
        return;
    }
    case SDL_EVENT_TEXT_INPUT: {
        ImGuiContext* ctx = ImGui::GetCurrentContext();
        const int prev_size = ctx ? ctx->InputEventsQueue.Size : 0;
        ImGui_ImplSDL3_ProcessEvent(&ev);
        if (ctx && ev.text.windowID == win_id) {
            for (int i = prev_size; i < ctx->InputEventsQueue.Size; ++i) {
                const ImGuiInputEvent& e = ctx->InputEventsQueue[i];
                if (e.Type == ImGuiInputEventType_Text && remote_->pending.chars.size() < kMaxPendingEvents)
                    remote_->pending.chars.push_back(e.Text.Char);
            }
        }
        return;
    }
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (ev.window.windowID == win_id) {
            const bool gained = ev.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
            remote_->focused = gained;
            remote_->pending.saw_focus = true;
            if (!gained) {
                for (std::uint8_t b = 0; b < ImGuiMouseButton_COUNT; ++b) {
                    if ((remote_->sdl_mouse_down_mask & (1u << b)) &&
                        remote_->pending.mouse_buttons.size() < kMaxPendingEvents) {
                        remote_->pending.mouse_buttons.push_back({b, false});
                    }
                }
                remote_->sdl_mouse_down_mask = 0;
                remote_->captured_mouse_mask = 0;
                remote_->local_owned_mouse_mask = 0;
                for (const auto& [sc, key] : remote_->held_keys) {
                    if (remote_->pending.keys.size() < kMaxPendingEvents)
                        remote_->pending.keys.push_back({key, false});
                }
                remote_->held_keys.clear();
            }
        }
        ImGui_ImplSDL3_ProcessEvent(&ev);
        return;
    default:
        ImGui_ImplSDL3_ProcessEvent(&ev);
        return;
    }
}

void ImGuiLayer::build_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    g_ui_fonts.fill(nullptr);
    g_chat_fonts.fill(nullptr);
    g_plate_font = nullptr;
    g_plate_font_size = std::round(plate_size_ * scale_);
    g_plate_faux_bold = false;
    const ImFontConfig cfg = detail::ui_font_config();
    float size = std::round(font_size_ * scale_);
    // The face set (set_font_face) when its file is there, else the Arial-class font found.
    std::filesystem::path font, bold;
    try {
        std::error_code ec;
        font = !face_.empty() && std::filesystem::is_regular_file(face_, ec) ? face_ : find_font(font_path_);
        bold = font == face_ ? face_bold_ : find_bold_font(font);
        if (bold.empty() || !std::filesystem::is_regular_file(bold, ec)) bold = font;
    } catch (const std::exception& e) {
        // A saved face that cannot be used must not stop every start: the built-in font stands in.
        log::warn("UI font: {}; using the built-in font", e.what());
        font.clear();
        bold.clear();
    }
    auto use_default = [&] {
        io.Fonts->Clear();
        g_ui_fonts.fill(nullptr);
        g_chat_fonts.fill(nullptr);
        g_plate_font = nullptr;
        g_plate_faux_bold = plate_bold_;
        ImFontConfig def = cfg;
        def.SizePixels = std::round(13.0f * scale_);
        io.Fonts->AddFontDefault(&def);
        detail::build_ui_font_atlas(*io.Fonts);
        log::debug("UI font: built-in, scale {:.2f}", scale_);
    };
    ImFont* loaded = nullptr;
    if (!font.empty()) loaded = io.Fonts->AddFontFromFileTTF(utf8(font).c_str(), size, &cfg);
    if (!loaded) {
        use_default();
        return;
    }
    log::debug("UI font: {} at {:.0f} px, scale {:.2f}", utf8(font), size, scale_);

    {
        for (std::size_t i = 1; i < kUiFontSizes.size(); ++i) {
            g_ui_fonts[i] = io.Fonts->AddFontFromFileTTF(utf8(font).c_str(), std::round(kUiFontSizes[i] * scale_),
                                                         &cfg);
        }
        for (std::size_t i = 0; i < kChatFonts; ++i) {
            const std::filesystem::path& file = kChatFontBold[i] ? bold : font;
            g_chat_fonts[i] = io.Fonts->AddFontFromFileTTF(utf8(file).c_str(),
                                                           std::round(kChatFontSizes[i] * scale_), &cfg);
        }
        // The name plates': their own face when its file is there, else the UI's; its bold file when bold is asked
        // for, else the regular one struck twice (plate_font_faux_bold).
        std::error_code ec;
        const bool own = !plate_face_.empty() && std::filesystem::is_regular_file(plate_face_, ec);
        const std::filesystem::path& regular = own ? plate_face_ : font;
        std::filesystem::path plate_bold = own ? plate_face_bold_ : (bold == font ? std::filesystem::path() : bold);
        if (!plate_bold.empty() && !std::filesystem::is_regular_file(plate_bold, ec)) plate_bold.clear();
        const std::filesystem::path& file = plate_bold_ && !plate_bold.empty() ? plate_bold : regular;
        g_plate_faux_bold = plate_bold_ && plate_bold.empty();
        g_plate_font = io.Fonts->AddFontFromFileTTF(utf8(file).c_str(), g_plate_font_size, &cfg);
    }
    // A file that reads but is not a font the rasteriser can parse fails here, not when the texture is made.
    if (!detail::build_ui_font_atlas(*io.Fonts)) {
        log::warn("UI font {} could not be built; using the built-in font", utf8(font));
        use_default();
    }
}

void ImGuiLayer::set_font_face(const std::filesystem::path& regular, const std::filesystem::path& bold) {
    if (regular == face_ && bold == face_bold_) return;
    face_ = regular;
    face_bold_ = bold;
    fonts_dirty_ = true;
}

void ImGuiLayer::set_plate_font(const std::filesystem::path& regular, const std::filesystem::path& bold, float size,
                                bool bold_face) {
    if (regular == plate_face_ && bold == plate_face_bold_ && size == plate_size_ && bold_face == plate_bold_) return;
    plate_face_ = regular;
    plate_face_bold_ = bold;
    plate_size_ = size;
    plate_bold_ = bold_face;
    fonts_dirty_ = true;
}

void ImGuiLayer::new_frame() {
    if (frame_open_) end_frame_without_render();
    if (fonts_dirty_) {
        // The UI font changed (/fontface): the atlas built again, its texture made again by the backend's NewFrame.
        fonts_dirty_ = false;
        vk_.wait_idle();
        build_fonts();
    }
    std::uint32_t min_images = vk_.min_image_count();
    if (min_images != min_image_count_) {
        min_image_count_ = min_images;
        ImGui_ImplVulkan_SetMinImageCount(min_images);
    }
    ImGui_ImplVulkan_NewFrame();
    apply_pad_navigation();
    ImGui_ImplSDL3_NewFrame();
    feed_pad_pointer();
    // Minimized (or not sized yet) the display is 0 x 0: ImGui would pull every window onto that empty screen, and
    // the UI ini would keep those places. The last real size stands in until the window is back.
    ImGuiIO& io = ImGui::GetIO();
    if (reset_interaction_pending_) {
        reset_interaction_pending_ = false;
        // Clear any widget/window hold left over from the previous character (e.g. clicking a swap button) without
        // wiping the fresh SDL input events already queued for this frame.
        io.ClearInputMouse();
        io.ClearInputKeys();
        ImGui::ClearActiveID();
        ImGui::ClearDragDrop();
        GImGui->MovingWindow = nullptr;
        GImGui->NavId = 0;
        GImGui->NavActivateId = GImGui->NavActivateDownId = GImGui->NavActivatePressedId = GImGui->NavNextActivateId = 0;
        GImGui->WantCaptureMouseNextFrame = GImGui->WantCaptureKeyboardNextFrame = -1;
        io.WantCaptureMouse = io.WantCaptureKeyboard = io.WantTextInput = false;
    }
    if (window_.minimized() || io.DisplaySize.x < 64.0f || io.DisplaySize.y < 64.0f) {
        if (last_w_ > 0.0f) io.DisplaySize = ImVec2(last_w_, last_h_);
    } else {
        last_w_ = io.DisplaySize.x;
        last_h_ = io.DisplaySize.y;
    }
    if (remote_ && remote_->enabled) {
        RemoteImGuiInput in;
        in.display_w = io.DisplaySize.x;
        in.display_h = io.DisplaySize.y;
        in.fb_scale_x = io.DisplayFramebufferScale.x;
        in.fb_scale_y = io.DisplayFramebufferScale.y;
        in.ui_scale = scale_;
        in.dt = io.DeltaTime;
        const bool local_active = GImGui->ActiveId != 0;
        const bool allow_input = visibility_.visible() && !mouse_locked_;
        auto pending = std::exchange(remote_->pending, {});
        // Derive live focus and modifier state from SDL rather than ImGui's local queue so eating local presses
        // never desyncs modifiers or leaves remote input stuck unfocused after swapping characters.
        if (!pending.saw_focus) remote_->focused = window_.focused();
        const SDL_Keymod sdl_mods = SDL_GetModState();
        remote_->mods = static_cast<std::uint8_t>(
            ((sdl_mods & SDL_KMOD_CTRL) ? 1u : 0u) | ((sdl_mods & SDL_KMOD_SHIFT) ? 2u : 0u) |
            ((sdl_mods & SDL_KMOD_ALT) ? 4u : 0u) | ((sdl_mods & SDL_KMOD_GUI) ? 8u : 0u));

        for (const auto& mb : pending.mouse_buttons) {
            if (mb.button >= ImGuiMouseButton_COUNT) continue;
            const auto bit = static_cast<std::uint8_t>(1u << mb.button);
            if (mb.down) {
                if (allow_input && !local_active) {
                    in.mouse_buttons.push_back(mb);
                    if (remote_->want_capture_mouse) remote_->captured_mouse_mask |= bit;
                    remote_->local_owned_mouse_mask &= static_cast<std::uint8_t>(~bit);
                } else {
                    remote_->local_owned_mouse_mask |= bit;
                }
            } else {
                // Always forward releases (unless owned purely by a local interaction) even if allow_input just
                // flipped false or local_active is true, so remote drags and clicks always complete cleanly.
                remote_->captured_mouse_mask &= static_cast<std::uint8_t>(~bit);
                const bool was_local = (remote_->local_owned_mouse_mask & bit) != 0;
                remote_->local_owned_mouse_mask &= static_cast<std::uint8_t>(~bit);
                if (!was_local) in.mouse_buttons.push_back(mb);
            }
        }
        if (allow_input && !local_active) {
            in.wheel_x = pending.wheel_x;
            in.wheel_y = pending.wheel_y;
        }
        for (const auto& ke : pending.keys) {
            if (!ke.down) {
                in.keys.push_back(ke);
            } else if (allow_input && (remote_->want_capture_keyboard || remote_->want_capture_mouse)) {
                in.keys.push_back(ke);
            }
        }
        if (allow_input && (remote_->want_capture_keyboard || remote_->want_text_input)) {
            in.chars = std::move(pending.chars);
        }

        const auto& win_in = window_.input();
        if (!pending.saw_mouse_pos) {
            // Immediately after swapping characters the cursor may not have moved yet; seed from ImGui's or SDL's
            // current window-space mouse position so hovering and clicking remote plugin/Lua windows works at once.
            if (io.MousePos.x > -FLT_MAX * 0.5f && io.MousePos.y > -FLT_MAX * 0.5f) {
                remote_->mouse_x = io.MousePos.x;
                remote_->mouse_y = io.MousePos.y;
                remote_->mouse_valid = true;
            } else if (remote_->focused) {
                remote_->mouse_x = win_in.mouse_x;
                remote_->mouse_y = win_in.mouse_y;
                remote_->mouse_valid = true;
            }
        }
        // Cross-check held buttons against live Window/SDL state so even an OS-swallowed mouse-up (e.g. capture
        // loss outside the window) clears held state and reconciles the headless context on the next frame.
        float live_mx = 0.0f, live_my = 0.0f;
        const SDL_MouseButtonFlags live_sdl = SDL_GetMouseState(&live_mx, &live_my);
        const std::uint8_t live_physical = static_cast<std::uint8_t>(
            (win_in.left_button ? 1u : 0u) | (win_in.right_button ? 2u : 0u) |
            ((live_sdl & SDL_BUTTON_MMASK) ? 4u : 0u) | ((live_sdl & SDL_BUTTON_X1MASK) ? 8u : 0u) |
            ((live_sdl & SDL_BUTTON_X2MASK) ? 16u : 0u));
        remote_->sdl_mouse_down_mask &= live_physical;
        const std::uint8_t held_any =
            static_cast<std::uint8_t>(remote_->sdl_mouse_down_mask | remote_->pad_mouse_down_mask);
        remote_->captured_mouse_mask &= held_any;
        remote_->local_owned_mouse_mask &= held_any;
        if (!allow_input) remote_->captured_mouse_mask = 0;

        in.mouse_x = remote_->mouse_x;
        in.mouse_y = remote_->mouse_y;
        in.mouse_valid = allow_input && remote_->mouse_valid;
        in.focused = allow_input && remote_->focused;
        in.mods = allow_input ? remote_->mods : 0u;
        in.mouse_down_mask = allow_input ? static_cast<std::uint8_t>(held_any & ~remote_->local_owned_mouse_mask) : 0u;
        in.has_mouse_down_mask = true;
        remote_->captured_input = std::move(in);

        // Remote plugin windows draw over local Hailstone UI: prevent clicks/scrolls/keys meant for remote windows
        // from falling through to local windows behind them, while still allowing releases so nothing stays held.
        const bool remote_owns_mouse = remote_->want_capture_mouse || remote_->captured_mouse_mask != 0;
        const bool eat_mouse = allow_input && remote_owns_mouse && !local_active;
        const bool eat_kbd = allow_input && remote_->want_capture_keyboard;
        if (eat_mouse || eat_kbd) {
            ImVector<ImGuiInputEvent>& q = GImGui->InputEventsQueue;
            for (int i = 0; i < q.Size;) {
                const ImGuiInputEvent& e = q[i];
                const bool drop_mouse =
                    eat_mouse &&
                    ((e.Type == ImGuiInputEventType_MouseButton && e.MouseButton.Down) ||
                     e.Type == ImGuiInputEventType_MouseWheel);
                const bool drop_kbd =
                    eat_kbd &&
                    ((e.Type == ImGuiInputEventType_Key && e.Key.Down && e.Key.Key != ImGuiMod_Ctrl &&
                      e.Key.Key != ImGuiMod_Shift && e.Key.Key != ImGuiMod_Alt && e.Key.Key != ImGuiMod_Super) ||
                     e.Type == ImGuiInputEventType_Text);
                if (drop_mouse || drop_kbd) q.erase(q.Data + i);
                else ++i;
            }
        }
    }
    if (mouse_locked_) {
        // set_mouse_locked: this frame's presses and wheel turns are dropped before ImGui sees them.
        ImVector<ImGuiInputEvent>& q = GImGui->InputEventsQueue;
        for (int i = 0; i < q.Size;) {
            const ImGuiInputEvent& e = q[i];
            if ((e.Type == ImGuiInputEventType_MouseButton && e.MouseButton.Down) || e.Type == ImGuiInputEventType_MouseWheel)
                q.erase(q.Data + i);
            else
                ++i;
        }
    }
    visibility_.prepare_input();
    keyboard_request_ = GImGui->WantCaptureKeyboardNextFrame; // (NewFrame applies it to io, then clears it)
    keep_tab_from_windows();
    ImGui::NewFrame();
    visibility_.prepare_input();
    frame_open_ = true;
    // The pad's X (ImGui's "menu" key) is the windows' own shortcut (use, take, none, ...; the Lua windows read it from
    // hs.gamepad). Held, ImGui would start its window switcher (a list of every window, the focus moved on release)
    // and, tapped, toggle the menu layer. Locked to an owner (from the frame it goes down until it is let go), ImGui's
    // navigation does not see it; the on-screen keyboard asks for it with that owner.
    ImGui::SetKeyOwner(ImGuiKey_GamepadFaceLeft, ImHashStr("##PadX"), ImGuiInputFlags_LockUntilRelease);
    if (!focus_request_.empty()) {
        if (ImGui::FindWindowByName(focus_request_.c_str())) {
            ImGui::SetWindowFocus(focus_request_.c_str());
            ImGui::SetNavCursorVisible(true);
            focus_request_.clear();
        } else if (++focus_frames_ > 120) {
            log::debug("gamepad: no window '{}' took the focus", focus_request_);
            focus_request_.clear();
        }
    }
}

void ImGuiLayer::apply_pad_navigation() {
    ImGuiIO& io = ImGui::GetIO();
    // Called before the backend's frame: a pad the window closed (unplugged) is taken back from it first.
    SDL_Gamepad* pad = pad_nav_ ? window_.sdl_gamepad() : nullptr;
    if (pad != nav_pad_) {
        ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, pad ? &pad : nullptr, pad ? 1 : 0);
        nav_pad_ = pad;
    }
    if (pad_nav_ == pad_nav_applied_) return;
    pad_nav_applied_ = pad_nav_;
    if (pad_nav_) {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        return;
    }
    // Off: the backend stops updating the pad's keys, so whatever was held is let go here.
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    for (int k = ImGuiKey_GamepadStart; k <= ImGuiKey_GamepadRStickDown; ++k)
        io.AddKeyAnalogEvent(static_cast<ImGuiKey>(k), false, 0.0f);
}

void ImGuiLayer::feed_pad_pointer() {
    ImGuiIO& io = ImGui::GetIO();
    const platform::Window::PadPointer& p = window_.pad_pointer();
    const platform::Window::FastPointer& fast = window_.fast_pointer();
    // ImGui draws the pointer itself (the backend hides the system's), so it shows where the pad put it, or the
    // window's own cursor (Window::set_pointer_speed) is.
    io.MouseDrawCursor = p.active || fast.active;
    if (fast.active && !p.active) {
        io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
        io.AddMousePosEvent(fast.x, fast.y);
        if (remote_ && remote_->enabled) {
            remote_->mouse_x = fast.x;
            remote_->mouse_y = fast.y;
            remote_->mouse_valid = true;
            remote_->pending.saw_mouse_pos = true;
        }
    }
    if (p.active) {
        // After the backend's frame: its own place for the mouse (the system's) comes first and this one wins.
        io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
        io.AddMousePosEvent(p.x, p.y);
        if (p.wheel != 0.0f) io.AddMouseWheelEvent(0.0f, p.wheel);
        if (remote_ && remote_->enabled) {
            remote_->mouse_x = p.x;
            remote_->mouse_y = p.y;
            remote_->mouse_valid = true;
            remote_->pending.saw_mouse_pos = true;
            remote_->pending.wheel_y += p.wheel;
        }
    }
    const bool left = p.active && p.left, right = p.active && p.right;
    if (left != pointer_left_) {
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, left);
        if (remote_ && remote_->enabled) {
            if (left) remote_->pad_mouse_down_mask |= 1u;
            else remote_->pad_mouse_down_mask &= static_cast<std::uint8_t>(~1u);
            remote_->pending.mouse_buttons.push_back({0u, left});
        }
    }
    if (right != pointer_right_) {
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, right);
        if (remote_ && remote_->enabled) {
            if (right) remote_->pad_mouse_down_mask |= 2u;
            else remote_->pad_mouse_down_mask &= static_cast<std::uint8_t>(~2u);
            remote_->pending.mouse_buttons.push_back({1u, right});
        }
    }
    pointer_left_ = left;
    pointer_right_ = right;
}

void ImGuiLayer::request_window_focus(std::string name) {
    if (!visible()) return;
    std::erase(pad_windows_, name);
    pad_windows_.push_back(name);
    focus_request_ = std::move(name);
    focus_frames_ = 0;
}

bool ImGuiLayer::pad_window_open() {
    if (!visible()) return false;
    // Between frames: a window drawn in the last frame is Active (its Begin ran) until the next NewFrame.
    const std::string top = pad_windows_.empty() ? std::string() : pad_windows_.back();
    std::erase_if(pad_windows_, [](const std::string& name) {
        const ImGuiWindow* w = ImGui::FindWindowByName(name.c_str());
        return !w || !(w->Active || w->WasActive);
    });
    // The window on top closed (a Tip over the merchant): the pad window under it takes the focus at once, so the
    // pad's next button goes to it rather than only giving it the focus back.
    if (!pad_windows_.empty() && pad_windows_.back() != top && focus_request_.empty())
        request_window_focus(pad_windows_.back());
    return !pad_windows_.empty();
}

bool detail::pad_window_has_focus(const char* name) {
    const ImGuiContext* g = ImGui::GetCurrentContext();
    if (!g || !g->NavWindow) return false;
    ImGuiWindow* tracked = ImGui::FindWindowByName(name);
    return tracked && ImGui::IsWindowChildOf(g->NavWindow, tracked, true);
}

bool ImGuiLayer::pad_window_focused() const {
    return visible() && !pad_windows_.empty() && detail::pad_window_has_focus(pad_windows_.back().c_str());
}

std::string ImGuiLayer::pad_focus_window() const {
    if (!visible()) return {};
    if (!focus_request_.empty()) return focus_request_;
    const ImGuiContext* g = ImGui::GetCurrentContext();
    return g && g->NavWindow ? std::string(g->NavWindow->RootWindow->Name) : std::string();
}

void ImGuiLayer::refocus_pad_window() {
    if (!visible()) return;
    if (!pad_windows_.empty() && focus_request_.empty()) request_window_focus(pad_windows_.back());
}

std::string ImGuiLayer::debug_state() const {
    const ImGuiContext* g = ImGui::GetCurrentContext();
    if (!g) return "no context";
    const ImGuiIO& io = ImGui::GetIO();
    char buf[768];
    std::snprintf(buf, sizeof buf,
                  "nav window '%s', nav id %08x, active id %08x, nav cursor %d, nav active %d, nav visible %d, "
                  "pad nav %d, has gamepad %d, wants mouse %d, wants keyboard %d, text input %d, dpad down %d (owner "
                  "%08x), nav move dir %d, nav init %d, nav layer %d, items in nav window %d",
                  g->NavWindow ? g->NavWindow->Name : "", g->NavId, g->ActiveId, g->NavCursorVisible ? 1 : 0,
                  io.NavActive ? 1 : 0, io.NavVisible ? 1 : 0,
                  (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) ? 1 : 0,
                  (io.BackendFlags & ImGuiBackendFlags_HasGamepad) ? 1 : 0, io.WantCaptureMouse ? 1 : 0,
                  io.WantCaptureKeyboard ? 1 : 0, io.WantTextInput ? 1 : 0, ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown) ? 1 : 0,
                  ImGui::GetKeyOwner(ImGuiKey_GamepadDpadDown), static_cast<int>(g->NavMoveDir),
                  g->NavInitRequest ? 1 : 0, static_cast<int>(g->NavLayer),
                  g->NavWindow ? g->NavWindow->DC.NavLayersActiveMask : -1);
    std::string out = buf;
    out += "; pad windows";
    for (const auto& w : pad_windows_) out += " '" + w + "'";
    return out;
}

void ImGuiLayer::render(const render::FrameInfo& frame) {
    vk_.begin_overlay(frame);
    if (!frame_open_) return;
    if (visible() && remote_ && remote_->enabled &&
        (remote_->want_capture_mouse || remote_->captured_mouse_mask != 0) &&
        remote_->mouse_cursor >= 0 && remote_->mouse_cursor < ImGuiMouseCursor_COUNT) {
        ImGui::SetMouseCursor(static_cast<ImGuiMouseCursor>(remote_->mouse_cursor));
    }
    const bool draw = visibility_.finish_frame(true);
    frame_open_ = false;
    if (!draw) return;
    ImDrawData* data = ImGui::GetDrawData();
    if (data->DisplaySize.x <= 0.0f || data->DisplaySize.y <= 0.0f) return;
    if (remote_) remote_->append_to_draw_data(data);
    // The backend sets its own viewport and scissor, replacing the frame's Y-flipped viewport.
    ImGui_ImplVulkan_RenderDrawData(data, frame.cmd);
}

void ImGuiLayer::set_remote_enabled(bool enabled) {
    if (!remote_) return;
    if (remote_->enabled == enabled) return;
    remote_->enabled = enabled;
    remote_->clear_session();
}

void ImGuiLayer::reset_interaction_on_next_frame() { reset_interaction_pending_ = true; }

void ImGuiLayer::push_remote_texture(RemoteImGuiTexture tex) {
    if (!remote_ || !remote_->enabled) return;
    if (remote_->pending_uploads.size() < 128) remote_->pending_uploads.push_back(std::move(tex));
}

void ImGuiLayer::set_remote_frame(RemoteImGuiFrame frame) {
    if (!remote_ || !remote_->enabled) return;
    remote_->want_capture_mouse = frame.want_capture_mouse;
    remote_->want_capture_keyboard = frame.want_capture_keyboard;
    remote_->want_text_input = frame.want_text_input;
    remote_->mouse_cursor = frame.mouse_cursor;
    remote_->frame = std::move(frame);
}

void ImGuiLayer::record_remote_uploads(const render::FrameInfo& frame) {
    if (remote_) remote_->record_uploads(frame);
}

std::optional<RemoteImGuiInput> ImGuiLayer::take_remote_input() {
    if (!remote_ || !remote_->enabled) return std::nullopt;
    return std::exchange(remote_->captured_input, std::nullopt);
}

bool ImGuiLayer::remote_wants_text_input() const {
    return visible() && remote_ && remote_->enabled && remote_->want_text_input;
}

void ImGuiLayer::end_frame_without_render() {
    if (!frame_open_) return;
    (void)visibility_.finish_frame(false);
    frame_open_ = false;
}

void ImGuiLayer::set_visible(bool visible) {
    const bool changed = visible != visibility_.visible();
    visibility_.set_visible(visible);
    if (changed) {
        keyboard_request_ = -1;
        focus_request_.clear();
        focus_frames_ = 0;
        pad_windows_.clear();
    }
}
bool ImGuiLayer::wants_mouse() const {
    return visibility_.wants_mouse() ||
           (visible() && remote_ && remote_->enabled &&
            (remote_->want_capture_mouse || remote_->captured_mouse_mask != 0));
}
bool ImGuiLayer::wants_keyboard() const {
    return visibility_.wants_keyboard(keyboard_request_) ||
           (visible() && remote_ && remote_->enabled && remote_->want_capture_keyboard);
}

void configure_keys() { ImGui::GetIO().ConfigMacOSXBehaviors = false; }

void keep_tab_from_windows() {
    // ImGui's tabbing is always on (NavUpdateCreateTabbingRequest, whatever ImGuiConfigFlags_NavEnableKeyboard says)
    // and, with no item active, a Tab focuses the first field of the focused window: the chat's input took the keys
    // from a Tab meant for the game. Owned for the frame, the press is not ImGui's navigation's (it tests for no
    // owner), while ImGui::IsKeyPressed (any owner: a browser page's keys) still sees it. A field being typed in
    // leaves Tab alone, so it still moves to the next field.
    if (GImGui->ActiveId == 0) ImGui::SetKeyOwner(ImGuiKey_Tab, ImHashStr("##GameTab"));
}

bool keyboard_for_ui(int requested) {
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureKeyboard) return false;
    if (requested == 1 || io.WantTextInput || ImGui::GetTopMostPopupModal()) return true;
    const ImGuiContext& g = *GImGui;
    if (g.ActiveId != 0) return g.ActiveIdSource != ImGuiInputSource_Mouse;
    // NewFrame computes capture before clearing a window's move ID on mouse release. With no active item,
    // only enabled keyboard navigation can account for the remaining capture flag.
    return io.NavActive && (io.ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard) && io.ConfigNavCaptureKeyboard;
}

float ui_scale() { return g_ui_scale; }
void set_ui_scale(float scale) { g_ui_scale = scale; }

ImFont* chat_font(int index) {
    return index >= 0 && index < static_cast<int>(kChatFonts) ? g_chat_fonts[static_cast<std::size_t>(index)] : nullptr;
}

float chat_font_size(int index) {
    return index >= 0 && index < static_cast<int>(kChatFonts) ? kChatFontSizes[static_cast<std::size_t>(index)] : 0.0f;
}

ImFont* plate_font() { return g_plate_font; }
float plate_font_size() { return g_plate_font_size; }
bool plate_font_faux_bold() { return g_plate_faux_bold; }

ImFont* ui_font(int index) {
    return index >= 1 && index < static_cast<int>(kUiFontSizes.size()) ? g_ui_fonts[static_cast<std::size_t>(index)]
                                                                         : nullptr;
}

} // namespace hailstone::ui
