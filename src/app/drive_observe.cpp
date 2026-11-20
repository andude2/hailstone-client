#include "app/drive_observe.h"

#include "core/byte_io.h"

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

constexpr std::size_t kObserveQueueLimit = std::size_t{16} << 20;

void put_packet(ByteWriter& w, const net::AppPacket& packet) {
    w.le(packet.opcode);
    w.le(static_cast<std::uint32_t>(packet.data.size()));
    w.bytes(packet.data);
}

bool send_message(platform::LocalPipeConnection& client, DriveObserveMessage kind,
                  std::span<const std::byte> body) {
    ByteWriter w;
    w.le(static_cast<std::uint8_t>(kind));
    w.bytes(body);
    const auto& bytes = w.data();
    return client.send_message(std::span<const std::byte>(bytes.data(), bytes.size()), 5000);
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
        if (!r.empty()) return std::nullopt;
        return state;
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
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<net::DriveSnapshot> snapshot;
    std::deque<std::pair<DriveObserveMessage, std::vector<std::byte>>> queue;
    std::size_t queued_bytes = 0;
    bool client_active = false;

    explicit Impl(platform::LocalPipeSession s) : server(s), session(server.session()) {
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
                initial = snapshot;
                client_active = true;
                queue.clear();
                queued_bytes = 0;
            }
            if (!send(*client, DriveObserveMessage::Snapshot, encode_drive_snapshot(*initial))) {
                std::lock_guard lock(mutex);
                client_active = false;
                continue;
            }

            while (!stop.load() && !client->closed()) {
                std::optional<std::pair<DriveObserveMessage, std::vector<std::byte>>> message;
                {
                    std::unique_lock lock(mutex);
                    if (!queue.empty()) {
                        message = std::move(queue.front());
                        queue.pop_front();
                        queued_bytes -= message->second.size();
                    } else {
                        wake.wait_for(lock, std::chrono::milliseconds(50));
                    }
                }
                if (message && !send(*client, message->first, message->second)) break;
            }
            client->close();
            {
                std::lock_guard lock(mutex);
                client_active = false;
                queue.clear();
                queued_bytes = 0;
            }
        }
    }

    void enqueue(DriveObserveMessage kind, std::vector<std::byte> body) {
        std::lock_guard lock(mutex);
        if (!client_active) return;
        if (queued_bytes + body.size() > kObserveQueueLimit) return; // a stalled viewer misses packets, does not grow us
        queued_bytes += body.size();
        if (kind == DriveObserveMessage::State && !queue.empty() &&
            queue.back().first == DriveObserveMessage::State) {
            queued_bytes -= queue.back().second.size();
            queue.back().second = std::move(body);
            queued_bytes += queue.back().second.size();
        } else {
            queue.emplace_back(kind, std::move(body));
        }
        wake.notify_one();
    }
};

DriveObserveServer::DriveObserveServer(platform::LocalPipeSession session)
    : impl_(std::make_unique<Impl>(std::move(session))) {}

DriveObserveServer::~DriveObserveServer() { stop(); }

const platform::LocalPipeSession& DriveObserveServer::session() const { return impl_->session; }

bool DriveObserveServer::needs_snapshot() const { return impl_->snapshot_requested.load(); }

void DriveObserveServer::set_snapshot(net::DriveSnapshot snapshot) {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->snapshot = std::move(snapshot);
        impl_->snapshot_requested.store(false);
    }
    impl_->wake.notify_all();
}

void DriveObserveServer::publish_zone_packet(const net::AppPacket& packet) {
    impl_->enqueue(DriveObserveMessage::Packet, encode_drive_packet(packet));
}

void DriveObserveServer::publish_state(const DriveLocalState& state) {
    impl_->enqueue(DriveObserveMessage::State, encode_drive_state(state));
}

void DriveObserveServer::stop() {
    if (impl_) impl_->stop_server();
}

} // namespace hailstone::app
