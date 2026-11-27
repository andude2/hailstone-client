#include "app/headless_app.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#include <imgui.h>
#include <imgui_internal.h>

#include "app/automation.h"
#include "app/actions_ui.h"
#include "app/ability_bulk.h"
#include "app/drive_control.h"
#include "app/drive_observe.h"
#include "app/lua_ui.h"
#include "core/config.h"
#include "app/navigator.h"
#include "app/player_movement.h"
#include "app/zone_move_world.h"
#include "assets/zone.h"
#include "core/character_files.h"
#include "core/game_dir.h"
#include "core/ini_file.h"
#include "core/server_profile.h"
#include "core/log.h"
#include "core/step_clock.h"
#include "core/wipe.h"
#include "game/collision.h"
#include "game/coords.h"
#include "game/mechanics/character.h"
#include "game/mechanics/collision_movement.h"
#include "game/mechanics/sheet_builder.h"
#include "game/mechanics/tables.h"
#include "game/mounts.h"
#include "game/position_tracker.h"
#include "game/zone_lines.h"
#include "game/zoning.h"
#include "nav/zone_nav.h"
#include "net/game_connection.h"
#include "net/login.h"
#include "platform/child_processes.h"
#include "platform/credentials.h"
#include "platform/paths.h"
#include "platform/private_file.h"
#include "ui/imgui_layer.h"
#include "ui/remote_imgui.h"

namespace hailstone::app {
namespace {

volatile std::sig_atomic_t stop_requested = 0;
void on_stop_signal(int) { stop_requested = 1; }
using Clock = std::chrono::steady_clock;

void stub_once(const char* what) {
    static bool face_logged = false, key_logged = false, window_logged = false;
    bool* flag = std::strcmp(what, "face") == 0 ? &face_logged : std::strcmp(what, "keypress") == 0 ? &key_logged : &window_logged;
    if (*flag) return;
    *flag = true;
    log::warn("headless: {} is not wired yet (docs/headless.md); automation that needs it will see it unavailable", what);
}

// CPU-only Dear ImGui context used when a headless character serves a drive pipe (--drive-observe / --drive-control).
// Created before Automation so plugins can cache the context pointer; NewFrame/Render run only while a GUI viewer is
// attached, streaming ImDrawData and on-demand RGBA32 font atlas updates over the drive pipe.
class HeadlessImGui {
public:
    explicit HeadlessImGui(const std::filesystem::path& ini_file) {
        IMGUI_CHECKVERSION();
        ctx_ = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx_);
        ImGuiIO& io = ImGui::GetIO();
        const std::u8string u8 = ini_file.u8string();
        ini_path_.assign(reinterpret_cast<const char*>(u8.data()), u8.size());
        io.IniFilename = ini_path_.empty() ? nullptr : ini_path_.c_str();
        io.IniSavingRate = 1.0f;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
        pio.Renderer_TextureMaxWidth = 4096;
        pio.Renderer_TextureMaxHeight = 4096;
        io.ConfigErrorRecovery = true;
        io.ConfigErrorRecoveryEnableAssert = false;
        io.ConfigErrorRecoveryEnableTooltip = true;
        io.DisplaySize = ImVec2(display_w_, display_h_);
        io.DisplayFramebufferScale = ImVec2(fb_scale_x_, fb_scale_y_);
        io.DeltaTime = 1.0f / 60.0f;
        ui::configure_keys();
        apply_scale(1.0f);
    }

    ~HeadlessImGui() {
        flush_ini();
        if (ctx_) {
            ImGui::DestroyContext(ctx_);
            ctx_ = nullptr;
        }
    }

    HeadlessImGui(const HeadlessImGui&) = delete;
    HeadlessImGui& operator=(const HeadlessImGui&) = delete;

    void flush_ini() {
        if (!ctx_) return;
        ImGui::SetCurrentContext(ctx_);
        ImGuiIO& io = ImGui::GetIO();
        if (GImGui && GImGui->SettingsLoaded && io.IniFilename && io.IniFilename[0] != '\0' &&
            GImGui->SettingsDirtyTimer > 0.0f) {
            ImGui::SaveIniSettingsToDisk(io.IniFilename);
            GImGui->SettingsDirtyTimer = 0.0f;
        }
    }

    void reset_input() {
        if (!ctx_) return;
        ImGui::SetCurrentContext(ctx_);
        ImGuiIO& io = ImGui::GetIO();
        io.ClearEventsQueue();
        io.ClearInputKeys();
        io.ClearInputMouse();
        ImGui::ClearActiveID();
        ImGui::ClearDragDrop();
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        io.AddFocusEvent(false);
        last_focused_ = false;
        last_mods_ = 0;
    }

