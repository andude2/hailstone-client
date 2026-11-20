// D0 of the drive plan (docs/research/drive-headless.md): a GameConnection
// behind a mirror Session, with a relay owning the real UDP Session, must log
// in, play, and zone exactly like a direct connection. This is the transport
// spike; the mirror and relay here are test-only. The production versions add
// the authenticated pipe, the snapshot and the drive window.

#include <doctest/doctest.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mock_server/harness.h"
#include "net/session.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace {

class Relay;

// The GUI's side of one session: a Session over two queues, whose real peer is
// a Relay-owned Session to the mock server. State, error and packets are shared
// through the Relay's link table.
class RelaySession final : public Session {
public:
    RelaySession(std::shared_ptr<Relay> relay, std::uint64_t id);
    ~RelaySession() override;

    void connect(const std::string& host, std::uint16_t port) override;
    void update() override;
    SessionState state() const override;
    std::string error() const override;
    void send(const AppPacket& packet, bool reliable = true) override;
    std::optional<AppPacket> receive() override;
    void disconnect() override;

private:
    std::shared_ptr<Relay> relay_;
    std::uint64_t id_ = 0;
};

class Relay : public std::enable_shared_from_this<Relay> {
public:
    explicit Relay(SessionOptions options) : options_(std::move(options)) {}

    std::unique_ptr<Session> make_session() {
        const std::uint64_t id = next_id_++;
        links_[id] = Link{};
        return std::make_unique<RelaySession>(shared_from_this(), id);
    }

    void request_connect(std::uint64_t id, std::string host, std::uint16_t port) {
        Link& l = links_[id];
        l.host = std::move(host);
        l.port = port;
        l.connect_requested = true;
        l.state = SessionState::Connecting;
    }
    void request_disconnect(std::uint64_t id) {
        auto it = links_.find(id);
        if (it == links_.end()) return;
        if (it->second.real) it->second.real->disconnect();
        it->second.state = SessionState::Disconnecting;
    }
    void forget(std::uint64_t id) {
        auto it = links_.find(id);
        if (it == links_.end()) return;
        if (it->second.real) it->second.real->disconnect();
        links_.erase(it);
    }

    void send(std::uint64_t id, const AppPacket& packet, bool reliable) {
        links_[id].outgoing.emplace_back(packet, reliable);
    }
    std::optional<AppPacket> receive(std::uint64_t id) {
        auto it = links_.find(id);
        if (it == links_.end() || it->second.incoming.empty()) return std::nullopt;
        AppPacket p = std::move(it->second.incoming.front());
        it->second.incoming.pop_front();
        return p;
    }
    SessionState state(std::uint64_t id) const {
        auto it = links_.find(id);
        return it == links_.end() ? SessionState::Disconnected : it->second.state;
    }
    std::string error(std::uint64_t id) const {
        auto it = links_.find(id);
        return it == links_.end() ? std::string() : it->second.error;
    }

    // Pumps every real session: creates it on a connect request, moves
    // application packets both ways, and copies its state to the mirror.
    // Single-threaded with the test loop, like the mock server.
    void pump() {
        for (auto& entry : links_) {
            Link& l = entry.second;
            if (l.connect_requested) {
                l.connect_requested = false;
                l.real = Session::create(options_);
                l.real->connect(l.host, l.port);
                ++created_;
            }
            if (!l.real) continue;
            l.real->update();
            l.state = l.real->state();
            l.error = l.real->error();
            while (auto p = l.real->receive()) l.incoming.push_back(std::move(*p));
            if (l.state == SessionState::Connected) {
                while (!l.outgoing.empty()) {
                    auto [p, reliable] = std::move(l.outgoing.front());
                    l.outgoing.pop_front();
                    l.real->send(p, reliable);
                }
            } else if (l.state == SessionState::Disconnected) {
                // A packet the client sent while the relay was tearing this
                // session down has nowhere to go; do not let it become a
                // warning from Session::send.
                l.outgoing.clear();
            }
        }
    }

