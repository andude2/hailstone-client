# Driving a headless character from the graphical client

**Status: plan; D0 (transport), D1 (snapshot) and D2's data path passed; the
same-window swap is built but not run live.** This page works out whether one
Hailstone client can borrow another character's session long enough to show
that character's UI in character A's own window (not a second process), and how
to build it without moving a live session between processes. Tests:
`tests/mock_drive_spike_test.cpp`, `tests/local_pipe_test.cpp`,
`tests/mock_drive_pipe_test.cpp`, `tests/drive_snapshot_test.cpp`,
`tests/drive_observe_test.cpp` and `tests/drive_client_test.cpp`. The
production pipe hardening, the live swap check and the control lease are still
ahead.

## The short answer

Yes, with one important change of direction. The obvious designs — logging the
graphical client in as the other character, or moving the headless character's
socket into the graphical client — either lose the headless character's state
or require exporting a live, encrypted UDP session. The workable design keeps
the session exactly where it is and makes the headless host lend it, packet by
packet, to a normal `GameConnection` in the graphical client:

```
char A's GUI (hailstone.exe)                  headless host (hailstone_headless)
+---------------------------+                 +-------------------------------+
| ClientApp for A           |                 | HeadlessApp                   |
|   ...                     |                 |   GameConnection for L       |
| Drive window / drive      |   local pipe    |   DriveRelay                 |
| client (a second process) |<--------------->|     world + zone Session     |
|   ClientApp for L         |   AppPackets    |                               |
|     MirrorSession         |                 +---------------+---------------+
+---------------------------+                                 |
                                                         EQEmu server
```

The graphical client runs the character's real game logic and real UI. The
headless host keeps the real network session, its encryption, sequence numbers,
retries and keepalives. On attach the headless sends a state snapshot; in a
read-only **observe** lease the headless keeps playing and only sends packets;
in a **control** lease it freezes, the graphical client drives, and on release
the graphical client sends the state snapshot back. No TCP/UDP socket changes
hands, no second login happens, and the character never camps or zones out.

There are therefore two useful first steps, and they should be built in this
order:

- **Observe:** show L's real UI and scene while L's automation keeps running.
  The GUI drops every outgoing packet; the headless never notices the viewer.
  This is the low-risk way to answer "let me look at L".
- **Control:** pause L's automation, let the GUI send actions, and hand the
  exact state back when the drive ends. This is the full "briefly play L".

