#pragma once

// Non-blocking UDP socket and name resolution. The only place socket APIs
// are used (BSD sockets on Linux/macOS, Winsock on Windows).

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hailstone::platform {

// An IPv4 or IPv6 address and port, independent of the OS socket structs.
struct Endpoint {
    enum class Family : std::uint8_t { V4, V6 };

    Family family = Family::V4;
    std::array<std::uint8_t, 16> address{}; // V4 uses the first 4 bytes
    std::uint16_t port = 0;
    // V6: the interface a link-local (fe80::/10) address is on (sin6_scope_id); 0 for any other address.
    std::uint32_t scope_id = 0;

    static Endpoint v4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d, std::uint16_t port);

    // "1.2.3.4:5", "[::1]:5" or "[fe80::1%3]:5".
    std::string to_string() const;
    // The address alone, without the port (with "%scope" when it has one).
    std::string address_string() const;

    bool operator==(const Endpoint&) const = default;
};

// Resolves host (a name or numeric address) to endpoints, IPv4 first.
// Returns an empty list and sets error on failure.
std::vector<Endpoint> resolve(const std::string& host, std::uint16_t port, std::string& error);

// `host` as a numeric IPv4 or IPv6 address, without asking a name server (so it never waits); nullopt for a name.
std::optional<Endpoint> numeric_endpoint(const std::string& host, std::uint16_t port);

// resolve() on a thread of its own (at most four at once; more wait their turn); `done` is called on that thread
// with its result, and holds up the lookups waiting behind it while it runs. A lookup still running when
// the program exits is not waited for (getaddrinfo cannot be cut off, and an unreachable name server holds it for
// tens of seconds): it is left to the process's end, so `done` must own what it uses, and may never be called.
void resolve_async(std::string host, std::uint16_t port,
                   std::function<void(std::vector<Endpoint> endpoints, std::string error)> done);

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Opens a non-blocking socket bound to an ephemeral port on all
    // interfaces of the family, or to bind_to when given.
    bool open(Endpoint::Family family, std::string& error, const Endpoint* bind_to = nullptr);
    void close();
    bool is_open() const;
    // Whether a child process started now would get this socket (an inheritable handle, or no close-on-exec): false
    // for every socket opened here, so the browser helper and other children never hold the client's sockets.
    bool inherited_by_children() const;

    std::optional<Endpoint> local_endpoint() const;

    // Returns false on a hard error (the datagram is dropped either way on
    // a full send buffer, as UDP allows).
    bool send_to(const Endpoint& to, std::span<const std::byte> data);

    // Returns the datagram size, or nullopt when nothing is waiting or on
    // an error (see last_error()). A datagram larger than buffer is dropped
    // on every platform, not returned cut short. A call skips at most
    // kMaxSkippedPerRecv such drops and transient errors, then returns nullopt
    // with no error.
    static constexpr int kMaxSkippedPerRecv = 64;
    std::optional<std::size_t> recv_from(std::span<std::byte> buffer, Endpoint& from);

    // Waits until a datagram is readable or the timeout passes.
    bool wait_readable(std::chrono::milliseconds timeout) const;

    // Why the last send_to or recv_from failed; empty when it did not (each call clears it, as open() does).
    const std::string& last_error() const { return error_; }

private:
    // SOCKET on Windows is pointer-sized; int elsewhere. Stored widened.
    std::intptr_t handle_ = -1;
    std::string error_;
};

} // namespace hailstone::platform
