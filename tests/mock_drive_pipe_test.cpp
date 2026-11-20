// D0, two processes: a GameConnection behind a LocalMirrorSession talks to a
// child process that owns the real UDP Sessions, over the local authenticated
// stream (platform/local_pipe). The protocol is small and test-only; the
// production relay will keep the same shape.

#include <doctest/doctest.h>
#include <SDL3/SDL.h>

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/byte_io.h"
#include "mock_server/harness.h"
#include "net/session.h"
#include "platform/child_processes.h"
#include "platform/executable_path.h"
#include "platform/local_pipe.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace {

enum class Message : std::uint8_t {
    Connect = 1,    // id, port, host
    Disconnect = 2, // id
    Send = 3,       // id, reliable, opcode, data
    State = 4,      // id, state, error
    Packet = 5,     // id, opcode, data
    Bye = 6,
};

void put_string(ByteWriter& w, std::string_view text) {
    w.le(static_cast<std::uint16_t>(text.size()));
    w.bytes(std::span(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

std::string get_string(ByteReader& r) {
    const auto n = r.le<std::uint16_t>();
    const auto bytes = r.bytes(n);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::vector<std::byte> encode_connect(std::uint64_t id, const std::string& host, std::uint16_t port) {
    ByteWriter w;
    w.le(static_cast<std::uint8_t>(Message::Connect));
    w.le(id);
    w.le(port);
    put_string(w, host);
    return w.take();
}

std::vector<std::byte> encode_disconnect(std::uint64_t id) {
    ByteWriter w;
    w.le(static_cast<std::uint8_t>(Message::Disconnect));
    w.le(id);
    return w.take();
}

std::vector<std::byte> encode_send(std::uint64_t id, const AppPacket& p, bool reliable) {
    ByteWriter w;
    w.le(static_cast<std::uint8_t>(Message::Send));
    w.le(id);
    w.le(static_cast<std::uint8_t>(reliable ? 1 : 0));
    w.le(p.opcode);
    w.le(static_cast<std::uint32_t>(p.data.size()));
    w.bytes(p.data);
    return w.take();
}

std::vector<std::byte> encode_state(std::uint64_t id, SessionState state, const std::string& error) {
    ByteWriter w;
    w.le(static_cast<std::uint8_t>(Message::State));
    w.le(id);
    w.le(static_cast<std::uint8_t>(state));
    put_string(w, error);
    return w.take();
}

std::vector<std::byte> encode_packet(std::uint64_t id, const AppPacket& p) {
    ByteWriter w;
    w.le(static_cast<std::uint8_t>(Message::Packet));
    w.le(id);
    w.le(p.opcode);
    w.le(static_cast<std::uint32_t>(p.data.size()));
    w.bytes(p.data);
    return w.take();
}

class LocalMirrorSession;

// The GUI's side: one pipe, one session id per Session the GameConnection asks
// for, and the state and packets the child reports for each.
class LocalRelayChannel : public std::enable_shared_from_this<LocalRelayChannel> {
public:
    explicit LocalRelayChannel(std::unique_ptr<platform::LocalPipeConnection> pipe) : pipe_(std::move(pipe)) {}

    std::unique_ptr<Session> make_session();

    void connect(std::uint64_t id, const std::string& host, std::uint16_t port) {
        {
            std::lock_guard lock(mutex_);
            auto& link = links_[id];
            link.state = SessionState::Connecting;
            link.error.clear();
        }
        if (!pipe_ || !pipe_->send_message(encode_connect(id, host, port))) fail_link(id);
    }
    void disconnect(std::uint64_t id) {
        {
            std::lock_guard lock(mutex_);
            auto it = links_.find(id);
            if (it != links_.end()) it->second.state = SessionState::Disconnecting;
        }
        if (pipe_) pipe_->send_message(encode_disconnect(id));
    }
    void send(std::uint64_t id, const AppPacket& packet, bool reliable) {
        if (!pipe_ || !pipe_->send_message(encode_send(id, packet, reliable))) fail_link(id);
    }
    std::optional<AppPacket> receive(std::uint64_t id) {
        std::lock_guard lock(mutex_);
        auto it = links_.find(id);
        if (it == links_.end() || it->second.packets.empty()) return std::nullopt;
        AppPacket packet = std::move(it->second.packets.front());
        it->second.packets.pop_front();
        return packet;
    }
    SessionState state(std::uint64_t id) const {
        std::lock_guard lock(mutex_);
        auto it = links_.find(id);
        return it == links_.end() ? SessionState::Disconnected : it->second.state;
    }
    std::string error(std::uint64_t id) const {
        std::lock_guard lock(mutex_);
        auto it = links_.find(id);
        return it == links_.end() ? std::string() : it->second.error;
    }

    void pump() {
        if (!pipe_) return;
        if (pipe_->closed()) {
            std::lock_guard lock(mutex_);
            for (auto& [id, link] : links_) {
                (void)id;
                if (link.state != SessionState::Disconnected) {
                    link.state = SessionState::Disconnected;
                    link.error = "the drive relay closed";
                }
            }
            return;
        }
        while (auto message = pipe_->receive_message(0)) handle(*message);
    }

private:
    struct Link {
        SessionState state = SessionState::Disconnected;
        std::string error;
        std::deque<AppPacket> packets;
    };

    void fail_link(std::uint64_t id) {
        std::lock_guard lock(mutex_);
        auto it = links_.find(id);
        if (it == links_.end()) return;
        it->second.state = SessionState::Disconnected;
        it->second.error = "the drive relay is gone";
    }

    void handle(std::span<const std::byte> body) {
        ByteReader r(body);
        const auto kind = static_cast<Message>(r.le<std::uint8_t>());
        if (kind == Message::Bye) {
            std::lock_guard lock(mutex_);
            for (auto& [link_id, link] : links_) {
                (void)link_id;
                link.state = SessionState::Disconnected;
                link.error = "the drive relay said goodbye";
            }
            return;
        }
        const std::uint64_t id = r.le<std::uint64_t>();
        if (kind == Message::State) {
            const auto state = static_cast<SessionState>(r.le<std::uint8_t>());
            const std::string error = get_string(r);
            std::lock_guard lock(mutex_);
            auto& link = links_[id];
            link.state = state;
            link.error = error;
        } else if (kind == Message::Packet) {
            AppPacket packet;
            packet.opcode = r.le<std::uint16_t>();
            const auto size = r.le<std::uint32_t>();
            if (size > 64u << 20) throw std::runtime_error("drive packet is too large");
            const auto data = r.bytes(size);
            packet.data.assign(data.begin(), data.end());
            std::lock_guard lock(mutex_);
            links_[id].packets.push_back(std::move(packet));
        }
    }

    std::unique_ptr<platform::LocalPipeConnection> pipe_;
    std::uint64_t next_id_ = 1;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, Link> links_;
};

class LocalMirrorSession final : public Session {
public:
    LocalMirrorSession(std::shared_ptr<LocalRelayChannel> channel, std::uint64_t id)
        : channel_(std::move(channel)), id_(id) {}

    void connect(const std::string& host, std::uint16_t port) override { channel_->connect(id_, host, port); }
    void update() override { channel_->pump(); }
    SessionState state() const override { return channel_->state(id_); }
    std::string error() const override { return channel_->error(id_); }
    void send(const AppPacket& packet, bool reliable = true) override { channel_->send(id_, packet, reliable); }
    std::optional<AppPacket> receive() override { return channel_->receive(id_); }
    void disconnect() override { channel_->disconnect(id_); }

private:
    std::shared_ptr<LocalRelayChannel> channel_;
    std::uint64_t id_ = 0;
};

std::unique_ptr<Session> LocalRelayChannel::make_session() {
    const std::uint64_t id = next_id_++;
    {
        std::lock_guard lock(mutex_);
        links_[id] = Link{};
    }
    return std::make_unique<LocalMirrorSession>(shared_from_this(), id);
}

// The child's side: a real Session per id the GUI asks to connect, pumped in one loop beside the pipe.
struct ChildLink {
    std::unique_ptr<Session> real;
    std::string host;
    std::uint16_t port = 0;
    bool connect_pending = true;
    SessionState state = SessionState::Disconnected;
    std::string error;
    std::deque<std::pair<AppPacket, bool>> outgoing;
};

bool send_child_state(platform::LocalPipeConnection& pipe, std::uint64_t id, const ChildLink& link) {
    return pipe.send_message(encode_state(id, link.state, link.error));
}

void run_relay(platform::LocalPipeConnection& pipe) {
    const SessionOptions options = quick_options().session;
    std::unordered_map<std::uint64_t, ChildLink> links;
    while (!pipe.closed()) {
        while (auto message = pipe.receive_message(0)) {
            ByteReader r(*message);
            const auto kind = static_cast<Message>(r.le<std::uint8_t>());
            if (kind == Message::Bye) return;
            const std::uint64_t id = r.le<std::uint64_t>();
            if (kind == Message::Connect) {
                ChildLink link;
                link.port = r.le<std::uint16_t>();
                link.host = get_string(r);
                link.state = SessionState::Connecting;
                links[id] = std::move(link);
                if (!send_child_state(pipe, id, links[id])) return;
            } else if (kind == Message::Send) {
                const bool reliable = r.le<std::uint8_t>() != 0;
                AppPacket packet;
                packet.opcode = r.le<std::uint16_t>();
                const auto size = r.le<std::uint32_t>();
                const auto data = r.bytes(size);
                packet.data.assign(data.begin(), data.end());
                auto it = links.find(id);
                if (it == links.end()) continue;
                if (it->second.real && it->second.state == SessionState::Connected)
                    it->second.real->send(packet, reliable);
                else
                    it->second.outgoing.emplace_back(std::move(packet), reliable);
            } else if (kind == Message::Disconnect) {
                auto it = links.find(id);
                if (it == links.end()) continue;
                it->second.state = SessionState::Disconnecting;
                if (it->second.real) it->second.real->disconnect();
                if (!send_child_state(pipe, id, it->second)) return;
            }
        }
        if (pipe.closed()) return;

        for (auto& [id, link] : links) {
            if (link.connect_pending) {
                link.connect_pending = false;
                link.real = Session::create(options);
                link.real->connect(link.host, link.port);
            }
            if (!link.real) continue;
            link.real->update();
            while (auto packet = link.real->receive())
                if (!pipe.send_message(encode_packet(id, *packet))) return;
            const auto state = link.real->state();
            const auto error = link.real->error();
            if (state != link.state || error != link.error) {
                link.state = state;
                link.error = error;
                if (!send_child_state(pipe, id, link)) return;
            }
            if (state == SessionState::Connected) {
                while (!link.outgoing.empty()) {
                    auto [packet, reliable] = std::move(link.outgoing.front());
                    link.outgoing.pop_front();
                    link.real->send(packet, reliable);
                }
            } else if (state == SessionState::Disconnected) {
                link.outgoing.clear();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void set_env(const char* name, const std::string& value) {
    SDL_SetEnvironmentVariable(SDL_GetEnvironment(), name, value.c_str(), true);
}
void clear_env(const char* name) { SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), name); }
const char* get_env(const char* name) { return SDL_GetEnvironmentVariable(SDL_GetEnvironment(), name); }
struct EnvClear {
    ~EnvClear() {
        clear_env("HAILSTONE_DRIVE_PIPE_ENDPOINT");
        clear_env("HAILSTONE_DRIVE_PIPE_TOKEN");
        clear_env("HAILSTONE_DRIVE_PIPE_CHILD");
    }
};

} // namespace

TEST_CASE("drive_pipe_relay_child") {
    const char* endpoint = get_env("HAILSTONE_DRIVE_PIPE_ENDPOINT");
    const char* token = get_env("HAILSTONE_DRIVE_PIPE_TOKEN");
    const char* child = get_env("HAILSTONE_DRIVE_PIPE_CHILD");
    if (!endpoint || !token || !child || !*endpoint || !*token) return;

    auto pipe = platform::LocalPipeConnection::connect({endpoint, token}, 15000);
    run_relay(*pipe);
}

TEST_CASE("drive pipe: a GameConnection plays through a two-process relay") {
    auto exe = platform::process_executable_path();
    REQUIRE(exe);
    EnvClear clear_on_exit;
    clear_env("HAILSTONE_DRIVE_PIPE_ENDPOINT");
    clear_env("HAILSTONE_DRIVE_PIPE_TOKEN");
    clear_env("HAILSTONE_DRIVE_PIPE_CHILD");

    MockEqServer server;
    EmptyGameDir dir;
    GameDir game(dir.path());
    platform::LocalPipeServer pipe_server(platform::make_local_pipe_session());
    const auto relay_session = pipe_server.session();

    set_env("HAILSTONE_DRIVE_PIPE_ENDPOINT", relay_session.endpoint);
    set_env("HAILSTONE_DRIVE_PIPE_TOKEN", relay_session.token);
    set_env("HAILSTONE_DRIVE_PIPE_CHILD", "1");
    platform::ChildProcesses child;
    std::string start_error;
    REQUIRE_MESSAGE(child.start_tracked("drive-relay", {exe->string(), "-tc=drive_pipe_relay_child"}, start_error),
                    start_error);

    auto accepted = pipe_server.accept(15000);
    REQUIRE_MESSAGE(accepted != nullptr, pipe_server.error());
    if (accepted) {
        auto channel = std::make_shared<LocalRelayChannel>(std::move(accepted));
        net::ConnectionOptions options = quick_options();
        options.session_factory = [channel] { return channel->make_session(); };
        net::GameConnection conn(game, std::move(options));
        auto step = [&] {
            conn.update();
            server.poll();
        };

        conn.start(server.ticket());
        REQUIRE(pump(step, [&] { return conn.state() == ConnectionState::CharacterSelect; }));

        conn.enter_world("Gennro");
        REQUIRE(pump(step, [&] { return conn.state() == ConnectionState::InZone; }));
        REQUIRE(pump(step, [&] { return conn.player_status().max_hp != 0; }));
        CHECK(conn.world().spawns.count(101) == 1);
        CHECK(server.zone_entries == 1);

        conn.set_player_motion(glm::vec3(1.0f, 2.0f, 3.0f), 128.0f, glm::vec3(0.0f), false);
        conn.send_position_now();
        REQUIRE(pump(step, [&] { return !server.client_updates.empty(); }));

        const std::size_t before = static_cast<std::size_t>(MockEqServer::count(server.zone_log, Op::ChannelMessage));
        conn.send_command("/say hello");
        REQUIRE(pump(step, [&] {
            return static_cast<std::size_t>(MockEqServer::count(server.zone_log, Op::ChannelMessage)) > before;
        }));

        server.request_zone_change(1, 20.0f, 10.0f, 3.0f);
        REQUIRE(pump(step, [&] {
            return server.zone_entries >= 2 && conn.state() == ConnectionState::InZone && conn.zone_id() == 1;
        }));
        CHECK(conn.world().spawns.count(201) == 1);
        CHECK(conn.world().spawns.count(101) == 0);
        CHECK(conn.error().empty());
    }

    // The GameConnection and the channel are gone now, so the pipe is closed and the child can exit.
    bool exited = false;
    for (int i = 0; i < 500 && !exited; ++i) {
        child.poll();
        for (const auto& e : child.take_exited()) exited = e.exit_code == 0;
        if (!exited) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!exited) child.stop_all(true);
    CHECK(exited);
}
