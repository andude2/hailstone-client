#include "app/drive_client.h"

#include "app/drive_observe.h"
#include "core/byte_io.h"
#include "platform/local_pipe.h"

#include <deque>
#include <fstream>
#include <span>
#include <stdexcept>
#include <utility>

namespace hailstone::app {
namespace {

struct SnapshotAssembler {
    std::vector<std::byte> bytes;
    std::uint32_t total = 0;
    bool active = false;

    bool feed(std::uint32_t whole, std::uint32_t offset, std::span<const std::byte> data) {
        if (whole > net::kDriveSnapshotMaxBytes) return false;
        if (!active) {
            active = true;
            total = whole;
            bytes.clear();
            bytes.reserve(whole);
        }
        if (whole != total || offset != bytes.size()) return false;
        bytes.insert(bytes.end(), data.begin(), data.end());
        return true;
    }

    bool complete() const { return active && bytes.size() == total; }

    void reset() {
        bytes.clear();
        total = 0;
        active = false;
    }
};

} // namespace

struct DriveClient::Impl {
    std::unique_ptr<platform::LocalPipeConnection> pipe;
    DriveObserveFile file;
    net::DriveSnapshot snapshot;
    std::optional<net::DriveSnapshot> resnapshot;
    SnapshotAssembler snapshot_in;
    std::deque<net::AppPacket> packets;
    std::optional<net::DriveSnapshot> local;
    std::vector<DriveChatLine> chat;
    std::optional<ui::RemoteImGuiFrame> imgui_frame;
    std::vector<ui::RemoteImGuiTexture> imgui_textures;
    std::vector<std::string> crew_commands;
    bool closed = false;
    std::string error;

    Impl(std::unique_ptr<platform::LocalPipeConnection> p, DriveObserveFile f)
        : pipe(std::move(p)), file(std::move(f)) {}

    bool handle_snapshot_chunk(std::span<const std::byte> body, std::optional<net::DriveSnapshot>& out) {
        try {
            ByteReader r(body);
            const auto total = r.le<std::uint32_t>();
            const auto offset = r.le<std::uint32_t>();
            if (!snapshot_in.feed(total, offset, body.subspan(8))) {
                snapshot_in.reset();
                return false;
            }
            if (snapshot_in.complete()) {
                out = net::decode_drive_snapshot(snapshot_in.bytes);
                snapshot_in.reset();
                return out.has_value();
            }
            return true;
        } catch (...) {
            snapshot_in.reset();
            return false;
        }
    }