    std::size_t created() const { return created_; }

private:
    struct Link {
        std::unique_ptr<Session> real;
        std::string host;
        std::uint16_t port = 0;
        bool connect_requested = false;
        SessionState state = SessionState::Disconnected;
        std::string error;
        std::deque<AppPacket> incoming;
        std::deque<std::pair<AppPacket, bool>> outgoing;
    };

    SessionOptions options_;
    std::unordered_map<std::uint64_t, Link> links_;
    std::uint64_t next_id_ = 1;
    std::size_t created_ = 0;
};

RelaySession::RelaySession(std::shared_ptr<Relay> relay, std::uint64_t id)
    : relay_(std::move(relay)), id_(id) {}

RelaySession::~RelaySession() {
    if (relay_) relay_->forget(id_);
}

void RelaySession::connect(const std::string& host, std::uint16_t port) {
    if (relay_) relay_->request_connect(id_, host, port);
}

void RelaySession::update() {
    if (relay_) relay_->pump();
}

SessionState RelaySession::state() const {
    return relay_ ? relay_->state(id_) : SessionState::Disconnected;
}

std::string RelaySession::error() const {
    return relay_ ? relay_->error(id_) : std::string();
}

void RelaySession::send(const AppPacket& packet, bool reliable) {
    if (relay_) relay_->send(id_, packet, reliable);
}

std::optional<AppPacket> RelaySession::receive() {
    return relay_ ? relay_->receive(id_) : std::nullopt;
}

void RelaySession::disconnect() {
    if (relay_) relay_->request_disconnect(id_);
}

} // namespace

TEST_CASE("drive spike: a GameConnection plays through a mirror relay") {
    MockEqServer server;
    EmptyGameDir dir;
    GameDir game(dir.path());

    auto relay = std::make_shared<Relay>(quick_options().session);
    net::ConnectionOptions options = quick_options();
    options.session_factory = [relay] { return relay->make_session(); };
    net::GameConnection conn(game, std::move(options));

    auto step = [&] {
        conn.update();
        server.poll();
    };

    // World and zone-in, all through the mirror.
    conn.start(server.ticket());
    REQUIRE(pump(step, [&] { return conn.state() == ConnectionState::CharacterSelect; }));
    CHECK(relay->created() == 1);

    conn.enter_world("Gennro");
    REQUIRE(pump(step, [&] { return conn.state() == ConnectionState::InZone; }));
    REQUIRE(pump(step, [&] { return conn.player_status().max_hp != 0; }));
    CHECK(conn.world().spawns.count(101) == 1);
    CHECK(server.zone_entries == 1);
    CHECK(relay->created() >= 2); // the world and the first zone session

    // An outgoing packet the client-authoritative path produces.
    conn.set_player_motion(glm::vec3(1.0f, 2.0f, 3.0f), 128.0f, glm::vec3(0.0f), false);
    conn.send_position_now();
    REQUIRE(pump(step, [&] { return !server.client_updates.empty(); }));

    // A typed command goes through the same relay.
    const std::size_t before = static_cast<std::size_t>(MockEqServer::count(server.zone_log, Op::ChannelMessage));
    conn.send_command("/say hello");
    REQUIRE(pump(step, [&] {
        return static_cast<std::size_t>(MockEqServer::count(server.zone_log, Op::ChannelMessage)) > before;
    }));

    // The server asks the client to zone. This drops the world session, makes
    // a new zone session through the relay, and enters the new zone.
    server.request_zone_change(1, 20.0f, 10.0f, 3.0f);
    REQUIRE(pump(step, [&] {
        return server.zone_entries >= 2 && conn.state() == ConnectionState::InZone && conn.zone_id() == 1;
    }));
    CHECK(conn.world().spawns.count(201) == 1);
    CHECK(conn.world().spawns.count(101) == 0);
    CHECK(relay->created() >= 3); // world, first zone, second zone
    CHECK(conn.error().empty());
}
