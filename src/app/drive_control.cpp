#include "app/drive_control.h"

#include "core/byte_io.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

namespace hailstone::app {
namespace {

void put_packet(ByteWriter& w, const net::AppPacket& packet) {
    w.le(packet.opcode);
    w.le(static_cast<std::uint32_t>(packet.data.size()));
    w.bytes(packet.data);
}

std::optional<net::AppPacket> get_packet(ByteReader& r) {
    try {
        net::AppPacket packet;
        packet.opcode = r.le<std::uint16_t>();
        const auto size = r.le<std::uint32_t>();
        if (size > net::kDriveSnapshotMaxBytes) return std::nullopt;
        const auto data = r.bytes(size);
        packet.data.assign(data.begin(), data.end());
        return packet;
    } catch (...) {
        return std::nullopt;
    }
}

void put_string(ByteWriter& w, std::string_view text) {
    w.le(static_cast<std::uint16_t>(text.size()));
    w.bytes(std::span(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

std::string get_string(ByteReader& r) {
    const auto size = r.le<std::uint16_t>();
    const auto bytes = r.bytes(size);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool send_kind(platform::LocalPipeConnection& pipe, DriveLeaseMessage kind,
               std::span<const std::byte> body) {
    ByteWriter w;
    w.le(static_cast<std::uint8_t>(kind));
    w.bytes(body);
    const auto& bytes = w.data();
    return pipe.send_message(std::span<const std::byte>(bytes.data(), bytes.size()), 5000);
}

std::vector<std::byte> encode_state(std::uint64_t id, net::SessionState state, const std::string& error) {
    ByteWriter w;
    w.le(id);
    w.le(static_cast<std::uint8_t>(state));
    put_string(w, error);
    return w.take();
}

} // namespace

struct DriveRelay::Impl {
    struct Link {
        std::unique_ptr<net::Session> real;
        net::SessionState state = net::SessionState::Disconnected;
        std::string error;
        bool adopted = false;
    };

    platform::LocalPipeServer server;
    platform::LocalPipeSession session;
    std::thread accept_thread;
    std::atomic<bool> stop{false};
    std::mutex mutex;
    std::shared_ptr<platform::LocalPipeConnection> pipe;
    std::optional<net::DriveSnapshot> snapshot;
    bool snapshot_sent = false;
    bool released_flag = false;
    std::optional<net::DriveSnapshot> release;
    net::SessionOptions options;
    std::unordered_map<std::uint64_t, Link> links;

    Impl(platform::LocalPipeSession s, net::SessionOptions o)
        : server(s), session(server.session()), options(std::move(o)) {
        accept_thread = std::thread([this] { accept_loop(); });
    }

    ~Impl() {
        stop.store(true);
        if (accept_thread.joinable()) accept_thread.join();
        std::shared_ptr<platform::LocalPipeConnection> p;
        {
            std::lock_guard lock(mutex);
            p = std::move(pipe);
        }
        if (p) p->close();
    }

    void accept_loop() {
        while (!stop.load()) {
            auto accepted = server.accept(200);
            if (!accepted) continue;
            std::lock_guard lock(mutex);
            if (!pipe) pipe = std::shared_ptr<platform::LocalPipeConnection>(std::move(accepted));
        }
    }

    std::shared_ptr<platform::LocalPipeConnection> current_pipe() {
        std::lock_guard lock(mutex);
        return pipe;
    }

    bool handle(platform::LocalPipeConnection& p, std::span<const std::byte> message) {
        if (message.empty()) return true;
        ByteReader r(message);
        const auto kind = static_cast<DriveLeaseMessage>(r.le<std::uint8_t>());
        if (kind == DriveLeaseMessage::Bye) return false;
        if (kind == DriveLeaseMessage::Release) {
            if (auto returned = net::decode_drive_snapshot(message.subspan(1))) {
                release = std::move(*returned);
                released_flag = true;
            }
            return false; // the lease ends: the headless takes the sessions back
        }
        if (kind == DriveLeaseMessage::Snapshot) return true; // the viewer never sends one
        try {
            const auto id = r.le<std::uint64_t>();
            auto it = links.find(id);
            if (kind == DriveLeaseMessage::Send) {
                const bool reliable = r.le<std::uint8_t>() != 0;
                if (auto packet = get_packet(r); packet && it != links.end() && it->second.real)
                    it->second.real->send(*packet, reliable);
            } else if (kind == DriveLeaseMessage::Connect) {
                const auto port = r.le<std::uint16_t>();
                const auto host = get_string(r);
                Link link;
                link.real = net::Session::create(options);
                link.real->connect(host, port);
                link.state = net::SessionState::Connecting;
                links[id] = std::move(link);
                send_kind(p, DriveLeaseMessage::State,
                          encode_state(id, net::SessionState::Connecting, std::string()));
            } else if (kind == DriveLeaseMessage::Disconnect) {
                if (it != links.end() && it->second.real && !it->second.adopted)
                    it->second.real->disconnect();
            }
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }

    void pump() {
        auto p = current_pipe();
        if (!p) return;
        if (!snapshot_sent && snapshot) {
            if (!send_kind(*p, DriveLeaseMessage::Snapshot, net::encode_drive_snapshot(*snapshot))) return;
            snapshot_sent = true;
        }
        while (auto message = p->receive_message(0)) {
            if (!handle(*p, std::span<const std::byte>(message->data(), message->size()))) break;
        }
        if (p->closed()) return;
        for (auto& [id, link] : links) {
            if (!link.real || released_flag) continue;
            link.real->update();
            while (auto packet = link.real->receive()) {
                ByteWriter body;
                body.le(id);
                put_packet(body, *packet);
                if (!send_kind(*p, DriveLeaseMessage::Packet, body.take())) break;
            }
            const auto state = link.real->state();
            const auto error = link.real->error();
            if (state != link.state || error != link.error) {
                link.state = state;
                link.error = error;
                send_kind(*p, DriveLeaseMessage::State, encode_state(id, state, error));
            }
        }
    }
};

DriveRelay::DriveRelay(platform::LocalPipeSession session)
    : impl_(std::make_unique<Impl>(std::move(session), net::SessionOptions{})) {}

DriveRelay::~DriveRelay() = default;

const platform::LocalPipeSession& DriveRelay::session() const { return impl_->session; }

bool DriveRelay::viewer_connected() const { return impl_->current_pipe() != nullptr; }

void DriveRelay::set_snapshot(net::DriveSnapshot snapshot) {
    std::lock_guard lock(impl_->mutex);
    impl_->snapshot = std::move(snapshot);
}

void DriveRelay::adopt(std::uint64_t id, std::unique_ptr<net::Session> session) {
    Impl::Link link;
    link.real = std::move(session);
    link.adopted = true;
    impl_->links[id] = std::move(link);
}

void DriveRelay::pump() { impl_->pump(); }

bool DriveRelay::released() const { return impl_->released_flag; }

net::DriveSnapshot DriveRelay::take_release() {
    net::DriveSnapshot snapshot = impl_->release.value_or(net::DriveSnapshot{});
    impl_->release.reset();
    return snapshot;
}

std::unique_ptr<net::Session> DriveRelay::take_session(std::uint64_t id) {
    auto it = impl_->links.find(id);
    if (it == impl_->links.end()) return nullptr;
    return std::move(it->second.real);
}

bool DriveRelay::closed() const {
    auto p = impl_->current_pipe();
    return !p || p->closed();
}

// ---------------------------------------------------------------- the viewer

struct DriveControlClient::Impl : std::enable_shared_from_this<DriveControlClient::Impl> {
    struct Link {
        std::deque<net::AppPacket> packets;
        net::SessionState state = net::SessionState::Disconnected;
        std::string error;
    };

    std::shared_ptr<platform::LocalPipeConnection> pipe;
    std::optional<net::DriveSnapshot> snapshot;
    std::unordered_map<std::uint64_t, Link> links;
    std::uint64_t next_id = 1;
    bool closed_flag = false;
    std::string error;

    void pump() {
        if (!pipe || closed_flag) return;
        if (pipe->closed()) {
            closed_flag = true;
            error = pipe->error();
            return;
        }
        while (auto message = pipe->receive_message(0)) {
            if (message->empty()) continue;
            const std::span<const std::byte> bytes(message->data(), message->size());
            ByteReader r(bytes);
            const auto kind = static_cast<DriveLeaseMessage>(r.le<std::uint8_t>());
            if (kind == DriveLeaseMessage::Bye) {
                closed_flag = true;
                break;
            }
            if (kind == DriveLeaseMessage::Snapshot) {
                if (auto value = net::decode_drive_snapshot(bytes.subspan(1))) snapshot = std::move(*value);
                continue;
            }
            try {
                const auto id = r.le<std::uint64_t>();
                auto it = links.find(id);
                if (it == links.end()) continue;
                if (kind == DriveLeaseMessage::Packet) {
                    if (auto packet = get_packet(r)) it->second.packets.push_back(std::move(*packet));
                } else if (kind == DriveLeaseMessage::State) {
                    it->second.state = static_cast<net::SessionState>(r.le<std::uint8_t>());
                    it->second.error = get_string(r);
                }
            } catch (const std::exception&) {
                closed_flag = true;
                break;
            }
        }
    }

    std::unique_ptr<net::Session> make_session();

    void send(std::uint64_t id, const net::AppPacket& packet, bool reliable) {
        if (!pipe || closed_flag) return;
        ByteWriter body;
        body.le(id);
        body.le(static_cast<std::uint8_t>(reliable ? 1 : 0));
        put_packet(body, packet);
        send_kind(*pipe, DriveLeaseMessage::Send, body.take());
    }

    void connect(std::uint64_t id, const std::string& host, std::uint16_t port) {
        if (!pipe || closed_flag) return;
        ByteWriter body;
        body.le(id);
        body.le(port);
        put_string(body, host);
        send_kind(*pipe, DriveLeaseMessage::Connect, body.take());
    }

    void disconnect(std::uint64_t id) {
        if (!pipe || closed_flag) return;
        ByteWriter body;
        body.le(id);
        send_kind(*pipe, DriveLeaseMessage::Disconnect, body.take());
    }

    net::SessionState state(std::uint64_t id) const {
        auto it = links.find(id);
        return it == links.end() ? net::SessionState::Disconnected : it->second.state;
    }

    std::string error_for(std::uint64_t id) const {
        auto it = links.find(id);
        return it == links.end() ? std::string() : it->second.error;
    }

    std::optional<net::AppPacket> receive(std::uint64_t id) {
        auto it = links.find(id);
        if (it == links.end() || it->second.packets.empty()) return std::nullopt;
        net::AppPacket packet = std::move(it->second.packets.front());
        it->second.packets.pop_front();
        return packet;
    }

    void release(net::DriveSnapshot returned) {
        if (!pipe || closed_flag) return;
        send_kind(*pipe, DriveLeaseMessage::Release, net::encode_drive_snapshot(returned));
        pipe->close();
        closed_flag = true;
    }
};

// Defined out of line so the vtable has one anchor in this translation unit.
class DriveLeaseMirrorSession final : public net::Session {
public:
    DriveLeaseMirrorSession(std::shared_ptr<DriveControlClient::Impl> impl, std::uint64_t id);
    void connect(const std::string& host, std::uint16_t port) override;
    void update() override;
    net::SessionState state() const override;
    std::string error() const override;
    void send(const net::AppPacket& packet, bool reliable) override;
    std::optional<net::AppPacket> receive() override;
    void disconnect() override;

private:
    std::shared_ptr<DriveControlClient::Impl> impl_;
    std::uint64_t id_;
};

DriveLeaseMirrorSession::DriveLeaseMirrorSession(std::shared_ptr<DriveControlClient::Impl> impl, std::uint64_t id)
    : impl_(std::move(impl)), id_(id) {}
void DriveLeaseMirrorSession::connect(const std::string& host, std::uint16_t port) { impl_->connect(id_, host, port); }
void DriveLeaseMirrorSession::update() { impl_->pump(); }
net::SessionState DriveLeaseMirrorSession::state() const { return impl_->state(id_); }
std::string DriveLeaseMirrorSession::error() const { return impl_->error_for(id_); }
void DriveLeaseMirrorSession::send(const net::AppPacket& packet, bool reliable) {
    impl_->send(id_, packet, reliable);
}
std::optional<net::AppPacket> DriveLeaseMirrorSession::receive() { return impl_->receive(id_); }
void DriveLeaseMirrorSession::disconnect() { impl_->disconnect(id_); }

std::unique_ptr<net::Session> DriveControlClient::Impl::make_session() {
    const std::uint64_t id = next_id++;
    links[id] = Link{};
    return std::make_unique<DriveLeaseMirrorSession>(shared_from_this(), id);
}

DriveControlClient::DriveControlClient(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
DriveControlClient::~DriveControlClient() = default;

std::unique_ptr<DriveControlClient> DriveControlClient::connect(const platform::LocalPipeSession& session,
                                                               int timeout_ms, std::string& error) {
    try {
        auto pipe = platform::LocalPipeConnection::connect(session, timeout_ms);
        auto impl = std::make_shared<Impl>();
        impl->pipe = std::shared_ptr<platform::LocalPipeConnection>(std::move(pipe));
        return std::unique_ptr<DriveControlClient>(new DriveControlClient(std::move(impl)));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

std::unique_ptr<net::Session> DriveControlClient::make_session() { return impl_->make_session(); }

std::optional<net::DriveSnapshot> DriveControlClient::take_snapshot() {
    std::optional<net::DriveSnapshot> snapshot = std::move(impl_->snapshot);
    impl_->snapshot.reset();
    return snapshot;
}

bool DriveControlClient::session_connected(std::uint64_t id) const {
    return impl_->state(id) == net::SessionState::Connected;
}

void DriveControlClient::pump() { impl_->pump(); }

void DriveControlClient::release(net::DriveSnapshot snapshot) { impl_->release(std::move(snapshot)); }

bool DriveControlClient::closed() const { return impl_->closed_flag || !impl_->pipe || impl_->pipe->closed(); }

} // namespace hailstone::app