    void pump() {
        if (!pipe || closed || local || resnapshot) return;
        if (pipe->closed()) {
            closed = true;
            error = pipe->error();
            return;
        }
        while (auto message = pipe->receive_message(0)) {
            if (message->empty()) continue;
            const std::span<const std::byte> bytes(message->data(), message->size());
            ByteReader r(bytes);
            const auto kind = static_cast<DriveObserveMessage>(r.le<std::uint8_t>());
            const auto body = bytes.subspan(1);
            if (kind == DriveObserveMessage::Bye) {
                closed = true;
                break;
            }
            if (kind == DriveObserveMessage::Packet) {
                if (auto packet = decode_drive_packet(body)) packets.push_back(std::move(*packet));
            } else if (kind == DriveObserveMessage::Chat) {
                if (auto line = decode_drive_chat(body)) {
                    if (chat.size() < 256) chat.push_back(std::move(*line));
                }
            } else if (kind == DriveObserveMessage::ImGuiFrame) {
                if (auto frame = decode_drive_imgui_frame(body)) imgui_frame = std::move(*frame);
            } else if (kind == DriveObserveMessage::ImGuiTexture) {
                if (auto tex = decode_drive_imgui_texture(body)) {
                    if (imgui_textures.size() < 64) imgui_textures.push_back(std::move(*tex));
                }
            } else if (kind == DriveObserveMessage::CrewCommand) {
                if (auto cmd = decode_drive_crew_command(body)) {
                    if (crew_commands.size() < 16) crew_commands.push_back(std::move(*cmd));
                }
            } else if (kind == DriveObserveMessage::Snapshot) {
                if (auto snap = net::decode_drive_snapshot(body)) {
                    packets.clear();
                    local.reset();
                    resnapshot = std::move(*snap);
                    break;
                }
            } else if (kind == DriveObserveMessage::SnapshotChunk) {
                std::optional<net::DriveSnapshot> snap;
                if (!handle_snapshot_chunk(body, snap)) {
                    closed = true;
                    break;
                }
                if (snap) {
                    packets.clear();
                    local.reset();
                    resnapshot = std::move(*snap);
                    break;
                }
            } else if (kind == DriveObserveMessage::State) {
                if (auto state = decode_drive_state(body)) {
                    net::DriveSnapshot next;
                    next.character = file.character;
                    next.position = state->position;
                    next.heading = state->heading;
                    next.velocity = state->velocity;
                    next.moving = state->moving;
                    next.target = state->target;
                    if (state->inventory_packet) next.inventory_packet = std::move(*state->inventory_packet);
                    next.ui = std::move(state->ui);
                    if (state->has_status) {
                        next.has_vitals = true;
                        next.stand_state = state->stand_state;
                        next.owner_status = state->owner_status;
                        next.dead = state->dead;
                        next.auto_attack = state->auto_attack;
                        next.auto_fire = state->auto_fire;
                        next.running = state->running;
                        next.hp = state->hp;
                        next.max_hp = state->max_hp;
                        next.mana = state->mana;
                        next.max_mana = state->max_mana;
                        next.endurance = state->endurance;
                        next.max_endurance = state->max_endurance;
                    }
                    if (state->has_xtargets) {
                        next.has_xtargets = true;
                        next.xtarget_auto_haters = state->xtarget_auto_haters;
                        next.xtarget_zone = state->xtarget_zone;
                        next.xtarget_slots = std::move(state->xtarget_slots);
                    }
                    local = std::move(next);
                    // GameConnection consumes the queued packets, then the
                    // state. A newer packet must wait until this state lands.
                    break;
                }
            }
        }
    }

    void send_forward(const net::AppPacket& packet, bool reliable, bool world) {
        if (!file.control || closed || !pipe || !net::drive_forward_allowed(packet, world)) return;
        DriveForwardedPacket fwd{packet, reliable, world};
        if (!pipe->send_message(encode_drive_forward(fwd), 50)) {
            closed = true;
            error = pipe->error();
        }
    }
};

class DriveMirrorSession final : public net::Session {
public:
    DriveMirrorSession(std::shared_ptr<DriveClient::Impl> impl, bool world)
        : impl_(std::move(impl)), world_(world) {}

    void connect(const std::string&, std::uint16_t) override {}
    void update() override { impl_->pump(); }
    net::SessionState state() const override {
        return impl_->closed ? net::SessionState::Disconnected : net::SessionState::Connected;
    }
    std::string error() const override { return impl_->error; }
    void send(const net::AppPacket& packet, bool reliable = true) override {
        impl_->send_forward(packet, reliable, world_);
    }
    std::optional<net::AppPacket> receive() override {
        if (world_ || impl_->packets.empty()) return std::nullopt;
        net::AppPacket packet = std::move(impl_->packets.front());
        impl_->packets.pop_front();
        return packet;
    }
    void disconnect() override {}

private:
    std::shared_ptr<DriveClient::Impl> impl_;
    bool world_ = false;
};

std::optional<DriveObserveFile> read_drive_observe_file(const std::filesystem::path& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read " + path.string();
        return std::nullopt;
    }
    DriveObserveFile file;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        if (key == "endpoint") file.endpoint = value;
        else if (key == "token") file.token = value;
        else if (key == "character") file.character = value;
        else if (key == "profile") file.profile = value;
        else if (key == "game_dir") file.game_dir = value;
        else if (key == "interactive") file.interactive = value == "1";
        else if (key == "control") file.control = value == "1";
    }
    if (file.endpoint.empty() || file.token.empty()) {
        error = path.string() + " has no endpoint/token";
        return std::nullopt;
    }
    return file;
}

