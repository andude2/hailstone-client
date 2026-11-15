#include "platform/tcp_socket.h"

#include <algorithm>
#include <cstring>
#include <format>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace hailstone::platform {

namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
const NativeSocket kInvalid = INVALID_SOCKET;
using SockLen = int;

struct WinsockInit {
    bool ok = false;
    WinsockInit() {
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockInit() {
        if (ok) WSACleanup();
    }
};

bool ensure_init() {
    static WinsockInit init;
    return init.ok;
}

int last_socket_error() { return WSAGetLastError(); }
bool would_block(int e) { return e == WSAEWOULDBLOCK; }
bool in_progress(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
std::string error_text(int e) { return std::format("winsock error {}", e); }
void close_native(NativeSocket s) { closesocket(s); }
bool set_nonblocking(NativeSocket s) {
    u_long nonblocking = 1;
    return ioctlsocket(s, FIONBIO, &nonblocking) == 0;
}
constexpr int kSendFlags = 0;
// winsock2.h's since Windows 7 SP1; older SDKs lack the name.
#ifndef WSA_FLAG_NO_HANDLE_INHERIT
#define WSA_FLAG_NO_HANDLE_INHERIT 0x80
#endif
void keep_from_children(NativeSocket s) {
    if (s != kInvalid) SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
}
bool inheritable(NativeSocket s) {
    DWORD flags = 0;
    return GetHandleInformation(reinterpret_cast<HANDLE>(s), &flags) && (flags & HANDLE_FLAG_INHERIT) != 0;
}
// A socket no child process inherits (SDL starts the helpers with inheritable pipes, so handles are passed on).
// WSA_FLAG_OVERLAPPED is what socket() gives; a system without the no-inherit flag takes the plain call instead.
NativeSocket open_socket(int af, int type, int protocol) {
    NativeSocket s = WSASocketW(af, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == kInvalid) {
        s = ::socket(af, type, protocol);
        keep_from_children(s);
    }
    return s;
}
NativeSocket accept_socket(NativeSocket listener) {
    NativeSocket s = ::accept(listener, nullptr, nullptr);
    keep_from_children(s);
    return s;
}
#else
using NativeSocket = int;
const NativeSocket kInvalid = -1;
using SockLen = socklen_t;

bool ensure_init() { return true; }
int last_socket_error() { return errno; }
bool would_block(int e) { return e == EAGAIN || e == EWOULDBLOCK || e == EINTR; }
bool in_progress(int e) { return e == EINPROGRESS || e == EINTR; }
std::string error_text(int e) { return std::strerror(e); }
void close_native(NativeSocket s) { ::close(s); }
bool set_nonblocking(NativeSocket s) {
    int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
void keep_from_children(NativeSocket s) {
    if (s == kInvalid) return;
    const int flags = fcntl(s, F_GETFD);
    if (flags >= 0) fcntl(s, F_SETFD, flags | FD_CLOEXEC);
}
bool inheritable(NativeSocket s) {
    const int flags = fcntl(s, F_GETFD);
    return flags >= 0 && (flags & FD_CLOEXEC) == 0;
}
// A socket closed across exec, so no child process (the browser helper) holds it. SOCK_CLOEXEC where the system has
// it (Linux, the BSDs), set at creation; macOS has only fcntl after it.
NativeSocket open_socket(int af, int type, int protocol) {
#ifdef SOCK_CLOEXEC
    NativeSocket s = ::socket(af, type | SOCK_CLOEXEC, protocol);
    if (s != kInvalid) return s;
#endif
    NativeSocket plain = ::socket(af, type, protocol);
    keep_from_children(plain);
    return plain;
}
NativeSocket accept_socket(NativeSocket listener) {
#if defined(__linux__) && defined(SOCK_CLOEXEC)
    NativeSocket s = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
#else
    NativeSocket s = ::accept(listener, nullptr, nullptr);
    keep_from_children(s);
#endif
    return s;
}
// A write to a connection the peer closed must give an error, not SIGPIPE (macOS: SO_NOSIGPIPE at creation).
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif
#endif

NativeSocket native(std::intptr_t h) { return static_cast<NativeSocket>(h); }
std::intptr_t widen(NativeSocket s) { return static_cast<std::intptr_t>(s); }
bool valid(std::intptr_t h) { return native(h) != kInvalid; }

SockLen to_sockaddr(const Endpoint& ep, sockaddr_storage& ss) {
    std::memset(&ss, 0, sizeof ss);
    if (ep.family == Endpoint::Family::V4) {
        sockaddr_in sin{};
        sin.sin_family = AF_INET;
        sin.sin_port = htons(ep.port);
        std::memcpy(&sin.sin_addr, ep.address.data(), 4);
        std::memcpy(&ss, &sin, sizeof sin);
        return static_cast<SockLen>(sizeof sin);
    }
    sockaddr_in6 sin6{};
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port = htons(ep.port);
    std::memcpy(&sin6.sin6_addr, ep.address.data(), 16);
    sin6.sin6_scope_id = ep.scope_id; // a link-local address names its interface
    std::memcpy(&ss, &sin6, sizeof sin6);
    return static_cast<SockLen>(sizeof sin6);
}

std::optional<Endpoint> from_sockaddr(const sockaddr* sa) {
    Endpoint ep;
    if (sa->sa_family == AF_INET) {
        sockaddr_in sin;
        std::memcpy(&sin, sa, sizeof sin);
        ep.family = Endpoint::Family::V4;
        std::memcpy(ep.address.data(), &sin.sin_addr, 4);
        ep.port = ntohs(sin.sin_port);
        return ep;
    }
    if (sa->sa_family == AF_INET6) {
        sockaddr_in6 sin6;
        std::memcpy(&sin6, sa, sizeof sin6);
        ep.family = Endpoint::Family::V6;
        std::memcpy(ep.address.data(), &sin6.sin6_addr, 16);
        ep.port = ntohs(sin6.sin6_port);
        ep.scope_id = sin6.sin6_scope_id;
        return ep;
    }
    return std::nullopt;
}

// A new stream socket's options: non-blocking, no Nagle delay (the services' messages are small and answered one
// by one), no SIGPIPE on macOS.
bool prepare(NativeSocket s, std::string& error) {
    if (!set_nonblocking(s)) {
        error = "cannot make socket non-blocking: " + error_text(last_socket_error());
        return false;
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), static_cast<SockLen>(sizeof one));
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, static_cast<SockLen>(sizeof one));
#endif
    return true;
}

// Whether the socket can be written to now (a connect finished, one way or the other).
bool writable_now(NativeSocket s) {
#ifdef _WIN32
    WSAPOLLFD pfd{};
    pfd.fd = s;
    pfd.events = POLLWRNORM;
    return WSAPoll(&pfd, 1, 0) > 0;
#else
    pollfd pfd{};
    pfd.fd = s;
    pfd.events = POLLOUT;
    return ::poll(&pfd, 1, 0) > 0;
#endif
}

} // namespace

