#pragma once

// A local, authenticated message pipe for the drive relay (docs/research/drive-headless.md).
//
// D0 uses a loopback TCP stream on 127.0.0.1: the named-pipe/Unix-domain-socket backend is later hardening, and this
// header's endpoint keeps the shape both can share. A message is a 4-byte little-endian length and that many bytes,
// bounded by kLocalPipeMaxMessage. The client must present the 256-bit token before either side exchanges messages;
// the server binds loopback only, so nothing is reachable from another machine. The endpoint and token are for the
// process that owns the session, never for a command line or a log.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hailstone::platform {

inline constexpr std::size_t kLocalPipeMaxMessage = std::size_t{8} << 20;

struct LocalPipeSession {
    std::string endpoint; // "127.0.0.1:<port>"; port 0 asks the server for a free one
    std::string token;    // 64 lowercase hexadecimal characters (32 bytes)
};

bool local_pipe_available() noexcept;
// Windows only for now. The other platforms throw std::runtime_error, so a caller can say why the drive pipe is
// unavailable rather than opening something weaker.
LocalPipeSession make_local_pipe_session();

// The connecting side of one authenticated pipe. receive_message(timeout_ms) returns nullopt on timeout, on close,
// or when the peer closed; error() then says which. A reader thread owns the pipe's reads, so a caller may send and
// receive from one thread without blocking the other direction.
class LocalPipeConnection {
public:
    struct Impl; // the platform side, defined in local_pipe.cpp
    explicit LocalPipeConnection(std::unique_ptr<Impl> impl);
    ~LocalPipeConnection();
    LocalPipeConnection(const LocalPipeConnection&) = delete;
    LocalPipeConnection& operator=(const LocalPipeConnection&) = delete;

    static std::unique_ptr<LocalPipeConnection> connect(const LocalPipeSession& session, int timeout_ms = 10000);

    bool send_message(std::span<const std::byte> body, int timeout_ms = 5000);
    // timeout_ms < 0 waits for a message or close; 0 polls the queue.
    std::optional<std::vector<std::byte>> receive_message(int timeout_ms = 0);
    void close() noexcept;
    bool closed() const;
    std::string error() const;

private:
    std::unique_ptr<Impl> impl_;
};

// The listening side: one instance, one accepted peer. Construct it before starting the peer, then accept().
class LocalPipeServer {
public:
    explicit LocalPipeServer(const LocalPipeSession& session);
    ~LocalPipeServer();
    LocalPipeServer(const LocalPipeServer&) = delete;
    LocalPipeServer& operator=(const LocalPipeServer&) = delete;

    std::unique_ptr<LocalPipeConnection> accept(int timeout_ms = 10000);
    // The bound session: its endpoint has the port the listener actually got.
    const LocalPipeSession& session() const;
    std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hailstone::platform