DriveClient::DriveClient(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
DriveClient::~DriveClient() = default;

std::unique_ptr<DriveClient> DriveClient::connect(const DriveObserveFile& file, std::string& error) {
    std::unique_ptr<platform::LocalPipeConnection> pipe;
    try {
        pipe = platform::LocalPipeConnection::connect({file.endpoint, file.token}, 10000);
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
    auto impl = std::make_shared<Impl>(std::move(pipe), file);
    std::optional<net::DriveSnapshot> initial;
    while (!initial) {
        const auto message = impl->pipe->receive_message(10000);
        if (!message || message->empty()) {
            error = "the drive observer sent no snapshot";
            return nullptr;
        }
        const std::span<const std::byte> bytes(message->data(), message->size());
        ByteReader r(bytes);
        const auto kind = static_cast<DriveObserveMessage>(r.le<std::uint8_t>());
        const auto body = bytes.subspan(1);
        if (kind == DriveObserveMessage::Snapshot) {
            initial = net::decode_drive_snapshot(body);
            if (!initial) {
                error = "the drive observer's snapshot is invalid";
                return nullptr;
            }
        } else if (kind == DriveObserveMessage::SnapshotChunk) {
            if (!impl->handle_snapshot_chunk(body, initial)) {
                error = "the drive observer's chunked snapshot is invalid";
                return nullptr;
            }
        } else {
            error = "the drive observer's first message is not a snapshot";
            return nullptr;
        }
    }
    impl->snapshot = std::move(*initial);
    return std::unique_ptr<DriveClient>(new DriveClient(std::move(impl)));
}

std::unique_ptr<net::Session> DriveClient::make_session(bool world) {
    return std::make_unique<DriveMirrorSession>(impl_, world);
}

net::DriveSnapshot DriveClient::take_snapshot() {
    net::DriveSnapshot snapshot = std::move(impl_->snapshot);
    impl_->snapshot = {};
    return snapshot;
}

std::optional<net::DriveSnapshot> DriveClient::take_resnapshot() {
    std::optional<net::DriveSnapshot> snap = std::move(impl_->resnapshot);
    impl_->resnapshot.reset();
    return snap;
}

std::optional<net::DriveSnapshot> DriveClient::take_local_state() {
    std::optional<net::DriveSnapshot> state = std::move(impl_->local);
    impl_->local.reset();
    return state;
}

std::vector<DriveChatLine> DriveClient::take_chat() {
    return std::exchange(impl_->chat, {});
}

void DriveClient::send_imgui_input(const ui::RemoteImGuiInput& input) {
    if (!impl_->closed && impl_->pipe) {
        if (!impl_->pipe->send_message(encode_drive_imgui_input(input), 20)) {
            impl_->closed = true;
            impl_->error = impl_->pipe->error();
        }
    }
}

std::optional<ui::RemoteImGuiFrame> DriveClient::take_imgui_frame() {
    std::optional<ui::RemoteImGuiFrame> out = std::move(impl_->imgui_frame);
    impl_->imgui_frame.reset();
    return out;
}

std::vector<ui::RemoteImGuiTexture> DriveClient::take_imgui_textures() {
    return std::exchange(impl_->imgui_textures, {});
}

std::vector<std::string> DriveClient::take_crew_commands() {
    return std::exchange(impl_->crew_commands, {});
}

void DriveClient::send_action(const net::DriveAction& action) {
    if ((impl_->file.interactive || impl_->file.control) && !impl_->closed) {
        if (!impl_->pipe->send_message(encode_drive_action(action), 50)) {
            impl_->closed = true;
            impl_->error = impl_->pipe->error();
        }
    }
}

void DriveClient::send_move(const net::DriveMove& move) {
    if (impl_->file.control && !impl_->closed) {
        if (!impl_->pipe->send_message(encode_drive_move(move), 20)) {
            impl_->closed = true;
            impl_->error = impl_->pipe->error();
        }
    }
}

void DriveClient::pump() { impl_->pump(); }

bool DriveClient::closed() const { return impl_->closed || !impl_->pipe || impl_->pipe->closed(); }

bool DriveClient::is_control() const { return impl_->file.control; }

const DriveObserveFile& DriveClient::info() const { return impl_->file; }

} // namespace hailstone::app
