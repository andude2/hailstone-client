#include "platform/local_pipe.h"

#include "core/byte_io.h"
#include "platform/tcp_socket.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace hailstone::platform {
namespace {

using Clock = std::chrono::steady_clock;
// A state snapshot is sent as one burst of 1 MiB chunks (up to the drive snapshot's 64 MiB): the queue holds it
// until the game thread drains, rather than failing the pipe at kLocalPipeMaxMessage * 2.
constexpr std::size_t kQueueLimit = kLocalPipeMaxMessage * 16;
constexpr std::string_view kHost = "127.0.0.1";

bool hex_char(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

std::uint16_t parse_port(std::string_view text) {
    if (text.empty() || text.size() > 5) throw std::invalid_argument("local pipe port is invalid");
    std::uint32_t port = 0;
    for (char c : text) {
        if (c < '0' || c > '9') throw std::invalid_argument("local pipe port is invalid");
        port = port * 10 + static_cast<std::uint32_t>(c - '0');
    }
    if (port > 65535) throw std::invalid_argument("local pipe port is invalid");
    return static_cast<std::uint16_t>(port);
}

Endpoint parse_endpoint(std::string_view endpoint) {
    const auto colon = endpoint.rfind(':');
    if (colon == std::string_view::npos || endpoint.substr(0, colon) != kHost)
        throw std::invalid_argument("local pipe endpoint must be 127.0.0.1:<port>");
    return Endpoint::v4(127, 0, 0, 1, parse_port(endpoint.substr(colon + 1)));
}

std::string format_endpoint(std::uint16_t port) { return std::string(kHost) + ":" + std::to_string(port); }

void validate_session(const LocalPipeSession& session) {
    (void)parse_endpoint(session.endpoint);
    if (session.token.size() != 64) throw std::invalid_argument("local pipe token is invalid");
    for (char c : session.token)
        if (!hex_char(c)) throw std::invalid_argument("local pipe token is invalid");
}

std::array<std::byte, 32> token_bytes(std::string_view token) {
    if (token.size() != 64) throw std::invalid_argument("local pipe token is invalid");
    auto nibble = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
        throw std::invalid_argument("local pipe token is invalid");
    };
    std::array<std::byte, 32> out{};
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<std::byte>((nibble(token[i * 2]) << 4) | nibble(token[i * 2 + 1]));
    return out;
}

std::string hexadecimal(std::span<const std::byte> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::byte byte : bytes) {
        const auto n = std::to_integer<unsigned>(byte);
        out.push_back(digits[n >> 4]);
        out.push_back(digits[n & 15]);
    }
    return out;
}

bool token_equal(std::span<const std::byte, 32> a, std::span<const std::byte, 32> b) {
    unsigned different = 0;
    for (std::size_t i = 0; i < a.size(); ++i) different |= std::to_integer<unsigned>(a[i] ^ b[i]);
    return different == 0;
}

bool send_all(TcpSocket& socket, std::span<const std::byte> data, Clock::time_point deadline) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        if (Clock::now() >= deadline) return false;
        const auto n = socket.send(data.subspan(sent));
        if (!n) return false;
        if (*n) sent += *n;
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

