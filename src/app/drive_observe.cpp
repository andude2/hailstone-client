#include "app/drive_observe.h"

#include "core/byte_io.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace hailstone::app {
namespace {

constexpr std::size_t kObserveQueueLimit = std::size_t{32} << 20;
constexpr std::size_t kObserveSnapshotChunk = std::size_t{1} << 20;
constexpr std::size_t kMaxCommandLen = 2048;
constexpr std::size_t kMaxChatLen = 4096;
constexpr std::size_t kMaxXTargetSlots = 64;
constexpr std::size_t kMaxRemoteMouseEvents = 64;
constexpr std::size_t kMaxRemoteKeyEvents = 128;
constexpr std::size_t kMaxRemoteChars = 128;
constexpr std::size_t kMaxRemoteDrawLists = 64;
constexpr std::size_t kMaxRemoteVertices = 262144;
constexpr std::size_t kMaxRemoteIndices = 524288;
constexpr std::size_t kMaxRemoteCmds = 8192;
constexpr std::uint16_t kMaxRemoteTextureDim = 4096;
constexpr std::size_t kMaxRemoteTextureBytes = std::size_t{6} << 20;

void put_packet(ByteWriter& w, const net::AppPacket& packet) {
    w.le(packet.opcode);
    w.le(static_cast<std::uint32_t>(packet.data.size()));
    w.bytes(packet.data);
}

void put_string(ByteWriter& w, std::string_view s) {
    w.le(static_cast<std::uint16_t>(s.size()));
    w.bytes(std::span(reinterpret_cast<const std::byte*>(s.data()), s.size()));
}

std::string get_string(ByteReader& r, std::size_t max_len) {
    const auto len = r.le<std::uint16_t>();
    if (len > max_len) throw std::out_of_range("drive string too long");
    const auto bytes = r.bytes(len);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool send_message(platform::LocalPipeConnection& client, DriveObserveMessage kind,
                  std::span<const std::byte> body) {
    ByteWriter w;
    w.le(static_cast<std::uint8_t>(kind));
    w.bytes(body);
    const auto& bytes = w.data();
    return client.send_message(std::span<const std::byte>(bytes.data(), bytes.size()), 5000);
}

bool send_snapshot_frames(platform::LocalPipeConnection& client, const std::vector<std::byte>& encoded) {
    if (encoded.size() > net::kDriveSnapshotMaxBytes) return false;
    if (encoded.size() <= kObserveSnapshotChunk) {
        return send_message(client, DriveObserveMessage::Snapshot, encoded);
    }
    const auto total = static_cast<std::uint32_t>(encoded.size());
    for (std::size_t offset = 0; offset < encoded.size(); offset += kObserveSnapshotChunk) {
        const std::size_t len = std::min(kObserveSnapshotChunk, encoded.size() - offset);
        ByteWriter w;
        w.le(total);
        w.le(static_cast<std::uint32_t>(offset));
        w.bytes(std::span<const std::byte>(encoded.data() + offset, len));
        if (!send_message(client, DriveObserveMessage::SnapshotChunk, w.take())) return false;
    }
    return true;
}

} // namespace

std::vector<std::byte> encode_drive_packet(const net::AppPacket& packet) {
    ByteWriter w;
    put_packet(w, packet);
    return w.take();
}

std::optional<net::AppPacket> decode_drive_packet(std::span<const std::byte> body) {
    try {
        ByteReader r(body);
        net::AppPacket packet;
        packet.opcode = r.le<std::uint16_t>();
        const auto size = r.le<std::uint32_t>();
        if (size > net::kDriveSnapshotMaxBytes) return std::nullopt;
        const auto data = r.bytes(size);
        packet.data.assign(data.begin(), data.end());
        if (!r.empty()) return std::nullopt;
        return packet;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<std::byte> encode_drive_action(const net::DriveAction& action) {
    ByteWriter w;
    w.le(static_cast<std::uint8_t>(DriveObserveMessage::Action));
    w.le(static_cast<std::uint8_t>(action.kind));
    net::write_slot(w, action.slot);
    w.le(action.argument);
    w.le(action.socket);
    w.le(action.zone_generation);
    w.le(action.inventory_revision);
    w.le(action.dialog);
    w.fixed_string(action.item_serial, 17);
    if (action.kind == net::DriveActionKind::Command) {
        const std::string_view text =
            action.text.size() > kMaxCommandLen ? std::string_view(action.text).substr(0, kMaxCommandLen) : action.text;
        put_string(w, text);
    }
    return w.take();
}

std::optional<net::DriveAction> decode_drive_action(std::span<const std::byte> body) {
    try {
        ByteReader r(body);
        if (static_cast<DriveObserveMessage>(r.le<std::uint8_t>()) != DriveObserveMessage::Action) return std::nullopt;
        net::DriveAction action;
        action.kind = static_cast<net::DriveActionKind>(r.le<std::uint8_t>());
        action.slot = net::read_slot(r);
        action.argument = r.le<std::uint32_t>();
        action.socket = r.le<std::int32_t>();
        action.zone_generation = r.le<std::uint32_t>();
        action.inventory_revision = r.le<std::uint64_t>();
        action.dialog = r.le<std::uint64_t>();
        action.item_serial = r.fixed_string(17);
        if (action.kind > net::DriveActionKind::Loot) return std::nullopt;
        if (action.kind == net::DriveActionKind::Command) action.text = get_string(r, kMaxCommandLen);
        if (!r.empty()) return std::nullopt;
        return action;
    } catch (const std::out_of_range&) {
        return std::nullopt;
    }
}

std::vector<std::byte> encode_drive_move(const net::DriveMove& move) {
    ByteWriter w;
    w.le(static_cast<std::uint8_t>(DriveObserveMessage::Move));
    w.f32(move.position.x);
    w.f32(move.position.y);
    w.f32(move.position.z);
    w.f32(move.heading);
    w.f32(move.velocity.x);
    w.f32(move.velocity.y);
    w.f32(move.velocity.z);
    w.f32(move.speed_run);
    w.le(static_cast<std::uint8_t>((move.moving ? 1u : 0u) | (move.active ? 2u : 0u)));
    w.le(move.stand_state);
    w.le(move.standing_on);
    return w.take();
}

std::optional<net::DriveMove> decode_drive_move(std::span<const std::byte> body) {
    try {
        ByteReader r(body);
        if (static_cast<DriveObserveMessage>(r.le<std::uint8_t>()) != DriveObserveMessage::Move) return std::nullopt;
        net::DriveMove move;
        move.position.x = r.f32();
        move.position.y = r.f32();
        move.position.z = r.f32();
        move.heading = r.f32();
        move.velocity.x = r.f32();
        move.velocity.y = r.f32();
        move.velocity.z = r.f32();
        move.speed_run = r.f32();
        const auto flags = r.le<std::uint8_t>();
        move.moving = (flags & 1u) != 0;
        move.active = (flags & 2u) != 0;
        move.stand_state = r.le<std::uint8_t>();
        move.standing_on = r.le<std::uint32_t>();
        if (!r.empty()) return std::nullopt;
        return move;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<std::byte> encode_drive_forward(const DriveForwardedPacket& fwd) {
    ByteWriter w;
    w.le(static_cast<std::uint8_t>(fwd.world ? DriveObserveMessage::ForwardWorld : DriveObserveMessage::ForwardZone));
    w.le(static_cast<std::uint8_t>(fwd.reliable ? 1u : 0u));
    put_packet(w, fwd.packet);
    return w.take();
}

std::optional<DriveForwardedPacket> decode_drive_forward(std::span<const std::byte> body) {
    try {
        ByteReader r(body);
        const auto kind = static_cast<DriveObserveMessage>(r.le<std::uint8_t>());
        if (kind != DriveObserveMessage::ForwardZone && kind != DriveObserveMessage::ForwardWorld) return std::nullopt;
        DriveForwardedPacket fwd;
        fwd.world = kind == DriveObserveMessage::ForwardWorld;
        fwd.reliable = r.le<std::uint8_t>() != 0;
        fwd.packet.opcode = r.le<std::uint16_t>();
        const auto size = r.le<std::uint32_t>();
        if (size > net::kDriveSnapshotMaxBytes) return std::nullopt;
        const auto data = r.bytes(size);
        fwd.packet.data.assign(data.begin(), data.end());
        if (!r.empty()) return std::nullopt;
        return fwd;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<std::byte> encode_drive_chat(const DriveChatLine& line) {
    ByteWriter w;
    w.le(line.kind);
    w.le(line.rgb);
    w.le(line.color);
    const std::string_view peer = line.peer.size() > 128 ? std::string_view(line.peer).substr(0, 128) : line.peer;
    const std::string_view text =
        line.text.size() > kMaxChatLen ? std::string_view(line.text).substr(0, kMaxChatLen) : line.text;
    put_string(w, peer);
    put_string(w, text);
    return w.take();
}

std::optional<DriveChatLine> decode_drive_chat(std::span<const std::byte> body) {
    try {
        ByteReader r(body);
        DriveChatLine line;
        line.kind = r.le<std::uint8_t>();
        line.rgb = r.le<std::uint32_t>();
        line.color = r.le<std::uint32_t>();
        line.peer = get_string(r, 128);
        line.text = get_string(r, kMaxChatLen);
        if (!r.empty()) return std::nullopt;
        return line;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<std::byte> encode_drive_state(const DriveLocalState& state) {
    ByteWriter w;
    w.f32(state.position.x);
    w.f32(state.position.y);
    w.f32(state.position.z);
    w.f32(state.heading);
    w.f32(state.velocity.x);
    w.f32(state.velocity.y);
    w.f32(state.velocity.z);
    w.le(static_cast<std::uint8_t>(state.moving ? 1 : 0));
    w.le(state.target);
    w.le(static_cast<std::uint8_t>(state.inventory_packet ? 1 : 0));
    if (state.inventory_packet) put_packet(w, *state.inventory_packet);
    const bool has_tail = state.has_status || state.has_xtargets;
    if (state.ui || has_tail) {
        const net::DriveUiState empty_ui{};
        const net::DriveUiState& ui = state.ui ? *state.ui : empty_ui;
        w.le(ui.zone_generation);
        w.le(ui.inventory_revision);
        w.le(static_cast<std::uint8_t>(ui.question.has_value()));
        if (const auto& q = ui.question) {
            w.le(q->id);
            w.le(q->kind);
            w.le(static_cast<std::uint8_t>((q->plain_text ? 1 : 0) | (q->ok_only ? 2 : 0)));
            w.fixed_string(q->text, q->text.size() + 1);
        }
        w.le(static_cast<std::uint8_t>(ui.quantity.has_value()));
        if (const auto& q = ui.quantity) {
            net::write_slot(w, q->slot);
            w.le(q->max);
            w.le(q->start);
        }
    }
    if (has_tail) {
        w.le(static_cast<std::uint8_t>(state.has_status ? 1 : 0));
        if (state.has_status) {
            w.le(state.stand_state);
            w.le(state.owner_status);
            const std::uint8_t flags = static_cast<std::uint8_t>(
                (state.dead ? 1u : 0u) | (state.auto_attack ? 2u : 0u) | (state.auto_fire ? 4u : 0u) |
                (state.running ? 8u : 0u));
            w.le(flags);
            w.le(state.hp);
            w.le(state.max_hp);
            w.le(state.mana);
            w.le(state.max_mana);
            w.le(state.endurance);
            w.le(state.max_endurance);
        }
        w.le(static_cast<std::uint8_t>(state.has_xtargets ? 1 : 0));
        if (state.has_xtargets) {
            w.le(static_cast<std::uint8_t>(state.xtarget_auto_haters ? 1 : 0));
            w.le(state.xtarget_zone);
            const std::size_t n = std::min<std::size_t>(state.xtarget_slots.size(), kMaxXTargetSlots);
            w.le(static_cast<std::uint32_t>(n));
            for (std::size_t i = 0; i < n; ++i) {
                const auto& s = state.xtarget_slots[i];
                w.le(s.type);
                w.le(s.status);
                w.le(s.spawn_id);
                put_string(w, s.name.size() > 128 ? std::string_view(s.name).substr(0, 128) : s.name);
            }
        }
    }
    return w.take();
}

std::optional<DriveLocalState> decode_drive_state(std::span<const std::byte> body) {
    try {
        ByteReader r(body);
        DriveLocalState state;
        state.position.x = r.f32();
        state.position.y = r.f32();
        state.position.z = r.f32();
        state.heading = r.f32();
        state.velocity.x = r.f32();
        state.velocity.y = r.f32();
        state.velocity.z = r.f32();
        state.moving = r.le<std::uint8_t>() != 0;
        state.target = r.le<std::uint32_t>();
        if (r.le<std::uint8_t>() != 0) {
            net::AppPacket packet;
            packet.opcode = r.le<std::uint16_t>();
            const auto size = r.le<std::uint32_t>();
            if (size > net::kDriveSnapshotMaxBytes) return std::nullopt;
            const auto data = r.bytes(size);
            packet.data.assign(data.begin(), data.end());
            state.inventory_packet = std::move(packet);
        }
        if (!r.empty()) {
            net::DriveUiState ui;
            ui.zone_generation = r.le<std::uint32_t>();
            ui.inventory_revision = r.le<std::uint64_t>();
            if (r.le<std::uint8_t>()) {
                net::DriveQuestion q;
                q.id = r.le<std::uint64_t>();
                q.kind = r.le<std::uint32_t>();
                const auto flags = r.le<std::uint8_t>();
                q.plain_text = (flags & 1) != 0;
                q.ok_only = (flags & 2) != 0;
                q.text = r.cstring();
                ui.question = std::move(q);
            }
            if (r.le<std::uint8_t>()) {
                net::QuantityRequest q;
                q.slot = net::read_slot(r);
                q.max = r.le<std::uint32_t>();
                q.start = r.le<std::uint32_t>();
                ui.quantity = q;
            }
            state.ui = std::move(ui);
        }
        if (!r.empty()) {
            state.has_status = r.le<std::uint8_t>() != 0;
            if (state.has_status) {
                state.stand_state = r.le<std::uint8_t>();
                state.owner_status = r.le<std::int8_t>();
                const auto flags = r.le<std::uint8_t>();
                state.dead = (flags & 1u) != 0;
                state.auto_attack = (flags & 2u) != 0;
                state.auto_fire = (flags & 4u) != 0;
                state.running = (flags & 8u) != 0;
                state.hp = r.le<std::int64_t>();
                state.max_hp = r.le<std::int64_t>();
                state.mana = r.le<std::uint32_t>();
                state.max_mana = r.le<std::uint32_t>();
                state.endurance = r.le<std::uint32_t>();
                state.max_endurance = r.le<std::uint32_t>();
            }
            state.has_xtargets = r.le<std::uint8_t>() != 0;
            if (state.has_xtargets) {
                state.xtarget_auto_haters = r.le<std::uint8_t>() != 0;
                state.xtarget_zone = r.le<std::uint32_t>();
                const auto n = r.le<std::uint32_t>();
                if (n > kMaxXTargetSlots) return std::nullopt;
                state.xtarget_slots.reserve(n);
                for (std::uint32_t i = 0; i < n; ++i) {
                    net::DriveXTargetSlot slot;
                    slot.type = r.le<std::uint32_t>();
                    slot.status = r.le<std::uint32_t>();
                    slot.spawn_id = r.le<std::uint32_t>();
                    slot.name = get_string(r, 128);
                    state.xtarget_slots.push_back(std::move(slot));
                }
            }
        }
        if (!r.empty()) return std::nullopt;
        return state;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<std::byte> encode_drive_imgui_input(const ui::RemoteImGuiInput& input) {
    ByteWriter w;
    w.le(static_cast<std::uint8_t>(DriveObserveMessage::ImGuiInput));
    w.f32(input.display_w);
    w.f32(input.display_h);
    w.f32(input.fb_scale_x);
    w.f32(input.fb_scale_y);
    w.f32(input.ui_scale);
    w.f32(input.dt);
    w.f32(input.mouse_x);
    w.f32(input.mouse_y);
    w.f32(input.wheel_x);
    w.f32(input.wheel_y);
    w.le(input.mods);
    w.le(static_cast<std::uint8_t>((input.mouse_valid ? 1u : 0u) | (input.focused ? 2u : 0u)));
    const std::size_t nb = std::min(input.mouse_buttons.size(), kMaxRemoteMouseEvents);
    w.le(static_cast<std::uint16_t>(nb));
    for (std::size_t i = 0; i < nb; ++i) {
        w.le(input.mouse_buttons[i].button);
        w.le(static_cast<std::uint8_t>(input.mouse_buttons[i].down ? 1u : 0u));
    }
    const std::size_t nk = std::min(input.keys.size(), kMaxRemoteKeyEvents);
    w.le(static_cast<std::uint16_t>(nk));
    for (std::size_t i = 0; i < nk; ++i) {
        w.le(input.keys[i].key);
        w.le(static_cast<std::uint8_t>(input.keys[i].down ? 1u : 0u));
    }
    const std::size_t nc = std::min(input.chars.size(), kMaxRemoteChars);
    w.le(static_cast<std::uint16_t>(nc));
    for (std::size_t i = 0; i < nc; ++i) w.le(input.chars[i]);
    return w.take();
}

std::optional<ui::RemoteImGuiInput> decode_drive_imgui_input(std::span<const std::byte> body) {
    try {
        ByteReader r(body);
        if (static_cast<DriveObserveMessage>(r.le<std::uint8_t>()) != DriveObserveMessage::ImGuiInput)
            return std::nullopt;
        ui::RemoteImGuiInput out;
        out.display_w = r.f32();
        out.display_h = r.f32();
        out.fb_scale_x = r.f32();
        out.fb_scale_y = r.f32();
        out.ui_scale = r.f32();
        out.dt = r.f32();
        out.mouse_x = r.f32();
        out.mouse_y = r.f32();
        out.wheel_x = r.f32();
        out.wheel_y = r.f32();
        out.mods = r.le<std::uint8_t>();
        const auto flags = r.le<std::uint8_t>();
        out.mouse_valid = (flags & 1u) != 0;
        out.focused = (flags & 2u) != 0;
        const auto nb = r.le<std::uint16_t>();
        if (nb > kMaxRemoteMouseEvents) return std::nullopt;
        out.mouse_buttons.reserve(nb);
        for (std::uint16_t i = 0; i < nb; ++i) {
            const auto btn = r.le<std::uint8_t>();
            const bool down = r.le<std::uint8_t>() != 0;
            out.mouse_buttons.push_back({btn, down});
        }
        const auto nk = r.le<std::uint16_t>();
        if (nk > kMaxRemoteKeyEvents) return std::nullopt;
        out.keys.reserve(nk);
        for (std::uint16_t i = 0; i < nk; ++i) {
            const auto key = r.le<std::uint16_t>();
            const bool down = r.le<std::uint8_t>() != 0;
            out.keys.push_back({key, down});
        }
        const auto nc = r.le<std::uint16_t>();
        if (nc > kMaxRemoteChars) return std::nullopt;
        out.chars.reserve(nc);
        for (std::uint16_t i = 0; i < nc; ++i) out.chars.push_back(r.le<std::uint32_t>());
        if (!r.empty()) return std::nullopt;
        return out;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<std::byte> encode_drive_imgui_frame(const ui::RemoteImGuiFrame& frame) {
    ByteWriter w;
    w.f32(frame.display_w);
    w.f32(frame.display_h);
    w.f32(frame.fb_scale_x);
    w.f32(frame.fb_scale_y);
    const std::uint8_t flags = static_cast<std::uint8_t>(
        (frame.want_capture_mouse ? 1u : 0u) | (frame.want_capture_keyboard ? 2u : 0u) |
        (frame.want_text_input ? 4u : 0u));
    w.le(flags);
    w.le(frame.mouse_cursor);
    const std::size_t nl = std::min(frame.lists.size(), kMaxRemoteDrawLists);
    w.le(static_cast<std::uint16_t>(nl));
    std::size_t total_v = 0, total_i = 0, total_c = 0;
    for (std::size_t l = 0; l < nl; ++l) {
        const auto& dl = frame.lists[l];
        const std::size_t nv = std::min(dl.vertices.size(), kMaxRemoteVertices - total_v);
        const std::size_t ni = std::min(dl.indices.size(), kMaxRemoteIndices - total_i);
        const std::size_t nc = std::min(dl.cmds.size(), kMaxRemoteCmds - total_c);
        total_v += nv;
        total_i += ni;
        total_c += nc;
        w.le(static_cast<std::uint32_t>(nv));
        for (std::size_t i = 0; i < nv; ++i) {
            const auto& v = dl.vertices[i];
            w.f32(v.x);
            w.f32(v.y);
            w.f32(v.u);
            w.f32(v.v);
            w.le(v.col);
        }
        w.le(static_cast<std::uint32_t>(ni));
        for (std::size_t i = 0; i < ni; ++i) w.le(dl.indices[i]);
        w.le(static_cast<std::uint32_t>(nc));
        for (std::size_t i = 0; i < nc; ++i) {
            const auto& c = dl.cmds[i];
            w.f32(c.clip_x);
            w.f32(c.clip_y);
            w.f32(c.clip_z);
            w.f32(c.clip_w);
            w.le(c.tex_id);
            w.le(c.vtx_offset);
            w.le(c.idx_offset);
            w.le(c.elem_count);
        }
    }
    return w.take();
}

std::optional<ui::RemoteImGuiFrame> decode_drive_imgui_frame(std::span<const std::byte> body) {
    try {
        ByteReader r(body);
        ui::RemoteImGuiFrame out;
        out.display_w = r.f32();
        out.display_h = r.f32();
        out.fb_scale_x = r.f32();
        out.fb_scale_y = r.f32();
        const auto flags = r.le<std::uint8_t>();
        out.want_capture_mouse = (flags & 1u) != 0;
        out.want_capture_keyboard = (flags & 2u) != 0;
        out.want_text_input = (flags & 4u) != 0;
        out.mouse_cursor = r.le<std::int8_t>();
        const auto nl = r.le<std::uint16_t>();
        if (nl > kMaxRemoteDrawLists) return std::nullopt;
        out.lists.reserve(nl);
        std::size_t total_v = 0, total_i = 0, total_c = 0;
        for (std::uint16_t l = 0; l < nl; ++l) {
            ui::RemoteDrawList dl;
            const auto nv = r.le<std::uint32_t>();
            if (nv > kMaxRemoteVertices - total_v) return std::nullopt;
            total_v += nv;
            dl.vertices.resize(nv);
            for (std::uint32_t i = 0; i < nv; ++i) {
                auto& v = dl.vertices[i];
                v.x = r.f32();
                v.y = r.f32();
                v.u = r.f32();
                v.v = r.f32();
                v.col = r.le<std::uint32_t>();
            }
            const auto ni = r.le<std::uint32_t>();
            if (ni > kMaxRemoteIndices - total_i) return std::nullopt;
            total_i += ni;
            dl.indices.resize(ni);
            for (std::uint32_t i = 0; i < ni; ++i) dl.indices[i] = r.le<std::uint16_t>();
            const auto nc = r.le<std::uint32_t>();
            if (nc > kMaxRemoteCmds - total_c) return std::nullopt;
            total_c += nc;
            dl.cmds.resize(nc);
            for (std::uint32_t i = 0; i < nc; ++i) {
                auto& c = dl.cmds[i];
                c.clip_x = r.f32();
                c.clip_y = r.f32();
                c.clip_z = r.f32();
                c.clip_w = r.f32();
                c.tex_id = r.le<std::uint32_t>();
                c.vtx_offset = r.le<std::uint32_t>();
                c.idx_offset = r.le<std::uint32_t>();
                c.elem_count = r.le<std::uint32_t>();
            }
            out.lists.push_back(std::move(dl));
        }
        if (!r.empty()) return std::nullopt;
        return out;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<std::byte> encode_drive_imgui_texture(const ui::RemoteImGuiTexture& tex) {
    ByteWriter w;
    w.le(tex.id);
    w.le(tex.tex_w);
    w.le(tex.tex_h);
    w.le(tex.x);
    w.le(tex.y);
    w.le(tex.w);
    w.le(tex.h);
    w.le(static_cast<std::uint8_t>((tex.full ? 1u : 0u) | (tex.destroy ? 2u : 0u)));
    w.le(static_cast<std::uint32_t>(tex.rgba.size()));
    if (!tex.rgba.empty()) w.bytes(tex.rgba);
    return w.take();
}

std::optional<ui::RemoteImGuiTexture> decode_drive_imgui_texture(std::span<const std::byte> body) {
    try {
        ByteReader r(body);
        ui::RemoteImGuiTexture out;
        out.id = r.le<std::uint32_t>();
        out.tex_w = r.le<std::uint16_t>();
        out.tex_h = r.le<std::uint16_t>();
        out.x = r.le<std::uint16_t>();
        out.y = r.le<std::uint16_t>();
        out.w = r.le<std::uint16_t>();
        out.h = r.le<std::uint16_t>();
        const auto flags = r.le<std::uint8_t>();
        out.full = (flags & 1u) != 0;
        out.destroy = (flags & 2u) != 0;
        const auto size = r.le<std::uint32_t>();
        if (out.destroy) {
            if (size != 0) return std::nullopt;
        } else {
            if (out.tex_w == 0 || out.tex_h == 0 || out.tex_w > kMaxRemoteTextureDim ||
                out.tex_h > kMaxRemoteTextureDim)
                return std::nullopt;
            if (static_cast<std::uint32_t>(out.x) + out.w > out.tex_w ||
                static_cast<std::uint32_t>(out.y) + out.h > out.tex_h)
                return std::nullopt;
            const std::size_t expected = static_cast<std::size_t>(out.w) * out.h * 4u;
            if (size != expected || size > kMaxRemoteTextureBytes) return std::nullopt;
            const auto pixels = r.bytes(size);
            out.rgba.assign(pixels.begin(), pixels.end());
        }
        if (!r.empty()) return std::nullopt;
        return out;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<std::byte> encode_drive_crew_command(std::string_view args) {
    ByteWriter w;
    const std::string_view clipped = args.size() > kMaxCommandLen ? args.substr(0, kMaxCommandLen) : args;
    put_string(w, clipped);
    return w.take();
}

std::optional<std::string> decode_drive_crew_command(std::span<const std::byte> body) {
    try {
        ByteReader r(body);
        std::string out = get_string(r, kMaxCommandLen);
        if (!r.empty()) return std::nullopt;
        return out;
    } catch (...) {
        return std::nullopt;
    }
}

struct DriveObserveServer::Impl {
    platform::LocalPipeServer server;
    platform::LocalPipeSession session;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> snapshot_requested{false};
    std::atomic<bool> viewer_active{false};
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<net::DriveSnapshot> snapshot;
    std::deque<std::pair<DriveObserveMessage, std::vector<std::byte>>> queue;
    std::size_t queued_bytes = 0;
    bool client_active = false;
    bool queue_overflow = false;
    const bool interactive;
    const bool control;
    std::vector<net::DriveAction> actions;
    std::optional<net::DriveMove> latest_move;
    std::vector<DriveForwardedPacket> forwarded;
    std::optional<ui::RemoteImGuiInput> pending_imgui_input;

    Impl(platform::LocalPipeSession s, bool allow_input, bool allow_control)
        : server(s), session(server.session()), interactive(allow_input || allow_control), control(allow_control) {
        thread = std::thread([this] { run(); });
    }

    ~Impl() { stop_server(); }

    void stop_server() {
        stop.store(true);
        wake.notify_all();
        if (thread.joinable() && thread.get_id() != std::this_thread::get_id()) thread.join();
    }

    bool send(platform::LocalPipeConnection& client, DriveObserveMessage kind,
              std::span<const std::byte> body) {
        return send_message(client, kind, body);
    }

    void handle_input(std::span<const std::byte> input) {
        if (input.empty()) return;
        const auto kind = static_cast<DriveObserveMessage>(std::to_integer<std::uint8_t>(input[0]));
        if (kind == DriveObserveMessage::ImGuiInput) {
            if (auto in = decode_drive_imgui_input(input)) {
                if (!interactive) {
                    in->mouse_valid = false;
                    in->wheel_x = 0.0f;
                    in->wheel_y = 0.0f;
                    in->mods = 0;
                    in->mouse_buttons.clear();
                    in->keys.clear();
                    in->chars.clear();
                }
                std::lock_guard lock(mutex);
                if (!pending_imgui_input) {
                    pending_imgui_input = std::move(*in);
                } else {
                    auto& dst = *pending_imgui_input;
                    dst.display_w = in->display_w;
                    dst.display_h = in->display_h;
                    dst.fb_scale_x = in->fb_scale_x;
                    dst.fb_scale_y = in->fb_scale_y;
                    dst.ui_scale = in->ui_scale;
                    dst.dt = std::min(dst.dt + in->dt, 0.25f);
                    dst.mouse_x = in->mouse_x;
                    dst.mouse_y = in->mouse_y;
                    dst.mouse_valid = in->mouse_valid;
                    dst.focused = in->focused;
                    dst.mods = in->mods;
                    dst.wheel_x += in->wheel_x;
                    dst.wheel_y += in->wheel_y;
                    for (const auto& e : in->mouse_buttons)
                        if (dst.mouse_buttons.size() < kMaxRemoteMouseEvents) dst.mouse_buttons.push_back(e);
                    for (const auto& e : in->keys)
                        if (dst.keys.size() < kMaxRemoteKeyEvents) dst.keys.push_back(e);
                    for (std::uint32_t c : in->chars)
                        if (dst.chars.size() < kMaxRemoteChars) dst.chars.push_back(c);
                }
            }
            return;
        }
        if (!interactive) return;
        if (kind == DriveObserveMessage::Action) {
            if (auto action = decode_drive_action(input)) {
                std::lock_guard lock(mutex);
                const std::size_t limit = control ? 64u : 16u;
                if (actions.size() < limit) actions.push_back(std::move(*action));
            }
        } else if (kind == DriveObserveMessage::Move && control) {
            if (auto move = decode_drive_move(input)) {
                std::lock_guard lock(mutex);
                latest_move = *move;
            }
        } else if ((kind == DriveObserveMessage::ForwardZone || kind == DriveObserveMessage::ForwardWorld) && control) {
            if (auto fwd = decode_drive_forward(input)) {
                if (net::drive_forward_allowed(fwd->packet, fwd->world)) {
                    std::lock_guard lock(mutex);
                    if (forwarded.size() < 64) forwarded.push_back(std::move(*fwd));
                }
            }
        }
    }

    void run() {
        while (!stop.load()) {
            auto client = server.accept(200);
            if (!client) continue;
            snapshot_requested.store(true);
            wake.notify_all();

            std::optional<net::DriveSnapshot> initial;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [this] { return stop.load() || snapshot.has_value(); });
                if (stop.load()) return;
                initial = std::move(snapshot);
                snapshot.reset();
                snapshot_requested.store(false);
            }
            if (!send_snapshot_frames(*client, net::encode_drive_snapshot(*initial))) {
                std::lock_guard lock(mutex);
                client_active = false;
                viewer_active.store(false);
                continue;
            }
            viewer_active.store(true);

            while (!stop.load() && !client->closed()) {
                while (auto input = client->receive_message(0)) {
                    handle_input(std::span<const std::byte>(input->data(), input->size()));
                }
                std::optional<std::pair<DriveObserveMessage, std::vector<std::byte>>> message;
                {
                    std::unique_lock lock(mutex);
                    if (queue_overflow) break;
                    if (!queue.empty()) {
                        message = std::move(queue.front());
                        queue.pop_front();
                        queued_bytes -= message->second.size();
                    } else {
                        wake.wait_for(lock, std::chrono::milliseconds(interactive ? 5 : 20));
                    }
                }
                if (message && !send(*client, message->first, message->second)) break;
            }
            client->close();
            viewer_active.store(false);
            {
                std::lock_guard lock(mutex);
                client_active = false;
                actions.clear();
                latest_move.reset();
                forwarded.clear();
                pending_imgui_input.reset();
                queue.clear();
                queued_bytes = 0;
            }
        }
    }

    void enqueue(DriveObserveMessage kind, std::vector<std::byte> body) {
        std::lock_guard lock(mutex);
        if (!client_active) return;
        if (kind == DriveObserveMessage::ImGuiFrame) {
            // Coalesce queued UI frames so a slow viewer never builds latency or overflows the queue.
            for (auto it = queue.rbegin(); it != queue.rend(); ++it) {
                if (it->first == DriveObserveMessage::ImGuiFrame) {
                    queued_bytes -= it->second.size();
                    it->second = std::move(body);
                    queued_bytes += it->second.size();
                    wake.notify_one();
                    return;
                }
            }
        }
        if (queued_bytes + body.size() > kObserveQueueLimit) {
            queue_overflow = true; // disconnect rather than advance the view past a missing packet
            wake.notify_one();
            return;
        }
        queued_bytes += body.size();
        queue.emplace_back(kind, std::move(body));
        wake.notify_one();
    }

    void enqueue_snapshot(const net::DriveSnapshot& next_snapshot) {
        const auto encoded = net::encode_drive_snapshot(next_snapshot);
        if (encoded.size() > net::kDriveSnapshotMaxBytes) return;
        std::lock_guard lock(mutex);
        if (!client_active) return;
        // Replace stale pre-zone packets with the new zone's snapshot, keeping ordered UI texture updates intact.
        std::deque<std::pair<DriveObserveMessage, std::vector<std::byte>>> kept;
        std::size_t kept_bytes = 0;
        for (auto& item : queue) {
            if (item.first == DriveObserveMessage::ImGuiTexture || item.first == DriveObserveMessage::CrewCommand) {
                kept_bytes += item.second.size();
                kept.push_back(std::move(item));
            }
        }
        queue = std::move(kept);
        queued_bytes = kept_bytes;
        queue_overflow = false;
        if (encoded.size() <= kObserveSnapshotChunk) {
            queued_bytes += encoded.size();
            queue.emplace_back(DriveObserveMessage::Snapshot, encoded);
        } else {
            const auto total = static_cast<std::uint32_t>(encoded.size());
            for (std::size_t offset = 0; offset < encoded.size(); offset += kObserveSnapshotChunk) {
                const std::size_t len = std::min(kObserveSnapshotChunk, encoded.size() - offset);
                ByteWriter w;
                w.le(total);
                w.le(static_cast<std::uint32_t>(offset));
                w.bytes(std::span<const std::byte>(encoded.data() + offset, len));
                auto chunk = w.take();
                queued_bytes += chunk.size();
                queue.emplace_back(DriveObserveMessage::SnapshotChunk, std::move(chunk));
            }
        }
        wake.notify_one();
    }
};

DriveObserveServer::DriveObserveServer(platform::LocalPipeSession session, bool interactive, bool control)
    : impl_(std::make_unique<Impl>(std::move(session), interactive, control)) {}

DriveObserveServer::~DriveObserveServer() { stop(); }

const platform::LocalPipeSession& DriveObserveServer::session() const { return impl_->session; }

bool DriveObserveServer::needs_snapshot() const { return impl_->snapshot_requested.load(); }

bool DriveObserveServer::viewer_connected() const { return impl_->viewer_active.load(); }

void DriveObserveServer::set_snapshot(net::DriveSnapshot snapshot) {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->snapshot = std::move(snapshot);
        impl_->queue.clear(); // this snapshot includes everything through the current game-thread tick
        impl_->queued_bytes = 0;
        impl_->queue_overflow = false;
        impl_->client_active = true;
        impl_->snapshot_requested.store(false);
    }
    impl_->wake.notify_all();
}

void DriveObserveServer::publish_snapshot(net::DriveSnapshot snapshot) { impl_->enqueue_snapshot(snapshot); }

void DriveObserveServer::publish_zone_packet(const net::AppPacket& packet) {
    impl_->enqueue(DriveObserveMessage::Packet, encode_drive_packet(packet));
}

void DriveObserveServer::publish_state(const DriveLocalState& state) {
    impl_->enqueue(DriveObserveMessage::State, encode_drive_state(state));
}

void DriveObserveServer::publish_chat(const DriveChatLine& line) {
    impl_->enqueue(DriveObserveMessage::Chat, encode_drive_chat(line));
}

void DriveObserveServer::publish_imgui_frame(const ui::RemoteImGuiFrame& frame) {
    impl_->enqueue(DriveObserveMessage::ImGuiFrame, encode_drive_imgui_frame(frame));
}

void DriveObserveServer::publish_imgui_texture(const ui::RemoteImGuiTexture& tex) {
    impl_->enqueue(DriveObserveMessage::ImGuiTexture, encode_drive_imgui_texture(tex));
}

void DriveObserveServer::publish_crew_command(std::string_view args) {
    impl_->enqueue(DriveObserveMessage::CrewCommand, encode_drive_crew_command(args));
}

std::vector<net::DriveAction> DriveObserveServer::take_actions() {
    std::lock_guard lock(impl_->mutex);
    return std::exchange(impl_->actions, {});
}

std::optional<net::DriveMove> DriveObserveServer::take_move() {
    std::lock_guard lock(impl_->mutex);
    return std::exchange(impl_->latest_move, std::nullopt);
}

std::vector<DriveForwardedPacket> DriveObserveServer::take_forwarded() {
    std::lock_guard lock(impl_->mutex);
    return std::exchange(impl_->forwarded, {});
}

std::optional<ui::RemoteImGuiInput> DriveObserveServer::take_imgui_input() {
    std::lock_guard lock(impl_->mutex);
    return std::exchange(impl_->pending_imgui_input, std::nullopt);
}

void DriveObserveServer::stop() {
    if (impl_) impl_->stop_server();
}

} // namespace hailstone::app