    void render_frame(Automation& automation, DriveObserveServer& observe,
                      const std::optional<ui::RemoteImGuiInput>& input, bool interactive, bool& send_full_atlas) {
        if (!ctx_) return;
        ImGui::SetCurrentContext(ctx_);
        ImGuiIO& io = ImGui::GetIO();

        if (input) {
            if (input->display_w >= 64.0f && input->display_h >= 64.0f) {
                display_w_ = input->display_w;
                display_h_ = input->display_h;
                has_display_size_ = true;
            }
            if (input->fb_scale_x > 0.0f && input->fb_scale_y > 0.0f) {
                fb_scale_x_ = input->fb_scale_x;
                fb_scale_y_ = input->fb_scale_y;
            }
            const float wanted_scale = std::clamp(input->ui_scale, 0.5f, 3.0f);
            if (std::fabs(wanted_scale - scale_) > 0.01f) {
                apply_scale(wanted_scale);
                send_full_atlas = true;
            }
            io.DeltaTime = std::clamp(input->dt, 1.0e-4f, 0.25f);
            if (interactive) {
                if (input->focused != last_focused_) {
                    io.AddFocusEvent(input->focused);
                    last_focused_ = input->focused;
                }
                const std::uint8_t changed_mods = static_cast<std::uint8_t>(input->mods ^ last_mods_);
                if (changed_mods & 1u) io.AddKeyEvent(ImGuiMod_Ctrl, (input->mods & 1u) != 0);
                if (changed_mods & 2u) io.AddKeyEvent(ImGuiMod_Shift, (input->mods & 2u) != 0);
                if (changed_mods & 4u) io.AddKeyEvent(ImGuiMod_Alt, (input->mods & 4u) != 0);
                if (changed_mods & 8u) io.AddKeyEvent(ImGuiMod_Super, (input->mods & 8u) != 0);
                last_mods_ = input->mods;
                if (input->mouse_valid) io.AddMousePosEvent(input->mouse_x, input->mouse_y);
                else io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
                std::array<bool, ImGuiMouseButton_COUNT> tracked_down{};
                for (int b = 0; b < ImGuiMouseButton_COUNT; ++b)
                    tracked_down[static_cast<std::size_t>(b)] = io.MouseDown[b];
                for (int i = 0; i < GImGui->InputEventsQueue.Size; ++i) {
                    const ImGuiInputEvent& e = GImGui->InputEventsQueue[i];
                    if (e.Type == ImGuiInputEventType_MouseButton && e.MouseButton.Button >= 0 &&
                        e.MouseButton.Button < ImGuiMouseButton_COUNT) {
                        tracked_down[static_cast<std::size_t>(e.MouseButton.Button)] = e.MouseButton.Down;
                    }
                }
                for (const auto& mb : input->mouse_buttons) {
                    if (mb.button < ImGuiMouseButton_COUNT) {
                        io.AddMouseButtonEvent(mb.button, mb.down);
                        tracked_down[mb.button] = mb.down;
                    }
                }
                if (input->has_mouse_down_mask) {
                    // Reconcile any missed mouse-up against the viewer's authoritative held-button mask so a
                    // dragged remote window or widget can never stay stuck held.
                    for (int b = 0; b < ImGuiMouseButton_COUNT; ++b) {
                        const bool held = ((input->mouse_down_mask >> b) & 1u) != 0;
                        if (!held && tracked_down[static_cast<std::size_t>(b)]) io.AddMouseButtonEvent(b, false);
                    }
                }
                if (input->wheel_x != 0.0f || input->wheel_y != 0.0f)
                    io.AddMouseWheelEvent(input->wheel_x, input->wheel_y);
                for (const auto& ke : input->keys) {
                    if (ke.key >= ImGuiKey_NamedKey_BEGIN && ke.key < ImGuiKey_NamedKey_END)
                        io.AddKeyEvent(static_cast<ImGuiKey>(ke.key), ke.down);
                }
                for (std::uint32_t c : input->chars) io.AddInputCharacter(c);
            }
        } else {
            io.DeltaTime = 1.0f / 30.0f;
        }
        // Do not run the first ImGui::NewFrame() until the viewer has reported its real viewport size; otherwise Dear
        // ImGui loads the saved .ini against the 1280x720 placeholder, clamps high-res window positions/sizes inward,
        // and overwrites the saved layout.
        if (!has_display_size_) return;

        ImDrawData* dd = nullptr;
        int kbd_req = -1;
        // Dear ImGui trickles coalesced same-button down+up edges across frames. Pump remaining queued events
        // immediately (bounded to 4 passes) so fast clicks complete on the same tick without backlogging.
        for (int pass = 0; pass < 4; ++pass) {
            if (pass > 0) io.DeltaTime = 1.0e-3f;
            io.DisplaySize = ImVec2(display_w_, display_h_);
            io.DisplayFramebufferScale = ImVec2(fb_scale_x_, fb_scale_y_);
            kbd_req = GImGui->WantCaptureKeyboardNextFrame;
            ui::keep_tab_from_windows();
            ImGui::NewFrame();
            automation.imgui(false);
            ImGui::Render();

            dd = ImGui::GetDrawData();
            if (dd && dd->Textures) {
                for (ImTextureData* tex : *dd->Textures) {
                    if (!tex) continue;
                    const auto id = static_cast<std::uint32_t>(tex->UniqueID);
                    if (tex->Status == ImTextureStatus_WantCreate) {
                        tex->SetTexID(static_cast<ImTextureID>(id));
                        send_texture_rect(observe, tex, 0, 0, static_cast<std::uint16_t>(tex->Width),
                                          static_cast<std::uint16_t>(tex->Height), true);
                        tex->SetStatus(ImTextureStatus_OK);
                    } else if (tex->Status == ImTextureStatus_WantUpdates) {
                        if (tex->TexID == ImTextureID_Invalid) tex->SetTexID(static_cast<ImTextureID>(id));
                        if (send_full_atlas) {
                            send_texture_rect(observe, tex, 0, 0, static_cast<std::uint16_t>(tex->Width),
                                              static_cast<std::uint16_t>(tex->Height), true);
                        } else if (tex->UpdateRect.w > 0 && tex->UpdateRect.h > 0) {
                            send_texture_rect(observe, tex, tex->UpdateRect.x, tex->UpdateRect.y, tex->UpdateRect.w,
                                              tex->UpdateRect.h, false);
                        }
                        tex->SetStatus(ImTextureStatus_OK);
                    } else if (tex->Status == ImTextureStatus_WantDestroy) {
                        ui::RemoteImGuiTexture msg;
                        msg.id = id;
                        msg.destroy = true;
                        observe.publish_imgui_texture(msg);
                        tex->SetTexID(ImTextureID_Invalid);
                        tex->SetStatus(ImTextureStatus_Destroyed);
                    } else if (tex->Status == ImTextureStatus_OK && send_full_atlas && tex->Pixels && tex->Width > 0 &&
                               tex->Height > 0) {
                        if (tex->TexID == ImTextureID_Invalid) tex->SetTexID(static_cast<ImTextureID>(id));
                        send_texture_rect(observe, tex, 0, 0, static_cast<std::uint16_t>(tex->Width),
                                          static_cast<std::uint16_t>(tex->Height), true);
                    }
                }
            }
            send_full_atlas = false;
            if (GImGui->InputEventsQueue.empty()) break;
        }
        if (!dd) return;

        static_assert(sizeof(ImDrawIdx) == sizeof(std::uint16_t));
        ui::RemoteImGuiFrame frame;
        frame.display_w = dd->DisplaySize.x;
        frame.display_h = dd->DisplaySize.y;
        frame.fb_scale_x = dd->FramebufferScale.x;
        frame.fb_scale_y = dd->FramebufferScale.y;
        frame.want_capture_mouse = io.WantCaptureMouse;
        frame.want_capture_keyboard = ui::keyboard_for_ui(kbd_req);
        frame.want_text_input = io.WantTextInput;
        frame.mouse_cursor = static_cast<std::int8_t>(ImGui::GetMouseCursor());
        frame.lists.reserve(static_cast<std::size_t>(dd->CmdLists.Size));
        for (const ImDrawList* src : dd->CmdLists) {
            if (!src || src->CmdBuffer.empty() || src->VtxBuffer.empty() || src->IdxBuffer.empty()) continue;
            ui::RemoteDrawList dl;
            dl.vertices.resize(static_cast<std::size_t>(src->VtxBuffer.Size));
            for (int i = 0; i < src->VtxBuffer.Size; ++i) {
                const ImDrawVert& v = src->VtxBuffer[i];
                dl.vertices[static_cast<std::size_t>(i)] = {v.pos.x, v.pos.y, v.uv.x, v.uv.y, v.col};
            }
            dl.indices.assign(src->IdxBuffer.Data, src->IdxBuffer.Data + src->IdxBuffer.Size);
            dl.cmds.reserve(static_cast<std::size_t>(src->CmdBuffer.Size));
            for (int i = 0; i < src->CmdBuffer.Size; ++i) {
                const ImDrawCmd& c = src->CmdBuffer[i];
                if (c.UserCallback != nullptr || c.ElemCount == 0) continue;
                dl.cmds.push_back({c.ClipRect.x, c.ClipRect.y, c.ClipRect.z, c.ClipRect.w,
                                   static_cast<std::uint32_t>(c.GetTexID()), c.VtxOffset, c.IdxOffset, c.ElemCount});
            }
            if (!dl.cmds.empty()) frame.lists.push_back(std::move(dl));
        }
        observe.publish_imgui_frame(frame);
    }

private:
    void apply_scale(float scale) {
        scale_ = scale;
        ImGuiStyle& style = ImGui::GetStyle();
        style = ImGuiStyle();
        ImGui::StyleColorsDark(&style);
        style.WindowRounding = 4.0f;
        style.FrameRounding = 3.0f;
        style.ScaleAllSizes(scale_);

        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();
        const ImFontConfig cfg = ui::detail::ui_font_config();
        static constexpr std::array kFontCandidates = {
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
        std::filesystem::path found;
        std::error_code ec;
        for (const char* p : kFontCandidates) {
            if (std::filesystem::is_regular_file(p, ec)) {
                found = p;
                break;
            }
        }
        ImFont* loaded = nullptr;
        if (!found.empty()) {
            const std::u8string u8 = found.u8string();
            const std::string utf8_path(reinterpret_cast<const char*>(u8.data()), u8.size());
            loaded = io.Fonts->AddFontFromFileTTF(utf8_path.c_str(), std::round(15.0f * scale_), &cfg);
        }
        if (!loaded || !ui::detail::build_ui_font_atlas(*io.Fonts)) {
            io.Fonts->Clear();
            ImFontConfig def = cfg;
            def.SizePixels = std::round(13.0f * scale_);
            io.Fonts->AddFontDefault(&def);
            ui::detail::build_ui_font_atlas(*io.Fonts);
        }
    }

    static void send_texture_rect(DriveObserveServer& observe, const ImTextureData* tex, std::uint16_t rx,
                                  std::uint16_t ry, std::uint16_t rw, std::uint16_t rh, bool full) {
        if (!tex || !tex->Pixels || rw == 0 || rh == 0 || tex->Width <= 0 || tex->Height <= 0) return;
        const auto tw = static_cast<std::uint16_t>(tex->Width);
        const auto th = static_cast<std::uint16_t>(tex->Height);
        if (static_cast<std::uint32_t>(rx) + rw > tw || static_cast<std::uint32_t>(ry) + rh > th) return;
        // Split large uploads into <= 4 MiB horizontal bands so every pipe message stays well under kLocalPipeMaxMessage.
        // Keep max_rows clamped to rh and iterate in size_t so narrow glyph rects (e.g. rw = 1, 2, 4, 8, 16) cannot
        // truncate max_rows to 0 in uint16_t or wrap y_off back below rh.
        const std::size_t row_bytes = static_cast<std::size_t>(rw) * 4u;
        const std::size_t max_rows = ui::remote_texture_band_rows(rw, rh);
        if (max_rows == 0) return;
        bool first_band = full;
        for (std::size_t y_off = 0; y_off < rh; y_off += max_rows) {
            const auto bh = static_cast<std::uint16_t>(std::min<std::size_t>(max_rows, rh - y_off));
            ui::RemoteImGuiTexture msg;
            msg.id = static_cast<std::uint32_t>(tex->UniqueID);
            msg.tex_w = tw;
            msg.tex_h = th;
            msg.x = rx;
            msg.y = static_cast<std::uint16_t>(ry + y_off);
            msg.w = rw;
            msg.h = bh;
            msg.full = first_band;
            first_band = false;
            msg.rgba.resize(static_cast<std::size_t>(rw) * bh * 4u);
            if (tex->BytesPerPixel == 4) {
                const std::size_t src_stride = static_cast<std::size_t>(tw) * 4u;
                for (std::uint16_t y = 0; y < bh; ++y) {
                    const auto* src = tex->Pixels + (static_cast<std::size_t>(msg.y) + y) * src_stride +
                                      static_cast<std::size_t>(rx) * 4u;
                    std::memcpy(msg.rgba.data() + static_cast<std::size_t>(y) * row_bytes, src, row_bytes);
                }
            } else if (tex->BytesPerPixel == 1) {
                for (std::uint16_t y = 0; y < bh; ++y) {
                    const auto* src = tex->Pixels + (static_cast<std::size_t>(msg.y) + y) * tw + rx;
                    auto* dst = reinterpret_cast<std::uint8_t*>(msg.rgba.data() + static_cast<std::size_t>(y) * row_bytes);
                    for (std::uint16_t x = 0; x < rw; ++x) {
                        dst[x * 4 + 0] = 255;
                        dst[x * 4 + 1] = 255;
                        dst[x * 4 + 2] = 255;
                        dst[x * 4 + 3] = src[x];
                    }
                }
            } else {
                return;
            }
            observe.publish_imgui_texture(msg);
        }
    }

    ImGuiContext* ctx_ = nullptr;
    std::string ini_path_;
    float scale_ = 1.0f;
    float display_w_ = 1280.0f;
    float display_h_ = 720.0f;
    bool has_display_size_ = false;
    float fb_scale_x_ = 1.0f;
    float fb_scale_y_ = 1.0f;
    bool last_focused_ = false;
    std::uint8_t last_mods_ = 0;
};

} // namespace

HeadlessArgs parse_headless_args(const std::vector<std::string_view>& args) {
    HeadlessArgs out;
    if (const char* gd = std::getenv("EQ_GAME_DIR")) out.options.game_dir = gd;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto need = [&](std::string_view name) -> std::string_view {
            if (i + 1 >= args.size()) {
                out.error = std::string(name) + " needs a value";
                return {};
            }
            return args[++i];
        };
        const std::string_view a = args[i];
        if (a == "--game-dir") {
            out.options.game_dir = need(a);
            out.options.game_dir_given = true;
        }
        else if (a == "--data-dir") out.options.data_dir = need(a);
        else if (a == "--host") {
            std::string_view h = need(a);
            if (auto c = h.rfind(':'); c != std::string_view::npos) {
                out.options.port = static_cast<std::uint16_t>(std::atoi(std::string(h.substr(c + 1)).c_str()));
                h = h.substr(0, c);
            }
            out.options.host = h;
        } else if (a == "--port") out.options.port = static_cast<std::uint16_t>(std::atoi(std::string(need(a)).c_str()));
        else if (a == "--user") out.options.user = need(a);
        else if (a == "--password") out.options.password = need(a);
        else if (a == "--character") out.options.character = need(a);
        else if (a == "--world") out.options.world = need(a);
        else if (a == "--server") out.options.server = need(a);
        else if (a == "--account") out.options.account = need(a);
        else if (a == "--crew") out.options.crew_file = need(a);
        else if (a == "--autoload") out.options.plugin_autoload = need(a);
        else if (a == "--no-chat-log") out.options.chat_log = false;
        else if (a == "--quiet") out.options.quiet = true;
        else if (a == "--list-servers") out.options.list_servers = true;
        else if (a == "--exec") out.options.exec.emplace_back(need(a));
        else if (a == "--exec-zone") out.options.exec_zone.emplace_back(need(a));
        else if (a == "--seconds") out.options.seconds = std::atoi(std::string(need(a)).c_str());
        else if (a == "--status-file") out.options.status_file = need(a);
        else if (a == "--no-collision") out.options.load_collision = false;
        else if (a == "--drive-observe") out.options.drive_observe = true;
        else if (a == "--drive-interact") out.options.drive_interact = true;
        else if (a == "--drive-control") out.options.drive_control = true;
        else if (a == "--drive-file") out.options.drive_file = need(a);
        else if (!out.error.empty()) break;
        else out.error = "unknown argument: " + std::string(a);
        if (!out.error.empty()) break;
    }
    if (out.error.empty() && out.options.drive_observe && !out.options.crew_file.empty())
        out.error = "--drive-observe is for a single character; a crew sets observe=1 in [crew]";
    if (out.error.empty() && out.options.drive_control && !out.options.crew_file.empty())
        out.error = "--drive-control is for a single character; a crew sets control=1 in [crew]";
    if (out.error.empty() && out.options.drive_observe && out.options.drive_control)
        out.error = "--drive-observe and --drive-control are exclusive";
    if (out.error.empty() && (out.options.drive_observe || out.options.drive_control) && out.options.drive_file.empty())
        out.error = "--drive-observe/--drive-control need --drive-file (the viewer reads its endpoint and token there)";
    if (out.error.empty() && out.options.drive_interact && !out.options.drive_observe)
        out.error = "--drive-interact needs --drive-observe";
    if (out.error.empty() && !out.options.drive_file.empty() && !out.options.drive_observe && !out.options.drive_control)
        out.error = "--drive-file needs --drive-observe or --drive-control";
    if (out.error.empty() && out.options.crew_file.empty() && out.options.account.empty()) {
        // A saved-account login (--account) resolves its host, user, password
        // and character from the profile at run time; a crew run likewise.
        if (out.options.host.empty()) out.error = "--host is required (or --account, or --crew)";
        else if (out.options.user.empty()) out.error = "--user is required";
        else if (out.options.password.empty()) out.error = "--password is required";
        else if (out.options.character.empty() && !out.options.list_servers) out.error = "--character is required";
        else if (out.options.game_dir.empty()) out.error = "--game-dir or EQ_GAME_DIR is required";
    }
    return out;
}