bool recv_exact(TcpSocket& socket, std::span<std::byte> data, Clock::time_point deadline) {
    std::size_t got = 0;
    while (got < data.size()) {
        if (Clock::now() >= deadline) return false;
        const auto n = socket.recv(data.subspan(got));
        if (!n) return false;
        if (*n) got += *n;
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

} // namespace

bool local_pipe_available() noexcept { return true; }

LocalPipeSession make_local_pipe_session() {
    // Not a cryptographic RNG; the token is a capability checked before any message, and D0's transport is loopback
    // only. A named-pipe/Unix-domain-socket backend moves the secret to the OS's protected handles.
    std::random_device random;
    std::array<std::byte, 32> token{};
    for (std::byte& byte : token) byte = static_cast<std::byte>(random() & 0xff);
    return {format_endpoint(0), hexadecimal(token)};
}

struct LocalPipeConnection::Impl {
    TcpSocket socket;
    std::thread reader;
    std::atomic<bool> stop{false};
    std::mutex write_mutex, state_mutex, queue_mutex;
    std::condition_variable queue_cv;
    std::deque<std::vector<std::byte>> queue;
    std::size_t queued_bytes = 0;
    std::atomic<bool> closed{false};
    std::string error;

    void start_reader() { reader = std::thread([this] { reader_loop(); }); }

    void fail(std::string why) {
        {
            std::lock_guard lock(state_mutex);
            if (closed.load()) return;
            closed.store(true);
            error = std::move(why);
        }
        queue_cv.notify_all();
        socket.close();
    }

    void reader_loop() noexcept {
        std::vector<std::byte> buffer;
        std::array<std::byte, 65536> chunk{};
        while (!stop.load(std::memory_order_relaxed)) {
            const auto n = socket.recv(chunk);
            if (!n) {
                fail("the local pipe peer closed");
                return;
            }
            if (*n == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*n));
            while (buffer.size() >= 4) {
                ByteReader length(std::span<const std::byte>(buffer).first(4));
                const std::uint32_t size = length.le<std::uint32_t>();
                if (size > kLocalPipeMaxMessage) {
                    fail("local pipe message is too large");
                    return;
                }
                if (buffer.size() < 4u + size) break;
                std::vector<std::byte> message(buffer.begin() + 4, buffer.begin() + 4 + static_cast<std::ptrdiff_t>(size));
                buffer.erase(buffer.begin(), buffer.begin() + 4 + static_cast<std::ptrdiff_t>(size));
                {
                    std::lock_guard lock(queue_mutex);
                    if (queued_bytes + message.size() > kQueueLimit) {
                        fail("local pipe receive queue is full");
                        return;
                    }
                    queued_bytes += message.size();
                    queue.push_back(std::move(message));
                }
                queue_cv.notify_one();
            }
        }
    }

    void close() noexcept {
        if (stop.exchange(true)) return;
        socket.close();
        if (reader.joinable() && reader.get_id() != std::this_thread::get_id()) reader.join();
        closed.store(true);
        queue_cv.notify_all();
    }

    bool send(std::span<const std::byte> body, int timeout_ms) {
        if (body.size() > kLocalPipeMaxMessage) {
            fail("local pipe message is too large");
            return false;
        }
        std::lock_guard write_lock(write_mutex);
        {
            if (closed.load()) return false;
        }
        ByteWriter length;
        length.le(static_cast<std::uint32_t>(body.size()));
        const auto& length_bytes = length.data();
        const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(1, timeout_ms));
        if (!send_all(socket, std::span<const std::byte>(length_bytes.data(), length_bytes.size()), deadline) ||
            (!body.empty() && !send_all(socket, body, deadline))) {
            fail("the local pipe peer or transport failed");
            return false;
        }
        return true;
    }

    std::optional<std::vector<std::byte>> receive(int timeout_ms) {
        std::unique_lock lock(queue_mutex);
        if (timeout_ms < 0) {
            queue_cv.wait(lock, [this] { return closed.load() || !queue.empty(); });
        } else if (!queue_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                      [this] { return closed.load() || !queue.empty(); })) {
            return std::nullopt;
        }
        if (queue.empty()) return std::nullopt;
        std::vector<std::byte> body = std::move(queue.front());
        queue.pop_front();
        queued_bytes -= body.size();
        return body;
    }
};

struct LocalPipeServer::Impl {
    TcpListener listener;
    LocalPipeSession session;
    std::string error;

    explicit Impl(LocalPipeSession s) : session(std::move(s)) {
        validate_session(session);
        const Endpoint at = parse_endpoint(session.endpoint);
        std::string listen_error;
        if (!listener.listen(at, listen_error)) throw std::runtime_error("local pipe could not listen: " + listen_error);
        const auto bound = listener.local_endpoint();
        if (!bound) throw std::runtime_error("local pipe has no local endpoint");
        session.endpoint = format_endpoint(bound->port);
    }

