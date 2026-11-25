#include "app/drive_control.h"

#include "core/byte_io.h"
#include "core/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
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

// A state snapshot crosses the pipe in chunks: one message is capped at kLocalPipeMaxMessage (8 MiB), and a long
// session's journal, or a viewer's return, is larger than that. Each chunk is {u32 whole size, u32 offset, bytes}.
constexpr std::size_t kSnapshotChunk = std::size_t{1} << 20;

bool send_snapshot(platform::LocalPipeConnection& pipe, DriveLeaseMessage kind,
                   const std::vector<std::byte>& encoded) {
    if (encoded.size() > net::kDriveSnapshotMaxBytes) return false;
    if (encoded.empty()) return true;
    const auto whole = static_cast<std::uint32_t>(encoded.size());
    for (std::size_t offset = 0; offset < encoded.size(); offset += kSnapshotChunk) {
        const std::size_t size = std::min(kSnapshotChunk, encoded.size() - offset);
        ByteWriter w;
        w.le(whole);
        w.le(static_cast<std::uint32_t>(offset));
        w.bytes(std::span<const std::byte>(encoded.data() + offset, size));
        if (!send_kind(pipe, kind, w.take())) return false;
    }
    return true;
}

// Reassembles a chunked snapshot; a size or offset that does not continue the same transfer is refused.
struct SnapshotReader {
    std::vector<std::byte> bytes;
    std::uint32_t whole = 0;
    bool active = false;

    bool feed(std::uint32_t total, std::uint32_t offset, std::span<const std::byte> data) {
        if (total > net::kDriveSnapshotMaxBytes) return false;
        if (!active) {
            active = true;
            whole = total;
            bytes.clear();
            bytes.reserve(whole);
        }
        if (total != whole || offset != bytes.size()) return false;
        bytes.insert(bytes.end(), data.begin(), data.end());
        return true;
    }
    bool complete() const { return active && bytes.size() == whole; }
    void reset() {
        bytes.clear();
        whole = 0;
        active = false;
    }
};

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
    std::shared_ptr<platform::LocalPipeConnection> retiring_pipe;
    std::optional<net::DriveSnapshot> snapshot;
    bool snapshot_sent = false;
    bool released_flag = false;
    std::optional<net::DriveSnapshot> release;
    SnapshotReader release_in;
    std::atomic<bool> new_viewer{false};
    net::SessionOptions options;
    std::unordered_map<std::uint64_t, Link> links;
    std::uint64_t active_world_id = 1;
    std::uint64_t active_zone_id = 2;

    Impl(platform::LocalPipeSession s, net::SessionOptions o)
        : server(s), session(server.session()), options(std::move(o)) {
        accept_thread = std::thread([this] { accept_loop(); });
    }

    ~Impl() {
        stop.store(true);
        if (accept_thread.joinable()) accept_thread.join();
        std::shared_ptr<platform::LocalPipeConnection> p, old_p;
        {
            std::lock_guard lock(mutex);
            p = std::move(pipe);
            old_p = std::move(retiring_pipe);
        }
        if (p) p->close();
        if (old_p) old_p->close();
    }

    void accept_loop() {
        while (!stop.load()) {
            auto accepted = server.accept(200);
            if (!accepted) continue;
            std::lock_guard lock(mutex);
            // A released lease's pipe is closed but still kept: a new viewer replaces it, so the same character
            // can be driven again without restarting the headless.
            if (!pipe || pipe->closed()) {
                pipe = std::shared_ptr<platform::LocalPipeConnection>(std::move(accepted));
                new_viewer.store(true);
            }
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
        if (kind == DriveLeaseMessage::ActiveSessions) {
            try {
                active_world_id = r.le<std::uint64_t>();
                active_zone_id = r.le<std::uint64_t>();
            } catch (const std::exception&) {
                return false;
            }
            return true;
        }
        if (kind == DriveLeaseMessage::Release) {
            try {
                ByteReader body(message.subspan(1));
                const auto total = body.le<std::uint32_t>();
                const auto offset = body.le<std::uint32_t>();
                if (!release_in.feed(total, offset, message.subspan(1 + 8))) return false;
                if (!release_in.complete()) return true; // more chunks
                if (auto returned = net::decode_drive_snapshot(release_in.bytes)) {
                    release = std::move(*returned);
                    released_flag = true;
                    send_kind(p, DriveLeaseMessage::ReleaseAck, {});
                    p.shutdown_write();
                }
                release_in.reset();
            } catch (const std::exception&) {
                return false;
            }
            return false; // the lease ends: the headless takes the sessions back
        }
        if (kind == DriveLeaseMessage::Snapshot || kind == DriveLeaseMessage::ReleaseAck) return true;
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
        try {
        if (!snapshot_sent && snapshot) {
            if (!send_snapshot(*p, DriveLeaseMessage::Snapshot, net::encode_drive_snapshot(*snapshot))) return;
            snapshot_sent = true;
        }
        while (auto message = p->receive_message(0)) {
            if (!handle(*p, std::span<const std::byte>(message->data(), message->size()))) break;
        }
        if (released_flag) return; // the headless takes the sessions back now
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
        } catch (const std::exception& e) {
            // Never terminate over a lease: close the pipe so the headless takes its unclean path (exit 3) and a
            // crew relaunches it.
            log::error("drive relay: {}", e.what());
            p->close();
        }
    }
};