The first usable version puts the driven character in a **separate drive
window** (its own process, launched by char A's GUI). Putting the driven
character in **the same window** as A is possible, but it is a much larger
change to `ClientApp` and is a later milestone, not part of the first one.

## What exists today

- `hailstone_headless` (`src/app/headless_app.cpp`, `docs/headless.md`) runs the
  same `net::GameConnection` and `app::Automation` as the graphical client, with
  no window, Vulkan device or UI. It already loads the zone as a collision mesh,
  runs movement, and handles zoning.
- The graphical client is one `ClientApp` (`src/app/client_app_internal.h`) with
  one `conn_` (`std::unique_ptr<net::GameConnection>`) and one set of per-zone
  renderers, UI panels and input state. There is no second view and no remote
  session.
- `net::GameConnection` owns one world `Session` and one zone `Session`
  (`src/net/session.h`). A `Session` is the client's SOE UDP protocol: it holds
  the connected socket, the server's crypto parameters and sequence state, the
  reliable send window, retries, acks and keepalives. `UdpSocket` deliberately
  keeps its handle out of child processes (`inherited_by_children`).
- `GameConnection` has a `zone_packet_tap` and test hooks that feed packets
  without a session (`receive_zone_packet_for_test`, `set_profile_for_test`).
  These are the seams a drive mode can use, but they are not a state-transfer
  format.
- `platform/gpu_ipc*` is an authenticated local transport built for the GPU
  asset broker. It is a useful pattern (named pipe, random name and token,
  current-user DACL, peer process id), but it is Windows-only and its schema is
  for GPU handles, not game traffic.

## Why the obvious designs do not work

| Design | What happens | Why it is not the answer |
|---|---|---|
| Log the GUI in as L after camping the headless L | The server sees one login; it is simple and uses the existing UI. | Camping removes L from its group and raid and loses its pet; it stops the crew's automation; a relogin and zone-in take time; char A's own GUI has to camp too, since it is logged in as another account. `/camp` can also be refused in combat. |
| Log the GUI in as L without camping the headless L | The server sees two sessions for one character and kicks one. | The headless L gets a link death; the crew supervisor may relaunch it and fight the GUI for the character. |
| Move L's socket and `Session` into the GUI | The GUI becomes a genuinely native client for L, with no proxy. | It needs a cross-process handle transfer plus the entire `Session` state (crypto, sequence, reliable window, queued packets), and then a `GameConnection` state transfer. It is the highest-risk route and it breaks `UdpSocket`'s deliberate no-inheritance rule. It is not needed for the first version. |
| Run a second `ClientApp` in the same process | Switching would be a pointer swap. | `ClientApp` has one `conn_` and hundreds of per-connection members (renderers, `WorldObjects`, UI panels, camera, chat, input). A second view is a refactor of the client, not a small feature. |
| Render L remotely and stream frames to A | No state transfer; A is a terminal. | The headless has no Vulkan device, no scene assets and no renderer, and adding them defeats the reason it is headless. The GPU sharing work shares immutable textures, not frames. |

## The design: a drive relay, a mirror session, and a snapshot

### 1. The headless keeps the transport

In an **observe** lease the headless keeps its own `GameConnection` running. A
small `DriveRelay` is fed a copy of every application packet the real
`Session`s receive and writes those to the viewer; it drops everything the
viewer sends. The headless's model is untouched, so automation keeps playing and
the viewer can come and go.

In a **control** lease the headless pauses its automation and movement, stops
feeding its own `GameConnection` from the sessions, and gives the sessions to
the relay. The relay then carries packets both ways.

In both modes the relay speaks `AppPacket`s over a local authenticated pipe: it
receives the server's application packets from the real `Session`s and writes
them to the drive client; in control mode it receives the drive client's
`AppPacket`s and sends them on the real sessions with the same reliability flag.

The real `Session` still does everything a session does: the handshake,
crypto, sequence numbers, acks, retransmission, keepalives and idle timeout. The
GUI never sees a key, a sequence number or a socket handle. The server sees the
same UDP five-tuple it was already talking to.

A zone change works the way it always does. The world session sends
`OP_ZoneServerInfo`; the GUI's `GameConnection` asks its `MirrorSession` to
connect; the relay creates a real zone `Session` to that address, and the GUI
sends `OP_ZoneEntry` through the relay. This is exactly the sequence a normal
client produces; only the packets take a detour through a pipe.

The relay must not become a general-purpose proxy. It accepts a new session
only for the zone server the world server named (`OP_ZoneServerInfo`), and it
validates every IPC frame. The trust boundary is the local pipe and its token,
not the game packets.

### 2. The GUI is a normal `GameConnection` behind a `MirrorSession`

`net::Session` is already an abstract interface (`create`, `connect`, `update`,
`state`, `error`, `send`, `receive`, `disconnect`, `stats`). A
`net::MirrorSession` implements it over the drive pipe:

- `connect(host, port)` sends a `Connect` request to the relay and returns.
- `send(packet, reliable)` sends a `Send` frame.
- `receive()` returns the next packet from a local queue.
- `update()` pumps the pipe and reports `Connected`.
- `stats()` reports zeros (or the relay's counters, later).

`GameConnection` gets a drive start path: a new world and zone mirror are
created, `state_` starts at `InZone`, the world login is marked already sent,
`character_` and the profile come from the snapshot, and `update()` does the
normal packet handling. From that point every existing UI window, renderer,
Lua `hs.*` call and input path works unchanged, because they all read
`GameConnection` and the `ClientApp` state built around it. That is the main
reason to choose this design.

The drive client needs one guard rail: its close, camp, logout and character
select paths must **release the lease**, not camp L. `/camp`, `/quit`,
`/logout`, `/exit` and the window's close button are intercepted in drive mode.
A deliberate `/camp` from the player is either refused or offered as "release
and return to the headless", depending on the UX decision.

### The seams this needs in `GameConnection`

The design adds as little as possible to the game logic:

- `ConnectionOptions::session_factory` (in place for D0): a
  `std::function<std::unique_ptr<Session>()>` used by `connect_world` and the
  zone connect path through `GameConnection::create_session`, instead of
  calling `Session::create` directly. The headless leaves it empty; the drive
  client installs one that returns a `MirrorSession` with the relay's session
  id. This keeps `GameConnection`'s state machine intact and avoids branching
  on a mode in every send and update path.
- `ConnectionOptions::drive_packet_tap`: called for every application packet in
  both directions (or at least incoming plus the outgoing packets that carry
  client-authoritative state). The observe relay needs a tee that does not
  steal the single `Session::receive()` result from the headless's own
  `GameConnection`; an outgoing tap lets the viewer follow position and target.
  The relay's snapshot/delta stream covers the rest.
- `start_drive(world_id, zone_id, character, profile)`: creates the two mirror
  sessions, sets `state_ = InZone`, `zone_ready_ = true`, `world_login_sent_ =
  true`, `character_`, and leaves the rest to the snapshot.
- `release_sessions()` and `adopt_sessions(world, zone, state)`: the headless
  side's seams. `release_sessions` moves the two `unique_ptr<Session>`s out of
  `GameConnection`; `adopt_sessions` installs the relay's current sessions into
  a fresh `GameConnection` before the snapshot is applied.
- `capture_snapshot()` / `apply_snapshot()`: implemented next to
  `GameConnection`'s state (or as friends), because the state is private and the
  existing public surface does not expose all of it. These are the only places
  that enumerate state for transfer.

The headless's loop must then know three modes: `Running` (normal),
`Relaying` (sessions lent, automation stopped, `conn_->update()` not called) and
`Resuming` (fresh connection, snapshot applied, collision and movement placed).
The `--status-file` reports the mode so a supervisor can tell a driving child
from a stuck one.

### 3. State moves as a snapshot, in both directions

The GUI's `GameConnection` starts empty. The headless's model is frozen at
attach, so neither side can rebuild the other from live packets alone. A
`DriveSnapshot` carries the state:

- **Attach:** the headless captures its current `GameConnection` and sends the
  snapshot; the drive client applies it to a fresh `GameConnection`; live
  packets follow.
- **Detach:** the drive client captures its `GameConnection` and sends the
  snapshot; the headless creates a fresh `GameConnection`, applies it, adopts
  the relay's current sessions, and restarts automation.

The snapshot is versioned, sectioned and bounded. A section is either a set of
server-direction `AppPacket`s (the client already decodes them) or a small typed
value for state that has no server packet (own position and heading, target,
for example). Sections have a length so an older reader can skip a newer one.
This is a state-transfer format, not a memory dump; nothing is written by
casting a struct onto a buffer.

Suggested section priority:

| Priority | Section | Why |
|---|---|---|
| P0 | identity (character, profile id, server and world names), profile, zone (`OP_PlayerProfile`, `OP_NewZone`) | Character sheet, zone, class, level, spells, tasks, AA; the profile id also finds the character's files |
| P0 | spawns (`OP_ZoneSpawns`/`OP_NewSpawn`), own position/heading, player status | Renderer, camera, movement, target range |
| P0 | inventory (`OP_CharInventory` and the bag items), buffs, gems, group | The UI a player checks first; automation's item and heal decisions |
| P1 | target, doors and ground objects, time of day, zone points | Targeting and the scene |
| P1 | raid, guild, tasks, AA, tribute, mercenary | The social windows and Sentinel's raid logic |
| P2 | open merchant/trade/loot/bank sessions, cursor item, quantity dialog | Nice to have; otherwise close them on attach and let the player reopen |
| P2 | chat backlog, hot buttons, socials, spell book | Mostly already in the character's files; chat is cosmetic |

The snapshot must be **complete enough for automation to resume safely**. The
headless reports which sections it received; if a P0 section is missing, it
does not silently resume into a stale inventory. The safe outcomes are to stay
paused, to refuse the detach, or to camp L with a clear message. The first
version should choose "stay paused and say what is missing".

A first implementation can use a **fresh zone-in burst** as the snapshot: the
packets a server sends when a character enters a zone already contain most of
P0. Several builders exist for tests and the mock server
(`encode_char_inventory`, `encode_spawn_doors`, and the mock server's zone-in
builders). They should be moved to a production `net/` helper and tested
against the decoders. This is the same idea as a snapshot, expressed in the
packets the client already understands.

### The client-authoritative state problem

A viewer fed only the server's packets will be wrong about some of the most
visible things. The server does not echo the client's own position, its target
changes, or item moves that the client applies locally. In a normal client
those are client-authoritative. In an **observe** lease, the headless is the
client that owns them, so the viewer needs them from the headless, not from the
server:

- **Position and heading:** the headless sends `OP_ClientUpdate`; the viewer
  can apply that packet to its own player spawn, or the headless can send a
  small `ObserverState` at about 10 Hz (position, heading, velocity, stand
  state) and the viewer extrapolates between samples. Without this L appears to
  stand still while it walks.
- **Target, inventory, cursor and other local-only changes:** the headless can
  send a P0 snapshot on attach and then a periodic snapshot or a delta. A
  full P0 snapshot once a second is simple and probably cheap enough for one
  viewer; a delta is the scalable version.

In a **control** lease this problem does not exist: the GUI is the client that
owns the local state, and the headless is frozen.

This is why observe is not just "copy incoming packets". D2 should start with a
periodic P0 snapshot (or a small delta) plus a position stream, and only then
claim that the viewer shows the character correctly.

## Attach and detach

The sequence below is a **control** lease. An observe lease shares the acquire
and snapshot steps but skips the pause, the outgoing relay and the release
snapshot; the headless's automation and model stay exactly as they were.

```
GUI (char A)                     headless (char L)                  server
    |                                   |                              |
    |-- list/acquire (drive pipe) ----->|                              |
    |                                   | pause automation, movement   |
    |                                   | stop conn_->update()         |
    |<-- snapshot + lease --------------|                              |
    |  create GameConnection(passenger) |                              |
    |  apply snapshot                   |                              |
    |                                   |                              |
    |<============== live AppPackets ================================>|
    |  UI, input, render                |  relay only                  |
    |                                   |                              |
    |-- release + snapshot ------------>|                              |
    |                                   | fresh GameConnection         |
    |                                   | apply snapshot, adopt session|
    |                                   | resume automation            |
```

In a control lease the headless is frozen for the whole drive. It does not
process incoming packets and it does not send anything of its own, so there is
exactly one game model — the drive client's. This is the key invariant. It also
means an accidental extra position update, checksum or zone entry cannot come
from the headless while the GUI is driving. In an observe lease the opposite
holds: the headless is the only model that acts, and the viewer is a passenger.

The two modes must not be mixed. A viewer that starts sending packets in an
observe lease is a bug; a control client that lets the headless act is a bug.

## Discovery, leases and security

The GUI needs to know which headless characters exist.

- **Crew supervisor:** `hailstone_headless --crew crew.ini` already knows every
  member, its label, account and child process. It is the natural drive hub:
  one authenticated pipe, a roster, and a request to lease a member. The
  supervisor forwards the request to the child that owns the session.
- **Standalone host:** each process can write a private registry entry under
  `run/` (pid, label, character, world, zone, endpoint, started-at) and listen
  on its own pipe. The GUI scans the folder and prunes stale entries. This is
  more moving parts; the supervisor is the better first target.

The drive pipe is local and current-user-only:

- D0 (implemented and tested): a loopback TCP stream on `127.0.0.1` with a
  256-bit token. The server binds loopback only, so it is not reachable from
  another machine; there is no OS peer-credential check yet, so another local
  user who knew the ephemeral port and the token could connect.
- Production hardening (not implemented): a Windows named pipe with a
  current-user DACL and `GetNamedPipeClientProcessId`, and a Unix domain socket
  in a 0700 folder with `SO_PEERCRED`/`getpeereid`, behind the same API.
- A 256-bit random token, compared in constant time, plus the OS peer check.
- No endpoint, token or password in a command line or a log. The GUI gets the
  endpoint and token from the supervisor over an already authenticated channel,
  or from a private file with owner-only permissions.
- The relay only connects to the zone server named by the world server; it does
  not accept an arbitrary host from the client.
- The relay refuses a second control lease while one is active. Observers may be
  a later feature; the first version has one client and one mode.

A lease has an id, an expiry and a heartbeat. The GUI sends a heartbeat every
few seconds; the headless releases the lease if it stops. On release, crash,
timeout or a closed pipe, the headless returns to its own model. A clean
release applies the GUI's snapshot. An unclean release leaves the headless with
a frozen model and no snapshot; the safe first behaviour is to **hold the
session and wait for a reattach** for a short grace period rather than resume
automation on stale state. After the grace period it can camp L with a clear
log line, or keep holding and report `stuck` to the supervisor. That choice
belongs in the milestone, not in a guess hidden in the code.

## The drive window

The first delivery is a **separate drive client process**, started by char A's
GUI:

```sh
hailstone --drive <endpoint> [--drive-token-file FILE] [--data-dir DIR]
```

It is the same binary and the same `ClientApp`, with a new start mode:

1. Skip the login screen, `LoginClient` and the updater.
2. Create the drive pipe client, acquire a control lease.
3. Create `conn_` in passenger mode with `MirrorSession`.
4. Apply the snapshot and let `update_connection` load the zone as it always
   does (`take_zone_to_load`).
5. Show the normal in-game UI and renderer, with a small "Driving <name>"
   title and a Release button.
6. On close, Release, `/camp`, `/quit` or a lost pipe, send the snapshot, end
   the lease, and exit (or return to A's window).

Char A's client keeps its own session and window. The drive client is a child
process, so A's connection, renderers and UI are untouched; the cost is a
second Vulkan device and a second zone load. A warm hidden drive client (one
idle process, shown and attached on demand) makes repeated drives much faster
and is a good follow-up, not a first requirement.

Putting the drive view in **A's window** means moving A's per-connection state
into a `CharacterView` and adding a second view to `ClientApp`: `conn_`, zone,
zone/spawn/actor renderers, `WorldObjects`, `SpellUi`, `ItemUi`, `StatsUi`,
`GroupUi`, `SocialUi`, `ActionsUi`, `ProgressUi`, `MarketUi`, chat, camera,
input and the Lua UI's per-character layout. The shell (window, `VkContext`,
workers, audio, config, `ImGuiLayer`) stays. A's connection can stay alive in a
parked view while L is shown; its renderers can be released and built again on
return. This is a large, risky refactor and should be a separate decision after
the drive client works.

## Milestones

### D0 — Transport spike (go/no-go) — passed in process and between processes

Prove the relay and mirror without a snapshot.

- `tests/mock_drive_spike_test.cpp` (14 assertions) starts a mock server, a
  `GameConnection` behind a mirror `Session`, an in-process relay and the real
  `Session`s. The mirror client does the world login, character list, zone-in,
  a position update and a `/say`; the server asks it to zone and the relay
  makes a second real zone session.
- `tests/local_pipe_test.cpp` (14 assertions) proves the local authenticated
  stream itself: a same-process round trip and a child process that authenticates
  with the token and echoes a small and a 256 KiB message.
- `tests/mock_drive_pipe_test.cpp` (15 assertions) runs the same gameplay as the
  in-process spike between two processes: the parent runs the mock server and
  the `GameConnection`, a child runs the relay and owns the real `Session`s, and
  the two exchange the relay protocol over the local stream.

Result: the design works. The GUI-side `GameConnection` saw the world, the
character list, the zone-in, the NPCs and the inventory; its `OP_ClientUpdate`
reached the server; its `/say` reached the server; and the server-requested zone
change produced a second real zone session and the new zone's spawns. The
headless side never handed over a socket, key or sequence number: the child
process kept the real sessions, and only application packets crossed the pipe.

D0's pipe is a **loopback TCP stream with a 256-bit token**, not a named pipe or
a Unix domain socket yet. The Windows named-pipe backend was written first and
could not be exercised in this development sandbox (a same-process pipe test
was refused, so the sandbox blocks named pipes); the TCP transport is the tested
one and keeps the same message API. Production hardening still needs:

- a Windows named pipe with a current-user DACL, and a Unix domain socket with
  peer credentials, behind the same `LocalPipe` API;
- a cryptographic random source for the token (`std::random_device` is used
  today), and OS peer credentials in addition to the token;
- the full relay's lifecycle under a real headless supervisor (no child test
  process), which is D3/D4 work.

None of that changes the `GameConnection`/`MirrorSession` seam D0 proved.

### D1 — Drive snapshot — passed in process

`net/drive_snapshot.h` defines `DriveSnapshot`, its versioned byte encoding and
its bound. `GameConnection::capture_snapshot()` records the zone stream in
`ConnectionOptions::snapshot_journal` (off by default, 64 MiB bound) and adds the
local state the server does not echo: the character name, the current
possessions as an `OP_CharInventory`, position, heading, velocity, moving and
target. `GameConnection::apply_snapshot()` replays the packets into a fresh
connection with outgoing sends suppressed, then applies the local state and
leaves the connection in `InZone`.

`tests/drive_snapshot_test.cpp` (37 assertions) proves capture → apply → compare
for a character in `qeynos2`: profile and character name, zone, spawns,
inventory, HP, target and position; then the encoded snapshot decodes and
applies again from the wire. Empty and half-truncated encodings are refused.

Known D1 limits, to carry into D2/D3:

- the zone journal grows with the session; the 64 MiB bound sets `truncated`
  and the caller must not trust a truncated snapshot;
- replaying old zone packets also replays old timers; buff/cooldown timers are
  not yet shortened to their remaining time;
- the inventory patch covers possessions (including the cursor) but not the
  bank, shared bank or an open merchant/trade/loot session;
- chat backlog and plugin state are not in the snapshot.

The test does not have a real game folder or vectors, so the spell names and
skill caps are the empty fixtures; the packet replay path is the real one.

### D2 — Observe in character A's window — data path passed; swap built, not run live

The user's target is character A's existing `hailstone.exe` window, not a
second client. The separate `hailstone --drive-file` viewer is a diagnostic
tool for the same stream.

- Headless: `hailstone_headless --drive-observe --drive-file F` starts
  `app::DriveObserveServer`, writes its endpoint and token to the new private
  file F, sends the snapshot to the first viewer, forwards every zone packet
  from the connection's `zone_packet_tap`, and sends a 100 ms local-state
  message (position, heading, velocity, moving, target, possessions). A crew
  sets `observe=1` in `[crew]`; the supervisor passes `--drive-observe` and
  `drive-<label>.txt` under `drive_dir` (default `<data dir>/run`) to each
  child, removes the file before a relaunch and when the child exits.
- Viewer data path: `app::DriveClient` reads F, connects, takes the snapshot,
  and feeds a normal `GameConnection` (`start_observe` / `apply_local_state`).
  A `State` message's inventory packet is applied as `OP_CharInventory`; live
  zone packets go through the ordinary `handle_zone` path. Every `send` from
  the viewer's mirror session is dropped, so an observe viewer cannot change
  the character.
- Same window: A types `/crew`; the Crew window lists `drive-*.txt` in
  `<data dir>/run`. **View** calls `switch_to_observe`: A's `GameConnection` is
  moved to `parked_conn_` and keeps updating, A's `game_view_`, character
  profile, UI directory and layout are saved, and A's zone is unloaded. L's
  snapshot is installed in the same `ClientApp`; `update_connection` loads L's
  zone, renderers, UI and audio. **View** on another member while L is shown
  connects the new member first and then replaces only L's view; A stays parked
  and its zone is not reloaded between headless characters. **Release** unloads
  the observed character, restores A's connection/view, clears the layout so
  A's UI reloads, and loads A's current zone again. No process is started or
  logged in.
- Plugins and MQ scripts are not autoloaded in the viewer path
  (`aenv.autoload` / `script_autoload` are empty); the headless keeps its own.

Tests: `tests/drive_observe_test.cpp` (12 assertions) proves the headless
server → pipe → viewer data path, including movement and a live chat packet;
`tests/drive_client_test.cpp` (13 assertions) does the same through the
production `DriveClient`. The GUI builds, but this development environment has
no display/GPU, so the swap itself has not been run.

Still open for D2:

- run A with `/crew` on a real display: view L, move L, open its windows, and
  release back to A; check that A's zone/UI reload and its session was kept;
- A's scene and UI are unloaded while L is shown, so A's open windows/camera
  and its chat are saved only to the extent `game_view_`/the character files
  hold them; `spell_ui_`, `item_ui_` and the other C++ panels may keep state
  from L when A returns. A `CharacterView` refactor would keep both live;
- A's `parked_conn_` keeps processing packets and its plugin hooks are still
  attached; whether A's scripts should pause while parked is open;
- the same window uses A's game folder: A and L must be on the same server's
  files (the usual crew case), because `ClientApp` has one `GameDir`;
- no live headless run yet (D5).

### D3 — Control lease

- Freeze the headless, lend the sessions, relay packets both ways, apply the
  snapshot, and hand a snapshot back on release.
- Intercept camp/quit/logout and close; add the Release button and status.
- Stop and restart automation around the drive; do not re-run login or zone-in
  hooks on resume.
- Place the movement body and collision from the returned snapshot.

Exit criteria: on the mock server, a drive client shows the character, moves,
casts, opens the inventory, zones, releases and exits; the headless resumes
with the same P0 state. Then repeat live.

### D4 — Roster, leases and failure handling

- Supervisor roster and per-child control pipes, or the standalone registry.
- One control lease, heartbeat, timeout, clean and unclean release.
- Crash matrix: GUI crash, headless crash, server link death, double attach,
  stale registry, version mismatch, pipe stall, snapshot failure.
- Status file and logs: `state=driving`, driver, lease, snapshot bytes,
  attach/detach time.

Exit criteria: a killed drive client cannot leave L silently automated on stale
state; a second GUI cannot steal a lease; a headless crash is reported in A's
GUI and does not hang it.

### D5 — Live acceptance

- One test server, char A in the GUI and L headless.
- Attach L, check character sheet, inventory, buffs, group, chat, target.
- Move, fight, loot, zone, use a merchant, return to the headless.
- Run for long enough to catch a server keepalive, a zone change and a
  reconnect after a link death.
- Measure attach time, snapshot size, IPC rate and frame impact.
- Repeat with the drive client on a second monitor and with A's window
  unfocused.

### D6 — Same-window view (optional, separate decision)

Refactor `ClientApp` into a shell plus `CharacterView`s; park A while L is
shown. Estimate separately after D2–D5; do not make D1–D5 depend on it.

### D7 — All-headless desktop (optional direction)

If A is also run headless, the GUI becomes a viewer/controller for whichever
session it attaches to, and "switching characters" needs no parked A and no
second connection. This is a coherent long-term architecture, but it changes
what the graphical client is; it is not required for the first drive feature.

## Tests

- **Unit:** IPC framing and authentication; lease state machine; snapshot
  section reader (bounds, version, unknown sections); packet builders against
  their decoders; the packet relay's session mapping.
- **Integration (in-process mock server):** attach, snapshot, move, cast, loot,
  zone, detach, resume; compare both models' P0 state before and after; a
  second attach while leased is refused; a closed pipe releases.
- **Two-process integration:** real pipe, real `Session` to the mock server,
  kill the drive client mid-drive, check the headless's recovery path.
- **Live:** a stock EQEmu test server, char A and headless L; the D5 list.
- **Fuzz:** snapshot reader and IPC frame parser in the fuzz lists.
- **Cross-platform:** Windows named pipe, Linux Unix socket; macOS builds and
  tests. The transport is in `src/platform/`, so the MSVC-under-Wine run
  (docs/ci.md) applies.

## Risks and open questions

| Risk | Why it matters | Mitigation |
|---|---|---|
| Snapshot misses a field automation uses | L resumes with a stale item, target or buff and acts wrongly | P0 completeness check; refuse to resume; live test before enabling automation resume |
| Two models diverge during a drive | The headless resumes from a model that saw different packets | Freeze the headless model; only the GUI handles packets; replace it from the snapshot on return |
| A duplicate packet from the headless | Two position updates or two zone entries confuse the server | Headless does not call `GameConnection::update()` while relayed; it only pumps sessions |
| Zone change through the relay | A new real session must be created at the right moment | The relay creates it on the mirror's `connect`; live-test a zone line and a summon |
| Plugin/MQ state on return | Sentinel and scripts may cache state or pointers | Stop and restart automation around the drive; document that in-memory automation state resets; re-run no login/zone-in hooks |
| Local-only actions | Item moves, target and position are not echoed by the server | The snapshot carries them; the headless does not try to infer them from packets |
| Lease crash or pipe stall | L could be left idle or automated with stale state | Heartbeat, grace period, hold-not-resume, status file; camp only as the last resort |
| Security of the local channel | A local process could drive or spy on a character | Current-user pipe/socket, peer check, token, no network, zone-address allowlist |
| Same-window refactor | It touches most of `ClientApp` | Keep it out of D1–D5; decide after the drive client is proven |
| Version skew | GUI and headless from different builds may not agree on the snapshot | Version handshake; refuse with a clear message |

Open questions to settle with a spike or a live run:

- Does a fresh `GameConnection` accept a packet-built snapshot with no session
  and reach a usable `InZone` state? (The test hooks say the packet path works;
  the state flags around it need checking.)
- Does a relay-created zone session receive `OP_ZoneEntry` from the GUI exactly
  as a normal client's zone connection does, including the spell checksums and
  the world's account binding?
- Which state truly must be in P0 before automation can resume, and which can be
  reconstructed from the character's files?
- How large is a real P0 snapshot for a level-100 character with a full
  inventory, and how long does capture/apply take?
- Does the server tolerate the app-packet gap while the snapshot is captured
  and sent? The session keeps acking; the application packets queue. The
  existing 64 MiB received queue is the bound.
- What should `/camp` mean in the drive client: refuse, release, or camp L?

## Deliberately out of scope for the first version

- Moving a live socket or `Session` between processes.
- Remote play over a network; the drive pipe is local only.
- Multiple simultaneous drivers of one character.
- Running L's automation while it is driven.
- Full plugin windows in the drive client (the headless owns the plugins).
- Seamless zero-load switching; a zone load is expected on attach.

## Recommendation

D0, D1 and D2's data path have passed; A's same-window swap is built. The next
steps are the **live `/crew` check** on a display and **D3 (control lease)**
for "briefly play L". The pipe hardening (named pipe/Unix socket, peer
credentials, CSPRNG) belongs with D3's lease/security work. After that, decide whether the same-window refactor (D6) is worth its cost.
If A is eventually run headless too (D7), the graphical client becomes a viewer
for any session and the separate drive client can disappear; that is the clean
end state, but it is a different product decision.