TcpSocket::~TcpSocket() { close(); }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept
    : handle_(other.handle_), state_(other.state_), error_(std::move(other.error_)), to_(std::move(other.to_)),
      connect_timeout_(other.connect_timeout_), connect_started_(other.connect_started_) {
    other.handle_ = widen(kInvalid);
    other.state_ = State::Closed;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        state_ = other.state_;
        error_ = std::move(other.error_);
        to_ = std::move(other.to_);
        connect_timeout_ = other.connect_timeout_;
        connect_started_ = other.connect_started_;
        other.handle_ = widen(kInvalid);
        other.state_ = State::Closed;
    }
    return *this;
}

void TcpSocket::close() {
    if (valid(handle_)) close_native(native(handle_));
    handle_ = widen(kInvalid);
    state_ = State::Closed;
    to_.clear();
}

void TcpSocket::fail(const std::string& what) {
    if (error_.empty()) error_ = what;
    close();
}

bool TcpSocket::connect(const Endpoint& to, std::string& error) { return connect(std::vector<Endpoint>{to}, error); }

bool TcpSocket::connect(std::vector<Endpoint> to, std::string& error) {
    close();
    error_.clear();
    to_ = std::move(to);
    std::reverse(to_.begin(), to_.end()); // taken from the back
    error = "no address to connect to";
    while (!to_.empty()) {
        Endpoint ep = to_.back();
        to_.pop_back();
        if (start(ep, error)) {
            error.clear();
            return true;
        }
    }
    return false;
}

bool TcpSocket::start(const Endpoint& to, std::string& error) {
    if (!ensure_init()) {
        error = "socket library initialisation failed";
        return false;
    }
    NativeSocket s = open_socket(to.family == Endpoint::Family::V4 ? AF_INET : AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalid) {
        error = "socket(): " + error_text(last_socket_error());
        return false;
    }
    if (!prepare(s, error)) {
        close_native(s);
        return false;
    }
    sockaddr_storage ss;
    SockLen len = to_sockaddr(to, ss);
    if (::connect(s, reinterpret_cast<const sockaddr*>(&ss), len) != 0) {
        int e = last_socket_error();
        if (!in_progress(e)) {
            error = std::format("connect({}): {}", to.to_string(), error_text(e));
            close_native(s);
            return false;
        }
        state_ = State::Connecting;
        connect_started_ = std::chrono::steady_clock::now();
    } else {
        state_ = State::Connected;
    }
    handle_ = widen(s);
    return true;
}