DriveRelay::DriveRelay(platform::LocalPipeSession session)
    : impl_(std::make_unique<Impl>(std::move(session), net::SessionOptions{})) {}

DriveRelay::~DriveRelay() = default;

const platform::LocalPipeSession& DriveRelay::session() const { return impl_->session; }

bool DriveRelay::viewer_connected() const {
    auto p = impl_->current_pipe();
    return p && !p->closed();
}

bool DriveRelay::viewer_waiting() const { return impl_->new_viewer.load(); }

void DriveRelay::begin_lease(net::DriveSnapshot snapshot) {
    impl_->new_viewer.store(false);
    impl_->snapshot_sent = false;
    impl_->release_in.reset();
    impl_->release.reset();
    impl_->released_flag = false;
    impl_->links.clear();
    impl_->active_world_id = 1;
    impl_->active_zone_id = 2;
    impl_->snapshot = std::move(snapshot);
    std::shared_ptr<platform::LocalPipeConnection> old_p;
    {
        std::lock_guard lock(impl_->mutex);
        old_p = std::move(impl_->retiring_pipe);
    }
    if (old_p) old_p->close();
}

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
    impl_->released_flag = false;
    impl_->snapshot.reset();
    impl_->snapshot_sent = false;
    // Detach the released pipe immediately so accept_loop() can admit a subsequent swap without waiting for the
    // prior client's FIN to propagate, while keeping the prior pipe alive briefly so ReleaseAck drains cleanly.
    std::shared_ptr<platform::LocalPipeConnection> prev_retiring;
    {
        std::lock_guard lock(impl_->mutex);
        prev_retiring = std::move(impl_->retiring_pipe);
        impl_->retiring_pipe = std::move(impl_->pipe);
    }
    if (prev_retiring) prev_retiring->close();
    return snapshot;
}

std::unique_ptr<net::Session> DriveRelay::take_session(std::uint64_t id) {
    auto it = impl_->links.find(id);
    if (it == impl_->links.end()) return nullptr;
    return std::move(it->second.real);
}

std::unique_ptr<net::Session> DriveRelay::take_world_session() {
    return impl_->active_world_id ? take_session(impl_->active_world_id) : nullptr;
}