    std::unique_ptr<LocalPipeConnection> accept(int timeout_ms) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(1, timeout_ms));
        while (Clock::now() < deadline) {
            auto accepted = listener.accept();
            if (!accepted) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            const auto auth_deadline = Clock::now() + std::chrono::seconds(5);
            std::array<std::byte, 32> received{};
            if (!recv_exact(*accepted, received, auth_deadline) ||
                !token_equal(received, token_bytes(session.token))) {
                error = "local pipe authentication failed";
                return nullptr;
            }
            const std::byte ok{1};
            if (!send_all(*accepted, std::span(&ok, 1), auth_deadline)) {
                error = "local pipe authentication failed";
                return nullptr;
            }
            auto impl = std::make_unique<LocalPipeConnection::Impl>();
            impl->socket = std::move(*accepted);
            impl->start_reader();
            return std::unique_ptr<LocalPipeConnection>(new LocalPipeConnection(std::move(impl)));
        }
        error = "local pipe peer did not connect";
        return nullptr;
    }
};

LocalPipeConnection::LocalPipeConnection(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LocalPipeConnection::~LocalPipeConnection() { close(); }

std::unique_ptr<LocalPipeConnection> LocalPipeConnection::connect(const LocalPipeSession& session, int timeout_ms) {
    validate_session(session);
    TcpSocket socket;
    const Endpoint at = parse_endpoint(session.endpoint);
    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(1, timeout_ms));
    std::string connect_error;
    if (!socket.connect(at, connect_error)) throw std::runtime_error("local pipe could not connect: " + connect_error);
    while (socket.state() == TcpSocket::State::Connecting) {
        if (Clock::now() >= deadline) throw std::runtime_error("local pipe connection timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (socket.state() != TcpSocket::State::Connected)
        throw std::runtime_error("local pipe connection failed: " + socket.last_error());
    const auto auth_deadline = Clock::now() + std::chrono::seconds(5);
    if (!send_all(socket, token_bytes(session.token), auth_deadline))
        throw std::runtime_error("local pipe authentication failed");
    std::array<std::byte, 1> accepted{};
    if (!recv_exact(socket, accepted, auth_deadline) || accepted[0] != std::byte{1})
        throw std::runtime_error("local pipe authentication failed");
    auto impl = std::make_unique<Impl>();
    impl->socket = std::move(socket);
    impl->start_reader();
    return std::unique_ptr<LocalPipeConnection>(new LocalPipeConnection(std::move(impl)));
}

bool LocalPipeConnection::send_message(std::span<const std::byte> body, int timeout_ms) {
    return impl_ && impl_->send(body, timeout_ms);
}

std::optional<std::vector<std::byte>> LocalPipeConnection::receive_message(int timeout_ms) {
    return impl_ ? impl_->receive(timeout_ms) : std::nullopt;
}

void LocalPipeConnection::close() noexcept {
    if (impl_) impl_->close();
}

bool LocalPipeConnection::closed() const {
    if (!impl_) return true;
    return impl_->closed.load();
}

std::string LocalPipeConnection::error() const {
    if (!impl_) return {};
    std::lock_guard lock(impl_->state_mutex);
    return impl_->error;
}

LocalPipeServer::LocalPipeServer(const LocalPipeSession& session) {
    impl_ = std::make_unique<Impl>(session);
}

LocalPipeServer::~LocalPipeServer() = default;

std::unique_ptr<LocalPipeConnection> LocalPipeServer::accept(int timeout_ms) {
    return impl_ ? impl_->accept(timeout_ms) : nullptr;
}

const LocalPipeSession& LocalPipeServer::session() const { return impl_->session; }

std::string LocalPipeServer::error() const { return impl_ ? impl_->error : std::string(); }

} // namespace hailstone::platform