void TcpSocket::next_address(const std::string& why) {
    if (valid(handle_)) close_native(native(handle_));
    handle_ = widen(kInvalid);
    state_ = State::Closed;
    std::string error = why;
    while (!to_.empty()) {
        Endpoint ep = to_.back();
        to_.pop_back();
        if (start(ep, error)) return;
    }
    fail(error);
}

TcpSocket::State TcpSocket::state() {
    if (state_ == State::Connecting && writable_now(native(handle_))) {
        int err = 0;
        SockLen len = static_cast<SockLen>(sizeof err);
        if (getsockopt(native(handle_), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) != 0)
            err = last_socket_error();
        if (err != 0) next_address("connect: " + error_text(err));
        else state_ = State::Connected;
    }
    // Also what reports a refused connect where WSAPoll does not (Windows 10 before 2004): it times out instead.
    if (state_ == State::Connecting && std::chrono::steady_clock::now() - connect_started_ >= connect_timeout_)
        next_address(std::format("connect: no answer in {} ms", connect_timeout_.count()));
    return state_;
}

std::optional<std::size_t> TcpSocket::send(std::span<const std::byte> data) {
    const State s = state();
    if (s == State::Closed) return std::nullopt;
    if (s == State::Connecting || data.empty()) return 0;
#ifdef _WIN32
    int n = ::send(native(handle_), reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()),
                   kSendFlags);
#else
    auto n = ::send(native(handle_), data.data(), data.size(), kSendFlags);
#endif
    if (n < 0) {
        int e = last_socket_error();
        if (would_block(e)) return 0;
        fail("send(): " + error_text(e));
        return std::nullopt;
    }
    return static_cast<std::size_t>(n);
}

std::optional<std::size_t> TcpSocket::recv(std::span<std::byte> buffer) {
    const State s = state();
    if (s == State::Closed) return std::nullopt;
    if (s == State::Connecting || buffer.empty()) return 0;
#ifdef _WIN32
    int n = ::recv(native(handle_), reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
#else
    auto n = ::recv(native(handle_), buffer.data(), buffer.size(), 0);
#endif
    if (n == 0) {
        fail("closed by the peer");
        return std::nullopt;
    }
    if (n < 0) {
        int e = last_socket_error();
        if (would_block(e)) return 0;
        fail("recv(): " + error_text(e));
        return std::nullopt;
    }
    return static_cast<std::size_t>(n);
}

TcpListener::~TcpListener() { close(); }

void TcpListener::close() {
    if (valid(handle_)) close_native(native(handle_));
    handle_ = widen(kInvalid);
}

bool TcpListener::listen(const Endpoint& at, std::string& error) {
    close();
    if (!ensure_init()) {
        error = "socket library initialisation failed";
        return false;
    }
    NativeSocket s = open_socket(at.family == Endpoint::Family::V4 ? AF_INET : AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalid) {
        error = "socket(): " + error_text(last_socket_error());
        return false;
    }
    if (!set_nonblocking(s)) {
        error = "cannot make socket non-blocking: " + error_text(last_socket_error());
        close_native(s);
        return false;
    }
    sockaddr_storage ss;
    SockLen len = to_sockaddr(at, ss);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&ss), len) != 0 || ::listen(s, 8) != 0) {
        error = "bind/listen: " + error_text(last_socket_error());
        close_native(s);
        return false;
    }
    handle_ = widen(s);
    return true;
}

std::optional<Endpoint> TcpListener::local_endpoint() const {
    if (!valid(handle_)) return std::nullopt;
    sockaddr_storage ss{};
    SockLen len = sizeof ss;
    if (getsockname(native(handle_), reinterpret_cast<sockaddr*>(&ss), &len) != 0) return std::nullopt;
    return from_sockaddr(reinterpret_cast<const sockaddr*>(&ss));
}

bool TcpSocket::inherited_by_children() const { return valid(handle_) && inheritable(native(handle_)); }

std::optional<TcpSocket> TcpListener::accept() {
    if (!valid(handle_)) return std::nullopt;
    NativeSocket s = accept_socket(native(handle_));
    if (s == kInvalid) return std::nullopt;
    std::string error;
    if (!prepare(s, error)) {
        close_native(s);
        return std::nullopt;
    }
    TcpSocket out;
    out.handle_ = widen(s);
    out.state_ = TcpSocket::State::Connected;
    return out;
}

} // namespace hailstone::platform
