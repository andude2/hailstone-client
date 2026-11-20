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

class DriveMirrorSession;

} // namespace

struct DriveClient::Impl {
    std::unique_ptr<platform::LocalPipeConnection> pipe;
    DriveObserveFile file;
    net::DriveSnapshot snapshot;
    std::deque<net::AppPacket> packets;
    std::optional<net::DriveSnapshot> local;
    bool closed = false;
    std::string error;

    Impl(std::unique_ptr<platform::LocalPipeConnection> p, DriveObserveFile f)
        : pipe(std::move(p)), file(std::move(f)) {}

    void pump() {
        if (!pipe || closed) return;
        if (pipe->closed()) {
            closed = true;
            error = pipe->error();
            return;
        }
        while (auto message = pipe->receive_message(0)) {
            const std::span<const std::byte> bytes(message->data(), message->size());
            ByteReader r(bytes);
            const auto kind = static_cast<DriveObserveMessage>(r.le<std::uint8_t>());
            const auto body = bytes.subspan(1);
            if (kind == DriveObserveMessage::Packet) {
                if (auto packet = decode_drive_packet(body)) packets.push_back(std::move(*packet));
            } else if (kind == DriveObserveMessage::State) {
                if (auto state = decode_drive_state(body)) {
                    net::DriveSnapshot next;
                    next.character = file.character;
                    next.position = state->position;
                    next.heading = state->heading;
                    next.velocity = state->velocity;
                    next.moving = state->moving;
                    next.target = state->target;
                    if (state->inventory_packet) next.inventory_packet = *state->inventory_packet;
                    local = std::move(next);
                }
            }
        }
    }
};

class DriveMirrorSession final : public net::Session {
public:
    explicit DriveMirrorSession(std::shared_ptr<DriveClient::Impl> impl) : impl_(std::move(impl)) {}

    void connect(const std::string&, std::uint16_t) override {}
    void update() override { impl_->pump(); }
    net::SessionState state() const override {
        return impl_->closed ? net::SessionState::Disconnected : net::SessionState::Connected;
    }
    std::string error() const override { return impl_->error; }
    void send(const net::AppPacket&, bool = true) override {} // observe: nothing leaves
    std::optional<net::AppPacket> receive() override {
        if (impl_->packets.empty()) return std::nullopt;
        net::AppPacket packet = std::move(impl_->packets.front());
        impl_->packets.pop_front();
        return packet;
    }
    void disconnect() override {}

private:
    std::shared_ptr<DriveClient::Impl> impl_;
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
    const auto message = impl->pipe->receive_message(10000);
    if (!message) {
        error = "the drive observer sent no snapshot";
        return nullptr;
    }
    const std::span<const std::byte> bytes(message->data(), message->size());
    ByteReader r(bytes);
    if (static_cast<DriveObserveMessage>(r.le<std::uint8_t>()) != DriveObserveMessage::Snapshot) {
        error = "the drive observer's first message is not a snapshot";
        return nullptr;
    }
    auto snapshot = net::decode_drive_snapshot(bytes.subspan(1));
    if (!snapshot) {
        error = "the drive observer's snapshot is invalid";
        return nullptr;
    }
    impl->snapshot = std::move(*snapshot);
    return std::unique_ptr<DriveClient>(new DriveClient(std::move(impl)));
}

std::unique_ptr<net::Session> DriveClient::make_session() {
    return std::make_unique<DriveMirrorSession>(impl_);
}

net::DriveSnapshot DriveClient::take_snapshot() {
    net::DriveSnapshot snapshot = std::move(impl_->snapshot);
    impl_->snapshot = {};
    return snapshot;
}

std::optional<net::DriveSnapshot> DriveClient::take_local_state() {
    std::optional<net::DriveSnapshot> state = std::move(impl_->local);
    impl_->local.reset();
    return state;
}

void DriveClient::pump() { impl_->pump(); }

const DriveObserveFile& DriveClient::info() const { return impl_->file; }

} // namespace hailstone::app