std::unique_ptr<net::Session> DriveRelay::take_zone_session() {
    return impl_->active_zone_id ? take_session(impl_->active_zone_id) : nullptr;
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
    SnapshotReader snapshot_in;
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
            if (kind == DriveLeaseMessage::Bye || kind == DriveLeaseMessage::ReleaseAck) {
                closed_flag = true;
                break;
            }
            if (kind == DriveLeaseMessage::Snapshot) {
                try {
                    ByteReader body(bytes.subspan(1));
                    const auto total = body.le<std::uint32_t>();
                    const auto offset = body.le<std::uint32_t>();
                    if (!snapshot_in.feed(total, offset, bytes.subspan(1 + 8))) {
                        closed_flag = true;
                        break;
                    }
                    if (snapshot_in.complete()) {
                        if (auto value = net::decode_drive_snapshot(snapshot_in.bytes))
                            snapshot = std::move(*value);
                        snapshot_in.reset();
                    }
                } catch (const std::exception&) {
                    closed_flag = true;
                    break;
                }
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
        if (!send_kind(*pipe, DriveLeaseMessage::Send, body.take())) {
            closed_flag = true;
            error = pipe->error();
        }
    }

    void connect(std::uint64_t id, const std::string& host, std::uint16_t port) {
        if (!pipe || closed_flag) return;
        ByteWriter body;
        body.le(id);
        body.le(port);
        put_string(body, host);
        if (!send_kind(*pipe, DriveLeaseMessage::Connect, body.take())) {
            closed_flag = true;
            error = pipe->error();
        }
    }

    void disconnect(std::uint64_t id) {
        if (!pipe || closed_flag) return;
        ByteWriter body;
        body.le(id);
        if (!send_kind(*pipe, DriveLeaseMessage::Disconnect, body.take())) {
            closed_flag = true;
            error = pipe->error();
        }
    }

    net::SessionState state(std::uint64_t id) const {
        if (closed_flag || !pipe || pipe->closed()) return net::SessionState::Disconnected;
        auto it = links.find(id);
        return it == links.end() ? net::SessionState::Disconnected : it->second.state;
    }

    std::string error_for(std::uint64_t id) const {
        if (closed_flag || !pipe || pipe->closed()) return !error.empty() ? error : (pipe ? pipe->error() : std::string());
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

    void release(net::DriveSnapshot returned, std::optional<std::pair<std::uint64_t, std::uint64_t>> active_sessions,
                 int ack_timeout_ms) {
        if (!pipe || closed_flag) return;
        if (active_sessions) {
            ByteWriter body;
            body.le(active_sessions->first);
            body.le(active_sessions->second);
            (void)send_kind(*pipe, DriveLeaseMessage::ActiveSessions, body.take());
        }
        const auto encoded = net::encode_drive_snapshot(returned);
        const bool sent = send_snapshot(*pipe, DriveLeaseMessage::Release, encoded);
        if (!sent)
            log::error("drive control: the release snapshot ({} bytes) could not be sent; the headless cannot resume",
                       encoded.size());
        // Half-close (FIN) so the headless can read the whole Release payload cleanly without a Windows TCP RST
        // dropping buffered chunks when incoming zone packets are sitting in our receive buffer.
        pipe->shutdown_write();
        if (sent && ack_timeout_ms > 0) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ack_timeout_ms);
            while (std::chrono::steady_clock::now() < deadline && !pipe->closed()) {
                const auto msg = pipe->receive_message(20);
                if (!msg) continue;
                if (msg->empty()) continue;
                const auto kind = static_cast<DriveLeaseMessage>(std::to_integer<std::uint8_t>((*msg)[0]));
                if (kind == DriveLeaseMessage::ReleaseAck || kind == DriveLeaseMessage::Bye) break;
            }
        }
        pipe->close();
        closed_flag = true;
    }
};

// Defined out of line so the vtable has one anchor in this translation unit.
class DriveLeaseMirrorSession final : public net::Session {
public:
    DriveLeaseMirrorSession(std::shared_ptr<DriveControlClient::Impl> impl, std::uint64_t id);
    std::uint64_t lease_id() const { return id_; }
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

void DriveControlClient::release(net::DriveSnapshot snapshot, const net::Session* world, const net::Session* zone,
                                 int ack_timeout_ms) {
    std::optional<std::pair<std::uint64_t, std::uint64_t>> active;
    if (world || zone) {
        const auto* wm = dynamic_cast<const DriveLeaseMirrorSession*>(world);
        const auto* zm = dynamic_cast<const DriveLeaseMirrorSession*>(zone);
        active = {wm ? wm->lease_id() : 0u, zm ? zm->lease_id() : 0u};
    }
    impl_->release(std::move(snapshot), active, ack_timeout_ms);
}

bool DriveControlClient::closed() const { return impl_->closed_flag || !impl_->pipe || impl_->pipe->closed(); }

} // namespace hailstone::app