CrewFile parse_crew_file(std::string_view text) {
    CrewFile out;
    IniFile ini;
    ini.parse(text);
    out.data_dir = ini.get_string("crew", "data_dir");
    out.game_dir = ini.get_string("crew", "game_dir");
    if (auto a = ini.get("crew", "autoload")) out.autoload = *a;
    out.chat_log = ini.get_int("crew", "chat_log", out.chat_log ? 1 : 0) != 0;
    out.stagger = ini.get_int("crew", "stagger", out.stagger);
    out.interact = ini.get_int("crew", "interact", 0) != 0;
    out.observe = ini.get_int("crew", "observe", out.observe ? 1 : 0) != 0;
    out.control = ini.get_int("crew", "control", out.control ? 1 : 0) != 0;
    out.drive_dir = ini.get_string("crew", "drive_dir");
    for (const auto& section : ini.sections()) {
        if (section == "crew") continue;
        CrewMember m;
        m.label = section;
        m.server = ini.get_string(section, "server");
        m.account = ini.get_string(section, "account");
        m.character = ini.get_string(section, "character");
        m.world = ini.get_string(section, "world");
        m.exec = ini.get_string(section, "exec");
        m.exec_zone = ini.get_string(section, "exec_zone");
        m.game_dir = ini.get_string(section, "game_dir");
        m.autoload = ini.get_string(section, "autoload");
        m.chat_log = ini.get_int(section, "chat_log", -1);
        m.seconds = ini.get_int(section, "seconds", 0);
        m.stagger = ini.get_int(section, "stagger", -1);
        m.interact = ini.get_int(section, "interact", -1);
        m.observe = ini.get_int(section, "observe", -1);
        m.control = ini.get_int(section, "control", -1);
        if (m.server.empty() || m.account.empty()) {
            out.error = "crew member [" + section + "] needs server= and account=";
            return out;
        }
        out.members.push_back(std::move(m));
    }
    if (out.error.empty() && out.members.empty()) out.error = "no crew members (a section per character)";
    return out;
}

std::string format_crew_file(const CrewFile& crew) {
    std::string out;
    out += "[crew]\n";
    if (!crew.data_dir.empty()) out += "data_dir=" + crew.data_dir + "\n";
    if (!crew.game_dir.empty()) out += "game_dir=" + crew.game_dir + "\n";
    out += "stagger=" + std::to_string(std::max(0, crew.stagger)) + "\n";
    if (!crew.autoload.empty()) out += "autoload=" + crew.autoload + "\n";
    out += std::string("chat_log=") + (crew.chat_log ? "1\n" : "0\n");
    out += std::string("observe=") + (crew.observe ? "1\n" : "0\n");
    out += std::string("interact=") + (crew.interact ? "1\n" : "0\n");
    out += std::string("control=") + (crew.control ? "1\n" : "0\n");
    if (!crew.drive_dir.empty()) out += "drive_dir=" + crew.drive_dir + "\n";
    std::set<std::string> used;
    for (const CrewMember& m : crew.members) {
        std::string base = !m.label.empty() ? m.label : (!m.character.empty() ? m.character : m.account);
        if (base.empty() || base == "crew") base = "member";
        for (char& c : base)
            if (c == '[' || c == ']' || c == '\r' || c == '\n') c = '_';
        std::string label = base;
        for (int n = 2; used.contains(label); ++n) label = base + "-" + std::to_string(n);
        used.insert(label);
        out += "\n[" + label + "]\n";
        out += "server=" + m.server + "\n";
        out += "account=" + m.account + "\n";
        if (!m.character.empty()) out += "character=" + m.character + "\n";
        if (!m.world.empty()) out += "world=" + m.world + "\n";
        if (!m.exec.empty()) out += "exec=" + m.exec + "\n";
        if (!m.exec_zone.empty()) out += "exec_zone=" + m.exec_zone + "\n";
        if (!m.game_dir.empty()) out += "game_dir=" + m.game_dir + "\n";
        if (!m.autoload.empty()) out += "autoload=" + m.autoload + "\n";
        if (m.chat_log >= 0) out += "chat_log=" + std::to_string(m.chat_log ? 1 : 0) + "\n";
        if (m.seconds > 0) out += "seconds=" + std::to_string(m.seconds) + "\n";
        if (m.stagger >= 0) out += "stagger=" + std::to_string(m.stagger) + "\n";
        if (m.interact >= 0) out += "interact=" + std::to_string(m.interact ? 1 : 0) + "\n";
        if (m.observe >= 0) out += "observe=" + std::to_string(m.observe ? 1 : 0) + "\n";
        if (m.control >= 0) out += "control=" + std::to_string(m.control ? 1 : 0) + "\n";
    }
    return out;
}

std::filesystem::path crew_drive_dir(const CrewFile& crew, const std::filesystem::path& data_dir) {
    std::filesystem::path drive_dir =
        crew.drive_dir.empty() ? data_dir / "run" : std::filesystem::path(crew.drive_dir);
    if (drive_dir.is_relative()) drive_dir = data_dir / drive_dir;
    return drive_dir;
}

