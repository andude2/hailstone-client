#pragma once

// D3 control lease (docs/research/drive-headless.md): the headless host lends its live network sessions to a
// viewer over the local authenticated pipe. DriveRelay is the headless side: it keeps the real Sessions, relays
// their application packets to the viewer and sends the viewer's packets on them. DriveControlClient is the viewer
// side: its MirrorSessions are handed to an ordinary GameConnection through ConnectionOptions::session_factory.
//
// One viewer at a time. Only application packets cross the pipe: no socket, key, sequence number or retransmission
// state does. The headless must not call its own GameConnection::update() while a lease is live (exactly one model
// acts), and the viewer must not resume the headless on a stale state: it sends its snapshot back with Release().

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "net/drive_snapshot.h"
#include "net/packet.h"
#include "net/session.h"
#include "platform/local_pipe.h"

namespace hailstone::app {

// The wire kinds. Connect, Disconnect, Send, State and Packet carry a session id: the headless's world is 1 and its
// zone is 2, the ids the viewer's session_factory assigns in call order.
enum class DriveLeaseMessage : std::uint8_t {
    Connect = 1,    // id, port, host (viewer -> headless: a new real session, for a zone change)
    Disconnect = 2, // id (ignored for an adopted session: the headless owns it)
    Send = 3,       // id, reliable, opcode, data (viewer -> headless)
    State = 4,      // id, state, error (headless -> viewer)
    Packet = 5,     // id, opcode, data (headless -> viewer)
    Bye = 6,
    Snapshot = 7, // encode_drive_snapshot (headless -> viewer, once)
    Release = 8,  // encode_drive_snapshot (viewer -> headless)
};

// The headless side. Construct it with a fresh pipe session, write the endpoint and token where the viewer reads
// them, then pump() once a frame from the game thread.
class DriveRelay {
public:
    struct Impl;
    explicit DriveRelay(platform::LocalPipeSession session);
    ~DriveRelay();
    DriveRelay(const DriveRelay&) = delete;
    DriveRelay& operator=(const DriveRelay&) = delete;

    const platform::LocalPipeSession& session() const;
    // True once a viewer has authenticated. The snapshot may go then.
    bool viewer_connected() const;
    // A viewer connected and no lease has started for it yet. The headless lends only then, so a released lease is
    // never re-lent to its own closing pipe.
    bool viewer_waiting() const;
    // Start a lease for the waiting viewer: resets the previous lease's snapshot/release state and stores this one.
    void begin_lease(net::DriveSnapshot snapshot);
    // The state the viewer starts from (capture_snapshot). Set it before or after the viewer connects; it is sent
    // once, at the next pump().
    void set_snapshot(net::DriveSnapshot snapshot);
    // Install the live session under `id` (1 the world, 2 the zone). A null world is normal in a zone.
    void adopt(std::uint64_t id, std::unique_ptr<net::Session> session);
    // Game thread: relay the viewer's messages and pump the real sessions. Does nothing before a viewer connects.
    void pump();
    // The viewer sent Release: its snapshot is ready and the sessions must be taken back with take_session().
    bool released() const;
    net::DriveSnapshot take_release();
    std::unique_ptr<net::Session> take_session(std::uint64_t id);
    // The viewer's pipe went away (clean release or not).
    bool closed() const;

private:
    std::unique_ptr<Impl> impl_;
};

// The viewer side. connect() authenticates the pipe; make_session() hands a MirrorSession to a GameConnection's
// session_factory (the first call is the world, the second the zone). take_snapshot() is the initial state.
class DriveControlClient {
public:
    struct Impl;
    static std::unique_ptr<DriveControlClient> connect(const platform::LocalPipeSession& session, int timeout_ms,
                                                       std::string& error);
    ~DriveControlClient();
    DriveControlClient(const DriveControlClient&) = delete;
    DriveControlClient& operator=(const DriveControlClient&) = delete;

    std::unique_ptr<net::Session> make_session();
    std::optional<net::DriveSnapshot> take_snapshot();
    // A session's state as the headless reported it (a GameConnection must not send before its zone is Connected).
    bool session_connected(std::uint64_t id) const;
    void pump();
    // Send the viewer's whole state back and end the lease.
    void release(net::DriveSnapshot snapshot);
    bool closed() const;

private:
    explicit DriveControlClient(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

} // namespace hailstone::app
