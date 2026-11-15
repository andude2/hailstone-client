#pragma once

// Non-blocking TCP connections, for the plugins' own services (the plugin SDK's tcp_* functions). With
// udp_socket.cpp the only place socket APIs are used (BSD sockets on Linux/macOS, Winsock on Windows).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "platform/udp_socket.h"

namespace hailstone::platform {

class TcpSocket {
public:
    enum class State : std::uint8_t { Closed, Connecting, Connected };

    TcpSocket() = default;
    ~TcpSocket();
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    // A connect that has not finished in this long is given up (kConnectTimeout by default): the system's own limit is
    // 21 s on Windows and about 2 min on Linux, and WSAPoll before Windows 10 2004 never reports a refused one.
    static constexpr std::chrono::milliseconds kConnectTimeout{10000};

    // Starts connecting without waiting: false (and error set) when the socket cannot even be made.
    bool connect(const Endpoint& to, std::string& error);
    // The same over a name's addresses (resolve()'s, IPv4 first): one that fails, at once or later (refused, or not
    // done within the timeout), hands over to the next. False (and error set) when none of them can even start.
    bool connect(std::vector<Endpoint> to, std::string& error);
    void set_connect_timeout(std::chrono::milliseconds timeout) { connect_timeout_ = timeout; }
    void close();

    // Connecting until the connection is made or refused (or times out, on the last address); Closed after an error
    // or the peer's close (last_error).
    State state();

    // The bytes the system took (0 while its buffer is full), or nullopt once the connection is closed.
    std::optional<std::size_t> send(std::span<const std::byte> data);
    // The bytes read (0 when nothing is waiting), or nullopt once the connection is closed and nothing is left.
    std::optional<std::size_t> recv(std::span<std::byte> buffer);

    const std::string& last_error() const { return error_; }
    // Whether a child process started now would get this socket (its handle inheritable, or no close-on-exec): false
    // for every socket made here, so the browser helper and other children never hold the client's connections.
    bool inherited_by_children() const;

private:
    friend class TcpListener;
    void fail(const std::string& what);
    // Starts the socket for one address: false (error set) when it cannot even start.
    bool start(const Endpoint& to, std::string& error);
    // The address being tried failed with `why`: the next one is started, or the socket fails.
    void next_address(const std::string& why);

    std::intptr_t handle_ = -1; // SOCKET on Windows is pointer-sized; int elsewhere. Stored widened.
    State state_ = State::Closed;
    std::string error_;
    std::vector<Endpoint> to_; // the addresses not yet tried, after the one being tried
    std::chrono::milliseconds connect_timeout_ = kConnectTimeout;
    std::chrono::steady_clock::time_point connect_started_{};
};

// A listening socket on one address (the tests' services).
class TcpListener {
public:
    TcpListener() = default;
    ~TcpListener();
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    // Binds and listens; port 0 picks a free one (local_endpoint says which).
    bool listen(const Endpoint& at, std::string& error);
    void close();
    std::optional<Endpoint> local_endpoint() const;
    // A waiting connection, already connected, or nullopt.
    std::optional<TcpSocket> accept();

private:
    std::intptr_t handle_ = -1;
};

} // namespace hailstone::platform