void clear_stale_drive_files(const std::filesystem::path& drive_dir) {
    std::error_code ec;
    std::filesystem::create_directories(drive_dir, ec);
    if (ec) return;
    for (const auto& entry : std::filesystem::directory_iterator(drive_dir, ec)) {
        if (ec || !entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().string();
        if (name.starts_with("drive-") && name.ends_with(".txt")) {
            std::error_code rm_ec;
            std::filesystem::remove(entry.path(), rm_ec);
        }
    }
}

void seed_headless_imgui_ini(const std::filesystem::path& ini, const std::filesystem::path& gui_ini,
                             const std::filesystem::path& legacy_ini) {
    if (ini.empty()) return;
    std::error_code ec;
    if (const auto parent = ini.parent_path(); !parent.empty()) std::filesystem::create_directories(parent, ec);
    if (std::filesystem::exists(ini, ec)) return;
    if (!legacy_ini.empty() && std::filesystem::is_regular_file(legacy_ini, ec)) {
        std::filesystem::rename(legacy_ini, ini, ec);
        if (!ec) return;
        ec.clear();
        std::filesystem::copy_file(legacy_ini, ini, std::filesystem::copy_options::skip_existing, ec);
        if (!ec) std::filesystem::remove(legacy_ini, ec);
        return;
    }
    if (!gui_ini.empty() && std::filesystem::is_regular_file(gui_ini, ec))
        std::filesystem::copy_file(gui_ini, ini, std::filesystem::copy_options::skip_existing, ec);
}

std::vector<std::string> headless_zone_commands(const HeadlessOptions& options, bool first_zone) {
    std::vector<std::string> out = options.exec_zone;
    if (first_zone) out.insert(out.end(), options.exec.begin(), options.exec.end());
    // An --exec line names a command, not something to say: a shell that ate
    // its leading '/' must not turn it into a say in the zone.
    for (auto& line : out)
        if (!line.empty() && line.front() != '/') line.insert(line.begin(), '/');
    return out;
}

namespace {
// One saved login, resolved from the profiles under the data folder: the
// profile, the account in it, and whether its keyring password is there.
struct SavedLogin {
    const ServerProfile* profile = nullptr;
    const SavedAccount* account = nullptr;
    std::string problem; // empty: resolved (account may still be null without --account)
};
SavedLogin resolve_saved(const std::vector<ServerProfile>& profiles, std::string_view server,
                         std::string_view account) {
    SavedLogin out;
    if (!server.empty()) {
        out.profile = pick_headless_profile(profiles, server);
        if (!out.profile) {
            out.problem = "no saved server matches \"" + std::string(server) + "\" (the saved servers are:";
            for (const auto& p : profiles) out.problem += " " + p.id;
            out.problem += ")";
            return out;
        }
    } else if (!account.empty()) {
        for (const auto& p : profiles)
            if (find_account(p, account)) {
                if (out.profile) {
                    out.problem = "account \"" + std::string(account) + "\" is saved on more than one server; name --server";
                    return out;
                }
                out.profile = &p;
            }
        if (!out.profile) {
            out.problem = "no saved server has account \"" + std::string(account) + "\"";
            return out;
        }
    }
    if (!account.empty() && out.profile) {
        out.account = find_account(*out.profile, account);
        if (!out.account) out.problem = "server " + out.profile->id + " has no saved account \"" + std::string(account) + "\"";
    }
    return out;
}

// The crew supervisor (--crew): one hailstone_headless child per member,
// staggered, each logging in by its saved account (the child reads its
// password from the keyring itself; a password never goes on a command
// line). Every member's login is proven before any child starts, as the
// box set's start proves its passwords. A child that exits 3 (its link
// died) is relaunched after 30 s; a clean camp (0) or a failure (1)
// stays down. Stopping the supervisor ends the children (gently, then
// forcibly after 5 s).
int run_crew(const HeadlessOptions& options) {
    std::ifstream in(options.crew_file, std::ios::binary);
    if (!in) {
        log::error("headless crew: cannot read {}", options.crew_file);
        return 2;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CrewFile crew = parse_crew_file(text);
    if (!crew.error.empty()) {
        log::error("headless crew: {}: {}", options.crew_file, crew.error);
        return 2;
    }
    const std::string data_dir = !crew.data_dir.empty() ? crew.data_dir : options.data_dir;
    if (!data_dir.empty()) platform::use_data_dir(data_dir, "--crew");
    if (std::string problem = platform::data_dir_problem(); !problem.empty()) {
        log::error("{}", problem);
        return 1;
    }
    const auto profiles = list_profiles();
    const bool observe_any = crew.observe ||
                             std::any_of(crew.members.begin(), crew.members.end(),
                                         [](const CrewMember& m) { return m.observe > 0; });
    const bool control_any = crew.control ||
                             std::any_of(crew.members.begin(), crew.members.end(),
                                         [](const CrewMember& m) { return m.control > 0; });
    std::filesystem::path drive_dir = crew.drive_dir.empty() ? platform::data_dir() / "run"
                                                             : std::filesystem::path(crew.drive_dir);
    if (drive_dir.is_relative()) drive_dir = platform::data_dir() / drive_dir;
    if (observe_any || control_any) {
        std::error_code ec;
        std::filesystem::create_directories(drive_dir, ec);
        if (ec) log::warn("headless crew: cannot make the drive folder {}: {}", drive_dir.string(), ec.message());
    }
    auto safe_file_name = [](std::string name) {
        for (char& c : name)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
        return name;
    };
    struct Member {
        CrewMember crew;
        std::string profile_id, character, world;
        std::string drive_file;
        bool observe = false;
        bool control = false;
        enum class State { Waiting, Running, Done } state = State::Waiting;
        Clock::time_point at{}; // launch or relaunch time, while Waiting
        int restarts = 0;
    };
    std::vector<Member> members;
    bool any_problem = false;
    for (const auto& m : crew.members) {
        auto saved = resolve_saved(profiles, m.server, m.account);
        if (!saved.problem.empty()) {
            log::error("headless crew: [{}] {}", m.label, saved.problem);
            any_problem = true;
            continue;
        }
        auto pw = platform::system_credentials().load(
            platform::credential_server(saved.profile->host, saved.profile->port), saved.account->username);
        if (!pw || pw->empty()) {
            log::error("headless crew: [{}] no saved password for {} on {}: log in once in the graphical "
                       "client with Save password ticked",
                       m.label, saved.account->username, saved.profile->id);
            any_problem = true;
            continue;
        }
        core::wipe(*pw);
        Member r;
        r.crew = m;
        r.profile_id = saved.profile->id;
        r.character = !m.character.empty() ? m.character : saved.account->character;
        r.world = !m.world.empty() ? m.world : saved.account->world;
        r.observe = m.observe >= 0 ? m.observe != 0 : crew.observe;
        r.control = m.control >= 0 ? m.control != 0 : crew.control;
        if (r.control && r.observe) {
            log::warn("headless crew: [{}] sets both observe and control; control wins", m.label);
            r.observe = false;
        }
        if (r.observe || r.control)
            r.drive_file = (drive_dir / ("drive-" + safe_file_name(m.label) + ".txt")).string();
        members.push_back(std::move(r));
    }
    if (any_problem) {
        log::error("headless crew: nothing started ({} of {} members cannot log in)",
                   crew.members.size() - members.size(), crew.members.size());
        return 1;
    }
    std::filesystem::path exe = platform::install_dir() / "hailstone_headless";
#if defined(_WIN32)
    exe += ".exe";
#endif
    platform::ChildProcesses children;
    auto launch = [&](Member& r) {
        std::vector<std::string> args{exe.string(), "--server", r.profile_id, "--account", r.crew.account};
        const std::string game_dir = !r.crew.game_dir.empty() ? r.crew.game_dir : crew.game_dir;
        if (!data_dir.empty()) args.insert(args.end(), {"--data-dir", data_dir});
        if (!game_dir.empty()) args.insert(args.end(), {"--game-dir", game_dir});
        if (!options.game_dir.empty() && game_dir.empty()) args.insert(args.end(), {"--game-dir", options.game_dir});
        if (!r.character.empty()) args.insert(args.end(), {"--character", r.character});
        if (!r.world.empty()) args.insert(args.end(), {"--world", r.world});
        if (!r.crew.exec.empty()) args.insert(args.end(), {"--exec", r.crew.exec});
        if (!r.crew.exec_zone.empty()) args.insert(args.end(), {"--exec-zone", r.crew.exec_zone});
        args.insert(args.end(), {"--autoload", !r.crew.autoload.empty() ? r.crew.autoload : crew.autoload});
        if (!(r.crew.chat_log >= 0 ? r.crew.chat_log != 0 : crew.chat_log)) args.emplace_back("--no-chat-log");
        args.emplace_back("--quiet"); // its log is its file's; this console is the supervisor's
        if (r.crew.seconds > 0) args.insert(args.end(), {"--seconds", std::to_string(r.crew.seconds)});
        if (r.observe && !r.drive_file.empty()) {
            std::error_code ec;
            std::filesystem::remove(r.drive_file, ec); // a relaunch may find the old file
            args.emplace_back("--drive-observe");
            if (r.crew.interact >= 0 ? r.crew.interact != 0 : crew.interact)
                args.emplace_back("--drive-interact");
            args.insert(args.end(), {"--drive-file", r.drive_file});
        } else if (r.control && !r.drive_file.empty()) {
            std::error_code ec;
            std::filesystem::remove(r.drive_file, ec);
            args.emplace_back("--drive-control");
            args.insert(args.end(), {"--drive-file", r.drive_file});
        }
        std::string error;
        if (children.start_tracked(r.crew.label, args, error)) {
            r.state = Member::State::Running;
            log::info("headless crew: started [{}] as {} on {}", r.crew.label, r.character, r.profile_id);
        } else {
            r.state = Member::State::Done;
            if (!r.drive_file.empty()) {
                std::error_code ec;
                std::filesystem::remove(r.drive_file, ec);
            }
            log::error("headless crew: [{}] could not start: {}", r.crew.label, error);
        }
    };
    auto at = Clock::now();
    for (auto& r : members) {
        r.at = at;
        at += std::chrono::seconds(r.crew.stagger >= 0 ? r.crew.stagger : crew.stagger);
    }
    log::info("headless crew: {} members on {} saved server(s); stopping this process ends them all",
              members.size(), profiles.size());
    Clock::time_point last_status = Clock::now();
    while (!stop_requested) {
        const auto now = Clock::now();
        for (auto& r : members)
            if (r.state == Member::State::Waiting && now >= r.at) launch(r);
        children.poll();
        for (const auto& e : children.take_exited()) {
            for (auto& r : members)
                if (r.crew.label == e.name && r.state == Member::State::Running) {
                    if (!r.drive_file.empty()) {
                        std::error_code ec;
                        std::filesystem::remove(r.drive_file, ec); // the endpoint is gone with the child
                    }
                    if (e.exit_code == 3 || e.exit_code < 0) { // 3: link died; negative: crashed (0xC0000409, ...)
                        r.state = Member::State::Waiting;
                        r.at = now + std::chrono::seconds(30);
                        ++r.restarts;
                        log::warn("headless crew: [{}] {}; relaunching in 30 s (restart {})", e.name,
                                  e.exit_code == 3 ? "link died" : "crashed", r.restarts);
                    } else {
                        r.state = Member::State::Done;
                        log::info("headless crew: [{}] ended (exit {})", e.name, e.exit_code);
                    }
                }
        }
        if (std::all_of(members.begin(), members.end(), [](const Member& r) { return r.state == Member::State::Done; }))
            break;
        if (now - last_status > std::chrono::seconds(30)) {
            last_status = now;
            std::size_t running = 0;
            for (const auto& r : members) running += r.state == Member::State::Running;
            log::info("headless crew: {} of {} running", running, members.size());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (stop_requested) {
        log::info("headless crew: stopping the children");
        children.stop_all(false);
        const auto end = Clock::now() + std::chrono::seconds(5);
        while (Clock::now() < end && children.running() > 0) {
            children.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        children.stop_all(true);
        for (const auto& r : members)
            if (!r.drive_file.empty()) {
                std::error_code ec;
                std::filesystem::remove(r.drive_file, ec);
            }
    }
    return 0;
}
} // namespace

int run_headless(HeadlessOptions options) {
    if (!options.crew_file.empty()) {
        stop_requested = 0;
        std::signal(SIGINT, on_stop_signal);
        std::signal(SIGTERM, on_stop_signal);
        return run_crew(options);
    }
    stop_requested = 0;
    std::signal(SIGINT, on_stop_signal);
    std::signal(SIGTERM, on_stop_signal);
    if (options.quiet) log::set_console(false); // everything still goes to the log file
    // Share a graphical client's folder when asked: its plugins, its
    // per-character plugin configs and its servers/ profiles are the crew's,
    // and an automation's config (e.g. HSMono's sentinel.json, which starts
    // its broker comms) lives there, not beside a worktree binary.
    if (!options.data_dir.empty()) platform::use_data_dir(options.data_dir, "--data-dir");
    if (std::string problem = platform::data_dir_problem(); !problem.empty()) {
        log::error("{}", problem);
        return 1;
    }
    // A saved server/account (--server/--account): the profile names the
    // login host, the keyring its password, the account its character and
    // world — as the graphical client's --account does. No password is
    // ever written to a file or a command line by this path.
    std::string saved_profile_id;
    std::vector<ServerProfile> saved_profiles;
    if (!options.server.empty() || !options.account.empty()) {
        saved_profiles = list_profiles();
        auto saved = resolve_saved(saved_profiles, options.server, options.account);
        if (!saved.problem.empty()) {
            log::error("headless: {}", saved.problem);
            return 1;
        }
        saved_profile_id = saved.profile->id;
        options.host = saved.profile->host;
        options.port = static_cast<std::uint16_t>(saved.profile->port);
        if (saved.account) {
            if (options.character.empty()) options.character = saved.account->character;
            if (options.world.empty()) options.world = saved.account->world;
            auto pw = platform::system_credentials().load(
                platform::credential_server(saved.profile->host, saved.profile->port), saved.account->username);
            if (!pw || pw->empty()) {
                log::error("headless: no saved password for {} on {}: log in once in the graphical client "
                           "with Save password ticked",
                           saved.account->username, saved.profile->id);
                return 1;
            }
            options.user = saved.account->username;
            options.password = std::move(*pw);
        }
        if (!saved.profile->game_files.empty() && !options.game_dir_given) options.game_dir = saved.profile->game_files;
        log::info("headless: saved server {} ({}:{}){}", saved.profile->id, options.host, options.port,
                  saved.account ? " as " + saved.account->username : "");
    }
    if (options.host.empty() || options.user.empty() || options.password.empty() || options.character.empty()) {
        log::error("headless: a login needs --host/--user/--password/--character, or --server/--account");
        return 1;
    }
    if (options.game_dir.empty()) {
        log::error("headless: --game-dir or EQ_GAME_DIR is required");
        return 1;
    }
    log::open_file(platform::logs_dir() /
                   ("headless-" + sanitize_file_name(options.character) + ".log"));
    GameDir game(options.game_dir);
    if (!saved_profile_id.empty()) game.set_overlay(profile_assets_dir(saved_profile_id));

    // Login, then play the --world server (or the first on the list).
    net::LoginClient login;
    login.start(options.host, options.port, options.user, options.password);
    core::wipe(options.password);
    auto deadline = Clock::now() + std::chrono::seconds(30);
    while (Clock::now() < deadline && !stop_requested) {
        login.update();
        if (login.state() == net::LoginState::LoggedIn) login.request_server_list();
        if (login.state() == net::LoginState::ServerList) {
            if (login.servers().empty()) { log::error("headless: no servers"); return 1; }
            if (options.list_servers) {
                for (const auto& s : login.servers())
                    std::printf("%u\t%s\tstatus %d\t%d players\n", s.id, s.name.c_str(), s.status, s.players);
                login.close();
                return 0;
            }
            auto id = headless_server_id(login.servers(), options.world);
            if (!id) {
                log::error("headless: no server matches --world \"{}\"; the list is:", options.world);
                for (const auto& s : login.servers()) log::error("  {}  {}", s.id, s.name);
                return 1;
            }
            for (const auto& s : login.servers())
                if (s.id == *id) log::info("headless: playing on {} ({})", s.name, s.id);
            login.request_play(*id);
        }
        if (login.state() == net::LoginState::PlayAccepted || login.state() == net::LoginState::Failed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (login.state() != net::LoginState::PlayAccepted) { log::error("headless login: {}", login.error()); return 1; }
    auto ticket = *login.ticket();
    login.close();
    log::info("headless: login ok, world {}:{}", ticket.world_host, ticket.world_port);

    std::unique_ptr<net::GameConnection> conn;
    std::optional<assets::Zone> zone;
    std::optional<game::CollisionMesh> collision;
    // The world this run plays on, as the login list named it: ${EverQuest.Server}
    // for the scripts and plugins (mq::Context::server_name). Automations key
    // real state on it — Sentinel scopes its broker heartbeat by it and finds
    // the character's config folder by it — so an empty answer silently
    // disables both, as an empty profile would in the graphical client.
    const std::string server_label = ticket.server_name;
    AutomationEnv env;
    env.install_dir = platform::install_dir();
    env.data_dir = platform::data_dir();
    env.autoload = options.plugin_autoload;
    env.server_name = [&server_label] { return server_label; };
    env.server_profile = [&server_label, &saved_profile_id] {
        return !saved_profile_id.empty() ? saved_profile_id : profile_id_for_world(server_label);
    };
    env.profile_name = [&server_label] { return server_label; };
    env.connection = [&conn] { return conn.get(); };
    Config ability_config;
    ActionsUi ability_actions(ability_config, {});
    UiHost ability_host;
    ability_host.actions = &ability_actions;
    env.ability = [&](mq::AbilityKind kind, std::string_view name) {
        return ability_state(ability_host, kind, name);
    };
    env.ability_bulk = [&] { return ability_bulk_rows(ability_host); };
    env.use_ability = [&](mq::AbilityKind kind, std::string_view name) {
        return use_ability(ability_host, kind, name);
    };
    env.window_open = [](std::string_view) { stub_once("window"); return false; };
    env.face = [](double, bool) { stub_once("face"); };
    env.keypress = [](std::string_view, int) { stub_once("keypress"); return false; };
    env.raise_window = [] { log::info("headless: /foreground ignored (no window)"); };
    env.line_of_sight = [&collision](const std::array<float, 3>& a, const std::array<float, 3>& b) -> std::optional<bool> {
        if (!collision) return std::nullopt;
        return collision->line_of_sight(glm::vec3(a[0], a[1], a[2]), glm::vec3(b[0], b[1], b[2]));
    };
    env.can_see = [&conn, &collision](std::uint32_t id) -> std::optional<bool> {
        if (!conn || !collision) return std::nullopt;
        const auto& w = conn->world();
        auto me = w.player();
        auto it = w.spawns.find(id);
        if (!me || it == w.spawns.end()) return std::nullopt;
        return collision->line_of_sight(me->position, it->second.position);
    };
    ability_host.run_button = [&](const game::HotButton& button) {
        if (!conn || !ability_host.character) return;
        AbilityView view;
        if (const auto* me = conn->world().player()) {
            view.position = me->position;
            view.heading = me->heading;
        }
        view.can_see_target = env.can_see(conn->target()).value_or(false);
        ability_actions.run_button(*conn, ability_host.character, view, button);
    };
    // Navigation (app/navigator.h) as the graphical client wires it, minus the
    // doors' models (no WorldObjects headless: the mesh runs through doorways,
    // and a door click still goes out when Navigator asks for one).
    float nav_radius = 1.78f; // the movement's sweep radius, kept in step below
    Navigator::Env nenv;
    nenv.spawn = [&conn](std::uint32_t id) -> const game::Spawn* {
        if (!conn) return nullptr;
        auto it = conn->world().spawns.find(id);
        return it == conn->world().spawns.end() ? nullptr : &it->second;
    };
    nenv.door = [&conn](std::uint8_t id) -> const game::Door* { return conn ? conn->door(id) : nullptr; };
    nenv.doors = [&conn] {
        std::vector<const game::Door*> v;
        if (conn)
            for (const auto& d : conn->doors()) v.push_back(&d);
        return v;
    };
    nenv.use_door = [&conn, &nav_radius](std::uint8_t id) { return conn && conn->click_door(id, nav_radius); };
    nenv.door_reachable = [&conn, &nav_radius](std::uint8_t id) { return conn && conn->door_in_reach(id, nav_radius); };
    nenv.chat = [](const std::string& t) { log::info("nav: {}", t); };
    nenv.request_server_path = [&conn](std::uint32_t id, game::ServerPathOrigin origin) {
        return conn ? conn->request_quiet_path(id, origin) : game::ServerPathLease{};
    };
    nenv.server_path = [&conn](const game::ServerPathLease& lease) {
        return conn ? conn->take_quiet_path(lease) : std::nullopt;
    };
    nenv.cancel_server_path = [&conn](const game::ServerPathLease& lease) {
        if (conn) conn->cancel_server_path(lease);
    };
    nenv.interaction_context = [&conn] { return conn ? conn->interaction_context() : 0; };
    nenv.started = [&conn] {
        if (conn && conn->follow_target()) conn->stop_follow(false);
    };
    nenv.line_of_sight = [&collision](const glm::vec3& a, const glm::vec3& b) {
        return collision && collision->line_of_sight(a, b);
    };
    nenv.threads = 2;
    nenv.prebuild = true;
    Navigator navigator(nenv);
    env.nav = [&navigator]() -> nav::NavControl* { return &navigator; };
    PlayerMovement movement;
    game::ZoneLineTrigger zone_line;
    // Create the CPU-only ImGui context before Automation so plugins can cache its pointer at init.
    std::unique_ptr<HeadlessImGui> headless_imgui;
    if (options.drive_observe || options.drive_control) {
        const std::string profile = !saved_profile_id.empty() ? saved_profile_id : profile_id_for_world(server_label);
        const auto char_files = character_files(profile, server_label, options.character);
        const auto ini = !char_files.empty()
                             ? char_files.headless_imgui_ini()
                             : platform::cache_dir() / ("imgui-headless-" + sanitize_file_name(options.character) + ".ini");
        const auto legacy_ini =
            platform::data_dir() / ("imgui-headless-" + sanitize_file_name(options.character) + ".ini");
        seed_headless_imgui_ini(ini, platform::imgui_ini(), legacy_ini);
        headless_imgui = std::make_unique<HeadlessImGui>(ini);
    }
    std::unique_ptr<DriveObserveServer> observe;
    env.headless = true;
    env.viewer_connected = [&observe] { return observe && observe->viewer_connected(); };
    Automation automation(env);

    if (options.drive_observe || options.drive_control) {
        try {
            observe = std::make_unique<DriveObserveServer>(
                platform::make_local_pipe_session(), options.drive_interact || options.drive_control,
                options.drive_control);
        } catch (const std::exception& e) {
            log::error("headless: {} could not start: {}",
                       options.drive_control ? "--drive-control" : "--drive-observe", e.what());
            return 1;
        }
        // Forward /crew commands issued by plugins on headless (e.g. Sentinel's peer switcher) to the attached GUI.
        automation.hooks().add_command(plugin::Hooks::kClient, "/crew", [&observe](std::string_view args) {
            if (observe && observe->viewer_connected()) observe->publish_crew_command(args);
        });
        if (const auto parent = std::filesystem::path(options.drive_file).parent_path(); !parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }
        log::info("headless: drive {} at {} (the file is written at the first zone-in)",
                  options.drive_control ? "control" : "observe", observe->session().endpoint);
    }

    net::ConnectionOptions copts;
    copts.hooks = &automation.hooks();
    copts.diagnostic_chat_log = options.chat_log;
    copts.plugins = automation.extensions();
    if (observe) {
        copts.snapshot_journal = true;
        copts.zone_packet_tap = [server = observe.get()](const net::AppPacket& p) {
            server->publish_zone_packet(p);
        };
    }
    if (!saved_profile_id.empty()) {
        // The character's files under the saved profile, its bank snapshot,
        // and the server's client files at the first zone-in, as in the GUI.
        copts.profile_id = saved_profile_id;
        copts.server_files_dir = profile_dir(saved_profile_id);
        copts.bank_snapshot_file = true;
    }
    conn = std::make_unique<net::GameConnection>(game, copts);
    conn->start(ticket);

    // The character sheet's tables (the GUI's StatsUi loads the same):
    // without them the Character's values are 0.
    game::mechanics::GameTables tables;
    if (!tables.load(game)) log::warn("headless: BaseData/SkillCaps missing; character values will be 0");
    std::unique_ptr<game::mechanics::CharacterSheet> sheet;
    std::unique_ptr<game::mechanics::Character> character;
    core::StepClock step_clock;
    step_clock.set_fixed(true);
    const auto start = Clock::now();
    bool entered = false, first_zone = true, consent_logged = false;
    bool movement_ready = false; // the movement body placed at a zone-in
    std::string loaded_zone;     // the zone whose collision is loaded
    bool nav_was_active = false; // for the walk's speed line, once per walk
    glm::vec3 feet{0.0f};
    float yaw = 0.0f, pitch = 0.0f, floor_offset = 6.0f;
    // DoTeleportB on the player (game/zoning.h), as the graphical client's
    // teleport_b runs it with only the connection and collision behind its
    // hooks: a move inside this zone places the movement body there, another
    // zone goes out through MoveToZone. Shared by the zone lines below and
    // the server's OP_RequestClientZoneChange (a summon, #goto), which the
    // connection hands to the app's teleport handler.
    auto teleport_b = [&](const game::TeleportBArgs& a) -> bool {
        game::TeleportBPlayer p;
        const glm::vec3 at = feet + glm::vec3(0, 0, floor_offset);
        p.x = at.x;
        p.y = at.y;
        p.z = at.z;
        p.heading = game::yaw_to_heading(yaw);
        p.stand_state = conn->player_status().stand_state;
        p.zone = static_cast<std::int32_t>(conn->current_zone_dword());
        if (const game::Spawn* me = conn->world().player()) p.race = static_cast<std::int32_t>(me->race);
        bool local = false;
        game::TeleportBHooks b;
        b.send_client_update = [&] { conn->send_position_now(); };
        b.position_sample = [&](int kind) {
            if (kind != game::kPositionUpdate) conn->record_position_sample(static_cast<std::uint32_t>(kind));
        };
        b.save_physics = [] {};
        b.floor_height = [&](float fx, float fy, float fz, int) -> double {
            local = true;
            auto f = collision ? collision->floor_below({fx, fy, fz}, 0.0f) : std::nullopt;
            return f ? static_cast<double>(*f) : static_cast<double>(game::kNoFloor);
        };
        b.update_actor = [] {};
        b.find_person_request = [&] { conn->find_person_request(); };
        b.rewind_reset = [] {};
        b.move_to_zone = [&](const game::MoveToZoneCall& m) { conn->move_to_zone(m); };
        bool ok = game::do_teleport_b(p, a, 5, b);
        if (ok && local) {
            feet = glm::vec3(p.x, p.y, p.z - floor_offset);
            yaw = game::heading_to_yaw(p.heading);
            movement.place(feet);
            conn->set_player_motion({p.x, p.y, p.z}, p.heading, glm::vec3(0), false);
            log::info("headless: moved within the zone to ({:.1f}, {:.1f}, {:.1f})", p.x, p.y, p.z);
        }
        return ok;
    };
    // A zone line under the feet (game/zone_lines.h): DoTeleport as the
    // graphical client's region_teleport runs it, with only the connection
    // and the collision mesh behind its hooks.
    auto region_teleport = [&](const std::string& text) {
        glm::vec3 pos = feet + glm::vec3(0, 0, floor_offset);
        const auto rows = conn->zone_point_rows();
        const std::uint32_t zone_dword = conn->current_zone_dword();
        game::DoTeleportInput in;
        in.index = -1;
        in.point[0] = pos.x;
        in.point[1] = pos.y;
        in.point[2] = pos.z;
        in.waiting_for_port = conn->waiting_for_port() ? 1 : 0;
        game::DoTeleportHooks h;
        h.resolve = [&](std::int32_t index, bool& has_description) {
            has_description = true;
            float loc[3] = {pos.x, pos.y, pos.z};
            auto t = game::resolve_teleport_index(text, index, static_cast<std::int32_t>(zone_dword), loc, rows);
            feet += glm::vec3(loc[0], loc[1], loc[2]) - pos;
            pos = glm::vec3(loc[0], loc[1], loc[2]);
            log::info("headless zone line {}: {} zone {:#x} point {} ({:.1f}, {:.1f}, {:.1f})", text,
                      t.ok ? "to" : "unresolved", t.found, t.zone_point, t.x, t.y, t.z);
            return t;
        };
        h.teleport_b = [&](std::int32_t to_zone, std::uint32_t x, std::uint32_t y, std::uint32_t z,
                           std::uint32_t heading) {
            game::TeleportBArgs a;
            a.zone = to_zone;
            a.x = std::bit_cast<float>(x);
            a.y = std::bit_cast<float>(y);
            a.z = std::bit_cast<float>(z);
            a.heading = std::bit_cast<float>(heading);
            return teleport_b(a);
        };
        h.send_index_request = [&](std::int32_t zp) { conn->send_teleport_index_request(zp); };
        h.position_sample = [&](int kind, const float* point) {
            const glm::vec3 at = point ? glm::vec3(point[0], point[1], point[2]) : pos;
            conn->record_position_sample(static_cast<std::uint32_t>(kind), &at);
        };
        h.free_description = [] {};
        h.get_spell_affect = [](std::int32_t) { return 0; };
        h.remove_affect = [](std::int32_t, std::int32_t) {};
        game::do_teleport(in, h);
        if (in.waiting_for_port == 0 && conn->waiting_for_port()) conn->clear_waiting_for_port();
    };
    // The connection's app hooks the graphical client installs per connection:
    // CastSpell's line-of-sight test, and OP_RequestClientZoneChange's
    // DoTeleportB (a summon or #goto in this zone moves the player instead of
    // zoning). Without the second, a server summon is answered with a zone
    // change the server never asked for and the movement body keeps
    // reporting the old position, so the character stays where it was.
    conn->set_line_of_sight([&collision](const glm::vec3& a, const glm::vec3& b) {
        return !collision || collision->line_of_sight(a, b);
    });
    conn->set_teleport_handler([&teleport_b](const game::TeleportBArgs& a) { teleport_b(a); });
    Clock::time_point in_zone_at{}, last_status{}, last_observe_state{}, human_drive_until{}, last_remote_imgui{};
    bool drive_file_written = false, drive_file_failed = false;
    bool viewer_was_connected = false;
    std::uint64_t last_viewer_gen = 0;
    bool send_full_imgui_atlas = false;
    std::uint64_t last_inventory_revision = ~std::uint64_t{0};
    std::uint64_t last_xtarget_hash = ~std::uint64_t{0};
    std::string zone_seen;
    net::ConnectionState last = net::ConnectionState::Idle;
    int result = 1;
    while (!stop_requested) {
        const auto now_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
        auto steps = step_clock.advance(now_us);
        if (step_clock.take_dropped()) {
            // The game clock drops time past a 250 ms hitch (docs/mechanics.md
            // "Frame rate"): under load the movement then lags the wall clock,
            // which reads as walking slower than a graphical client.
            static Clock::time_point last_drop_log{};
            if (Clock::now() - last_drop_log > std::chrono::seconds(10)) {
                last_drop_log = Clock::now();
                log::warn("headless: the game clock dropped steps under load; movement lags the wall clock");
            }
        }
        if (conn && !steps.empty()) {
            std::vector<std::int64_t> at;
            const std::int64_t conn_now = net::GameConnection::now_ms();
            const auto real_ms = static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
            for (const auto& st : steps)
                at.push_back(conn_now - std::max<std::int64_t>(0, real_ms - static_cast<std::int64_t>(st.ms)));
            conn->set_game_steps(std::move(at));
        }
        conn->update();
        bool force_state_push = false;
        if (observe) {
            const bool connected = observe->viewer_connected();
            const std::uint64_t viewer_gen = observe->viewer_generation();
            if (connected != viewer_was_connected || (connected && viewer_gen != last_viewer_gen)) {
                viewer_was_connected = connected;
                if (connected) last_viewer_gen = viewer_gen;
                human_drive_until = {};
                last_inventory_revision = ~std::uint64_t{0};
                last_xtarget_hash = ~std::uint64_t{0};
                send_full_imgui_atlas = connected;
                last_remote_imgui = {};
                if (headless_imgui) {
                    if (!connected) headless_imgui->flush_ini();
                    headless_imgui->reset_input();
                }
                log::info("headless: drive viewer {}", connected ? "attached" : "detached");
            }
            // Apply the latest human movement pose BEFORE semantic actions so reach checks (looting a corpse,
            // opening a door/merchant/bank) see the player's current position right as they arrive.
            if (options.drive_control) {
                if (auto move = observe->take_move(); move && conn->in_zone() && movement_ready) {
                    const auto now_move = Clock::now();
                    if (move->active) {
                        if (navigator.status().active) navigator.stop("Player took movement control");
                        human_drive_until = now_move + std::chrono::milliseconds(450);
                    } else {
                        human_drive_until = {};
                    }
                    feet = move->position - glm::vec3(0, 0, floor_offset);
                    yaw = game::heading_to_yaw(move->heading);
                    movement.place(feet);
                    conn->set_speed_run(move->speed_run);
                    conn->set_standing_on(move->standing_on);
                    conn->set_player_motion(move->position, move->heading, move->velocity, move->moving);
                    if (zone)
                        if (auto text = zone_line.update(feet, zone->regions)) region_teleport(*text);
                }
            }
            if (options.drive_interact || options.drive_control) {
                for (const auto& action : observe->take_actions()) {
                    const bool ok = conn->apply_drive_action(action, options.drive_control, nav_radius);
                    force_state_push = true;
                    if (!ok && !options.drive_control) break;
                }
            }
            if (options.drive_control) {
                for (const auto& fwd : observe->take_forwarded()) {
                    if (!net::drive_forward_allowed(fwd.packet, fwd.world)) continue;
                    if (fwd.world) conn->send_world_packet(fwd.packet);
                    else conn->send_zone_packet(fwd.packet);
                }
            }
        }
        if (observe && conn->in_zone()) {
            if (!drive_file_written && !drive_file_failed) {
                const std::string character_name = !options.character.empty() ? options.character : conn->character_name();
                const std::string text = "endpoint=" + observe->session().endpoint + "\n" +
                                         "token=" + observe->session().token + "\n" +
                                         "character=" + character_name + "\n" +
                                         "profile=" + saved_profile_id + "\n" +
                                         "game_dir=" + options.game_dir + "\n" +
                                         "interactive=" + ((options.drive_interact || options.drive_control) ? "1\n" : "0\n") +
                                         "control=" + (options.drive_control ? "1\n" : "0\n");
                if (platform::write_private_file(options.drive_file, text)) {
                    drive_file_written = true;
                    log::info("headless: drive {} file {} written",
                              options.drive_control ? "control" : "observe", options.drive_file);
                } else {
                    drive_file_failed = true;
                    log::error("headless: --drive-file {} could not be written (it must not exist yet)",
                               options.drive_file);
                }
            }
            if (observe->needs_snapshot()) {
                auto snap = conn->capture_snapshot();
                last_inventory_revision = conn->inventory().revision();
                last_xtarget_hash = ~std::uint64_t{0};
                log::info("headless: sending snapshot for {} at ({:.1f}, {:.1f}, {:.1f})",
                          conn->character_name(), snap.position.x, snap.position.y, snap.position.z);
                observe->set_snapshot(std::move(snap));
            }
            const auto now = Clock::now();
            const auto interval = options.drive_control ? std::chrono::milliseconds(50) : std::chrono::milliseconds(100);
            const std::uint64_t inv_rev = conn->inventory().revision();
            if (observe->viewer_connected() &&
                (force_state_push || inv_rev != last_inventory_revision || now - last_observe_state >= interval)) {
                last_observe_state = now;
                const auto local = conn->capture_local_state();
                DriveLocalState state;
                state.position = local.position;
                state.heading = local.heading;
                state.velocity = local.velocity;
                state.moving = local.moving;
                state.target = local.target;
                if (inv_rev != last_inventory_revision) {
                    last_inventory_revision = inv_rev;
                    state.inventory_packet = local.inventory_packet;
                }
                if (options.drive_interact || options.drive_control) state.ui = conn->capture_drive_ui();
                if (options.drive_control) {
                    state.has_status = true;
                    state.stand_state = local.stand_state;
                    state.owner_status = local.owner_status;
                    state.dead = local.dead;
                    state.auto_attack = local.auto_attack;
                    state.auto_fire = local.auto_fire;
                    state.running = local.running;
                    state.hp = local.hp;
                    state.max_hp = local.max_hp;
                    state.mana = local.mana;
                    state.max_mana = local.max_mana;
                    state.endurance = local.endurance;
                    state.max_endurance = local.max_endurance;
                    // Hash xtargets so we only transmit the slot list when it changes.
                    std::uint64_t xh = 14695981039346656037ull;
                    auto mix = [&](std::uint64_t v) { xh = (xh ^ v) * 1099511628211ull; };
                    mix(local.xtarget_auto_haters ? 1u : 0u);
                    mix(local.xtarget_zone);
                    mix(local.xtarget_slots.size());
                    for (const auto& s : local.xtarget_slots) {
                        mix(s.type);
                        mix(s.status);
                        mix(s.spawn_id);
                        mix(s.name.size());
                    }
                    if (xh != last_xtarget_hash) {
                        last_xtarget_hash = xh;
                        state.has_xtargets = true;
                        state.xtarget_auto_haters = local.xtarget_auto_haters;
                        state.xtarget_zone = local.xtarget_zone;
                        state.xtarget_slots = local.xtarget_slots;
                    }
                }
                observe->publish_state(state);
            }
        }
        if (auto z = conn->take_zone_to_load()) {
            if (*z != loaded_zone) {
                navigator.zone_unloaded();
                movement_ready = false;
                zone.reset();
                collision.reset();
                if (options.load_collision) {
                    try {
                        zone = assets::load_zone(game, *z);
                        collision = game::CollisionMesh::build(*zone);
                        conn->reground();
                        navigator.zone_loaded(*collision, &zone->regions,
                                              platform::cache_dir() / "navmesh" /
                                                  (server_label.empty() ? std::string("default")
                                                                        : profile_id_for_world(server_label)) /
                                                  nav::ZoneNav::cache_name(*z));
                        log::info("headless: collision for {} loaded (no scene)", *z);
                    } catch (const std::exception& e) {
                        log::warn("headless: zone {} collision unavailable: {}; continuing without it", *z, e.what());
                        zone.reset();
                        collision.reset();
                    }
                }
                loaded_zone = *z;
            }
        }
        ability_host.conn = conn.get();
        ability_host.in_game = conn->state() == net::ConnectionState::InZone;
        ability_host.character = nullptr;
        character.reset();
        if (ability_host.in_game && conn->world().player() && conn->profile()) {
            conn->refresh_worn_stats_disabled();
            if (!sheet) sheet = std::make_unique<game::mechanics::CharacterSheet>();
            game::mechanics::build_sheet_into(
                *sheet, *conn->profile(), conn->inventory(), conn->spell_state(), conn->live_state(),
                [&conn](std::vector<game::mechanics::SheetAbility>& out) { conn->aa_sheet_abilities_into(out); },
                conn->zone_info() ? &*conn->zone_info() : nullptr, &conn->world());
            sheet->prestige_limit = conn->game_feature(0x13);
            character = std::make_unique<game::mechanics::Character>(
                *sheet, tables, game::mechanics::spell_lookup(conn->spell_table()));
            character->process_all_stats();
            ability_host.character = character.get();
            const std::int64_t items = character->stats().hp_bonus;
            conn->set_own_hp_items(sheet->vitals.cur_hp ? 0 : items, sheet->vitals.max_hp ? 0 : items);
        }
        const auto frame_begin = Clock::now();
        automation.frame();
        if (const auto frame_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - frame_begin).count();
            frame_ms >= 1000) {
            log::warn("headless: automation.frame() stalled for {} ms", frame_ms);
        }
        if (observe && headless_imgui && observe->viewer_connected()) {
            const auto now_ui = Clock::now();
            auto imgui_in = observe->take_imgui_input();
            if (imgui_in || now_ui - last_remote_imgui >= std::chrono::milliseconds(33)) {
                last_remote_imgui = now_ui;
                headless_imgui->render_frame(automation, *observe, imgui_in,
                                             options.drive_interact || options.drive_control, send_full_imgui_atlas);
                if (const auto ui_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - now_ui).count();
                    ui_ms >= 1000) {
                    log::warn("headless: remote ImGui render stalled for {} ms", ui_ms);
                }
            }
        }
        // A lease's resume must keep the loop and the plugin frames coming; a line every 10 s says whether it does.
        {
            static std::uint64_t frames = 0;
            static Clock::time_point last_heartbeat{};
            ++frames;
            if (const auto now = Clock::now(); now - last_heartbeat > std::chrono::seconds(10)) {
                last_heartbeat = now;
                const auto& xt = conn->xtargets().slots;
                const auto filled = static_cast<std::size_t>(std::count_if(
                    xt.begin(), xt.end(), [](const game::XTargetSlot& s) { return s.spawn_id != 0; }));
                log::info("headless: frame {}: {}, player {}, zone {}, plugins {}, pulses {}, xtargets {}/{}", frames,
                          net::to_string(conn->state()), conn->world().player() ? "yes" : "no",
                          conn->world().zone_short_name, automation.loaded_plugin_count(), automation.pulse_count(),
                          filled, xt.size());
            }
        }
        if (automation.plugin_consent() && !consent_logged) {
            consent_logged = true;
            log::error("headless: a server plugin waits for consent and cannot be asked here; "
                       "allow it from the graphical client on this server first (docs/headless.md)");
        }
        // The connection has already dispatched the chat hooks; mirror lines to an attached GUI viewer.
        while (auto c = conn->poll_chat()) {
            if (observe && observe->viewer_connected()) {
                observe->publish_chat({static_cast<std::uint8_t>(c->kind), c->rgb, c->color, c->peer, c->text});
            }
        }
        if (conn->state() != last) {
            last = conn->state();
            log::info("[{}] {}", net::to_string(last), conn->status());
            if (!conn->error().empty()) log::info("  error: {}", conn->error());
        }
        if (last == net::ConnectionState::CharacterSelect && !entered) {
            conn->enter_world(options.character);
            entered = true;
        }
        if (last == net::ConnectionState::InZone) {
            if (zone_seen != conn->world().zone_short_name) {
                zone_seen = conn->world().zone_short_name;
                in_zone_at = Clock::now();
                // Headless has no ExtendedTargetWindow; match the stock client's AutoAddHaters=true default so
                // xtargets stay populated and a GUI control lease adopts true instead of a stale false.
                conn->set_xtarget_auto_haters(true);
                log::info("headless: in {} as {}", zone_seen, options.character);
                for (const auto& line : headless_zone_commands(options, first_zone)) {
                    log::info("headless: exec: {}", line);
                    conn->send_command(line);
                }
                if (!first_zone && observe && observe->viewer_connected()) {
                    last_inventory_revision = conn->inventory().revision();
                    last_xtarget_hash = ~std::uint64_t{0};
                    observe->publish_snapshot(conn->capture_snapshot());
                }
                first_zone = false;
            }
            if (!steps.empty() && collision)
                conn->smooth(static_cast<float>(steps.size()) / core::StepClock::kRate,
                             [&collision](const glm::vec3& p) { return collision->floor_below(p, 0.5f); });
            // The movement body (app/player_movement.h), placed at the zone-in
            // and driven per game step by Navigator's steering, as the
            // graphical client drives it: /nav, /moveto and /stick walk here.
            if (const game::Spawn* me = conn->world().player(); me && !movement_ready) {
                conn->set_running(true); // a background character travels at a run (the GUI's run/walk toggle)
                floor_offset = me->heights.avatar_height;
                feet = me->position - glm::vec3(0, 0, floor_offset);
                yaw = game::heading_to_yaw(me->heading);
                pitch = 0.0f;
                movement.reset(feet, yaw);
                movement_ready = true;
                zone_line.reset();
                if (zone) (void)zone_line.update(feet, zone->regions);
            }
            if (movement_ready && Clock::now() >= human_drive_until) {
                for (const auto& st : steps) {
                    const game::Spawn* me = conn->world().player();
                    if (!me) break;
                    const float dt_step = st.elapsed_ms ? static_cast<float>(st.elapsed_ms) / 1000.0f
                                                        : 1.0f / core::StepClock::kRate;
                    Navigator::Frame nf;
                    nf.feet = feet;
                    nf.now = std::chrono::duration<double>(start.time_since_epoch()).count() +
                             static_cast<double>(st.ms) / 1000.0;
                    const std::uint8_t stand = conn->player_status().stand_state;
                    nf.can_move = !conn->interface_locked() && !conn->player_status().dead &&
                                  (stand == net::kStandStanding || stand == net::kStandCrouching);
                    nf.player_input = false; // nobody's keys here: a script's own stop ends a walk
                    nf.height = floor_offset;
                    MovementKeys mk;
                    const nav::SteerOutput steer = navigator.frame(nf);
                    if (steer.active) {
                        yaw = steer.yaw;
                        mk.forward = steer.forward;
                        mk.jump = steer.jump;
                        mk.strafe_left = steer.strafe < 0;
                        mk.strafe_right = steer.strafe > 0;
                        if (steer.pitch) pitch = *steer.pitch;
                    }
                    if (conn->player_status().dead) mk = MovementKeys{};
                    ZoneMoveWorld world(collision ? &*collision : nullptr, nullptr,
                                        zone ? &zone->regions : nullptr);
                    world.apply_rules(conn->server_rules(), conn->doors(), navigator.passes_open_doors());
                    nav_radius = game::mechanics::sweep_radius(
                        game::mechanics::SweepRadiusSpawn{floor_offset, me->race});
                    movement.set_collision_radius(nav_radius);
                    movement.set_race(me->race);
                    PlayerMovement::Frame frame =
                        movement.update(mk, yaw, pitch, dt_step * 1000.0f,
                                        static_cast<std::uint32_t>(st.ms + 1), *conn,
                                        zone ? &zone->regions : nullptr, floor_offset, &world, character.get(),
                                        st.elapsed_ms);
                    feet = frame.feet;
                    yaw = frame.yaw;
                    pitch = std::clamp(frame.pitch, -1.5f, 1.5f);
                    // Once a second of walking: our own speed in units a
                    // second (the body's velocity a tick, at 60 ticks), and
                    // the fastest the target spawn moved in that second by
                    // its own update velocity (units a 50 ms tick, x20: a
                    // position average reads low whenever it stops or turns,
                    // and position deltas spike on a teleport or summon,
                    // whose updates carry no velocity), with the spawn
                    // record's run/walk multipliers both stand on
                    // (0.7 / 0.30 for a player before buffs).
                    if (steer.active) {
                        static int nav_steps = 0;
                        static float seen_fastest = 0.0f;
                        {
                            // Sampled every step, whatever is logged later.
                            std::uint32_t want = navigator.status().stick_spawn;
                            if (!want && conn->world().target_id) want = conn->world().target_id;
                            if (!want) {
                                const std::string goal = navigator.status().goal;
                                for (const auto& [id, s] : conn->world().spawns)
                                    if (game::display_name(s.name) == goal) { want = id; break; }
                            }
                            if (const auto s = conn->world().spawns.find(want); want && s != conn->world().spawns.end())
                                seen_fastest = std::max(seen_fastest, glm::length(glm::vec2(s->second.velocity)) * 20.0f);
                        }
                        if (++nav_steps % 60 == 1) {
                            const float own = glm::length(glm::vec2(frame.velocity)) * 60.0f;
                            std::string target = "none";
                            float trun = 0.0f, twalk = 0.0f;
                            // The spawn being walked to (Navigator's stick
                            // spawn, else its goal by name), else the target:
                            // a chase rarely leaves the leader targeted.
                            std::uint32_t goal_id = navigator.status().stick_spawn;
                            if (!goal_id && conn->world().target_id) goal_id = conn->world().target_id;
                            if (!goal_id) {
                                const std::string goal = navigator.status().goal;
                                for (const auto& [id, s] : conn->world().spawns)
                                    if (game::display_name(s.name) == goal) { goal_id = id; break; }
                            }
                            if (const auto t = conn->world().spawns.find(goal_id);
                                goal_id && t != conn->world().spawns.end()) {
                                target = game::display_name(t->second.name);
                                trun = t->second.run_speed;
                                twalk = t->second.walk_speed;
                            }
                            log::info("headless nav: own {:.1f} u/s (record run {:.2f} walk {:.2f}); target {} "
                                      "fastest {:.1f} u/s (record run {:.2f} walk {:.2f})",
                                      own, me->run_speed, me->walk_speed, target, seen_fastest, trun, twalk);
                            seen_fastest = 0.0f;
                        }
                    }
                    nav_was_active = steer.active;
                    for (const auto& line : frame.messages) log::info("movement: {}", line);
                    if (frame.stand_state) conn->set_stand_state(*frame.stand_state);
                    if (frame.sit_toggle) conn->sit_toggle();
                    if (frame.close_loot) conn->end_loot();
                    conn->set_speed_run(frame.speed_run);
                    conn->set_player_motion(feet + glm::vec3(0, 0, floor_offset), game::yaw_to_heading(yaw),
                                            frame.velocity, frame.moving);
                    conn->set_air_supply(movement.air_supply());
                    conn->set_standing_on(movement.standing_on());
                    if (zone)
                        if (auto text = zone_line.update(feet, zone->regions)) region_teleport(*text);
                }
            }
            if (options.seconds > 0 && Clock::now() - in_zone_at > std::chrono::seconds(options.seconds))
                conn->camp();
        }
        if (last == net::ConnectionState::Disconnected) {
            // 3 is the crew supervisor's relaunch signal: the link died
            // under a character that was in the world. A camp is 0.
            result = conn->error() == "Camped out." ? 0 : conn->link_death() ? 3 : 1;
            break;
        }
        if (!options.status_file.empty() && Clock::now() - last_status > std::chrono::seconds(1)) {
            last_status = Clock::now();
            std::ofstream(options.status_file, std::ios::trunc)
                << "state=" << net::to_string(last) << " zone=" << zone_seen << " character=" << options.character << "\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (stop_requested) log::info("headless: stop requested; shutting down");
    if (headless_imgui) headless_imgui->flush_ini();
    automation.shutdown();
    if (conn) conn->disconnect();
    return stop_requested ? 0 : result;
}

} // namespace hailstone::app
