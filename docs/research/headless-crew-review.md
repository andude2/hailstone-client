# Headless crew: review and the road to full control

**Status: D3 built, D6 remaining.** This page records what the headless project is today, how the graphical
client swaps to a headless character's view, why the original character loses world state when the view is
released, and the architecture that makes "assume the whole stream" work. It summarises and extends
[Driving a headless character from the graphical client](drive-headless.md), whose D0/D1/D2 milestones and D3
(control lease) are built: `--drive-control` in the headless host, the production relay (`src/app/drive_control.*`)
and the graphical client's `/crew` **Control**. D6 (`CharacterView`, so A stays fully engaged) is the remaining
piece, with its plan below. The headless host itself is in [the headless client](../headless.md).

## What exists today

The headless project is a **second host process** for the client's network and
automation layers, with no rendering half:

- `hailstone_headless` (`src/app/headless_app.cpp`, `src/app/headless_main.cpp`)
  runs one character through the same `net::LoginClient`, `net::GameConnection`
  and `app::Automation` as the graphical client, but loads a zone only as a
  `game::CollisionMesh`. There is no window, Vulkan device, audio device or UI
  pack, and `Automation::imgui()` is never called, so plugin and script windows
  never draw. Movement is the graphical client's own mechanics `Character` +
  `Navigator` + `PlayerMovement`, driven once per game step, so `/nav`,
  `/moveto`, `/stick`, `/follow` and zone lines all walk and zone.
- **Crew mode** (`hailstone_headless --crew crew.ini`) supervises one child
  process per member: staggered launches, a link death relaunches after 30 s, a
  camp or failure stays down, and every saved password is proven before any
  child starts.
- The **drive relay** moves application packets between the two processes over
  a local authenticated pipe (`src/platform/local_pipe.*`, loopback TCP with a
  256-bit token in this build). The headless side is
  `src/app/drive_observe.*`; the graphical side is `src/app/drive_client.*`; the
  state transfer is `src/net/drive_snapshot.*`.

The one rule that must not change: **the network session stays where it is.**
The headless host owns the real `Session`s (handshake, crypto, sequence numbers,
acks, retransmission, keepalives). The graphical client never sees a socket, a
key or a sequence number; it sees application packets through a
`MirrorSession`. That is the D0 design and it is the right one.

## How the same-window swap works today

The current integration is D2: A types `/crew`, the Crew window lists the
`run/drive-*.txt` files, and **View** calls
`ClientApp::switch_to_observe()` (`src/app/client_app.cpp`). In order:

1. Read the endpoint file and connect a `DriveClient`; pull the headless
   character's `DriveSnapshot`. A failed attach leaves the current view alone.
2. **Park A.** A's `GameConnection` moves to `parked_conn_`, A's `game_view_`
   and per-character paths are stashed, and A's zone is unloaded.
3. Point the single shared `Automation` at the parked A
   (`Automation::set_parked_character`), so A's plugins and scripts keep
   pulsing against A; `Automation::imgui()` suppresses their windows and the
   `aenv.*` callbacks that need the scene (`nav`, `keypress`, `face`, hot
   buttons, line of sight) return nothing while parked.
4. Build a fresh `GameConnection` for L behind a `MirrorSession`
   (`ConnectionOptions::session_factory`), `start_observe()`,
   `apply_snapshot()`, and load L's zone into the same `ClientApp`.

**Release** (`release_observe()`) undoes it: L's connection and pipe go, A's
`parked_conn_` moves back into `conn_`, the saved view/paths return, and A's
zone is loaded again from scratch.

**View is read-only. Interact is not control.** `--drive-interact` (or
`interact=1` in a crew) carries *typed* actions only: possessions and bag
clicks, stack splits, augment sockets, confirmation and quantity answers
(`net::DriveAction`, `GameConnection::apply_drive_action`). The worker applies
them to its own connection with stale-zone, inventory-revision and prompt
checks, and keeps its automation running. There is no movement, chat, casting,
bank/trade or NPC interaction from the graphical client.

## Why the original character loses world state on return

The symptom — extended targets empty, plugin connections gone after **Release**
— follows from the design, not from a single bug. The research page itself
lists it under D2's open items ("A's scene and UI are unloaded while L is
shown").

1. **A's zone is unloaded and reloaded.** The connection (`parked_conn_`, and
   with it the raw `xtargets_`, spawns, inventory and buffs) keeps updating in
   `pump_parked_connection()`; the presentation layer does not. `unload_zone()`
   tears down `WorldObjects`, the renderers and the per-zone bindings, and
   `load_zone()` builds them again on return. Anything that lived only in that
   layer is lost.
2. **One `Automation` is shared and re-pointed.** There is a single
   `NativePlugins` + `ScriptHost` + hook bus. While parked its world-geometry
   callbacks are suppressed (`world_wanted()` is false) and its scene-bound
   callbacks are stubbed out. A is *paused*, not *engaged*: exactly the
   "controls tied to the graphical scene are unavailable to the parked
   character" limitation in the headless documentation.
3. **The active connection is replaced by `std::unique_ptr` moves.** `conn_`
   becomes L and then becomes A again, so any component that cached a raw
   `GameConnection*` (a UI panel, a plugin host, a hook wiring) can end up
   pointing at a dead object or at the wrong character. The `Automation::conn()`
   indirection is careful about this; the presentation and plugin world state
   are the places where it leaks out.

In short, the swap was built as **observe + park + full zone reload**. It was
never intended to keep two characters simultaneously live, and it does not.

A test now pins the first part down: the zone stream, extended targets among it, survives both hand-offs
(`tests/drive_control_test.cpp`, "extended targets survive the lease and the hand-back"). So the loss the player
sees is the scene and plugin-window presentation being torn down and rebuilt, not the connection's own data —
which is why D6 is the fix.

## The design that makes it work

Three pieces, in this order.

### 1. A shell plus per-character `CharacterView`s (D6)

Move everything that belongs to one character out of `ClientApp` into a
`CharacterView`: its own `GameConnection`, `WorldObjects`, zone/spawn/actor
renderers, camera and input, the `StatsUi`/`ItemUi`/`SpellUi`/`GroupUi`/
`SocialUi`/`ActionsUi`/`ProgressUi`/`MarketUi` panels, chat, its Lua UI layout,
and its own `Automation` (its own plugins, scripts and hook bus).

The shell keeps only what is genuinely shared: the window, `VkContext`,
workers, audio, config, the `ImGuiLayer` and the `GameDir`.

"Select a character" then means making one view active and the others inactive
— not tearing anything down. A's connection, zone, plugins, extended targets,
broker sockets and script state stay live in the background. **Release is not a
reload, so nothing drops.** This is the change that actually satisfies "the
original client stays fully engaged".

### 2. The D3 control lease (built: make View into Play)

Keep the transport where it is and give the GUI authority instead of
read-only:

- **Attach:** the headless pauses its automation and movement, stops feeding
  its own `GameConnection`, and lends the sessions to the relay. The GUI builds
  a `GameConnection` over the `MirrorSession` and applies the snapshot.
- **During:** application packets flow both ways. The GUI's sends become real
  `Session::send`s on the headless side. Because it is an ordinary
  `GameConnection`, every existing UI window, renderer, `hs.*` call and input
  path works unchanged — movement, chat, casting, bank, trade, NPCs.
- **Detach:** the GUI captures *its* `GameConnection` into a snapshot and sends
  it back; the headless builds a fresh connection, applies it, adopts the
  relay's sessions and resumes automation.

The invariant: **exactly one model acts at a time.** In a control lease the
headless is frozen (it does not call `GameConnection::update()` and sends
nothing of its own); in an observe lease the headless is the only actor and the
viewer is a passenger. The modes must never mix.

### 3. Lease and failure hardening (D4)

One control lease at a time, with a heartbeat and expiry; a crashed or
unclean-released viewer leaves the headless **holding the session for a grace
period rather than resuming on stale state**, then a deliberate outcome
(reattach, camp, or report `stuck`). Intercept `/camp`, `/quit`, close and the
logout paths in the viewer so they release the lease instead of camping L. A
zone change is already carried by the relay: it only creates a session for the
address `OP_ZoneServerInfo` names.

### Keep the transport where it is

Do not move a socket or a `Session` between processes, and do not log the GUI in
as L. Both are explicitly rejected in the design page. The headless host remains
the network owner at every stage.

## Sequencing

1. **D3 control lease — done.** The headless host freezes and lends its sessions, the graphical client drives a
   normal `GameConnection` over the lease, and Release hands the state back. It works on the same observe-style
   park, so A is still unloaded while it drives.
2. **D6 `CharacterView` — next.** It is the largest change, but it is what fixes the state-loss symptom: both
   characters live, A never parked and never reloaded. The two phases are below.
3. **D4 leases/failure matrix**, then **D5 live acceptance** on a test server.
4. **D7, optional end state:** if A is also run headless, the graphical client becomes a viewer/controller for
   whichever session it attaches to, and the separate drive client disappears. This is a product decision, not
   required for the first working version.

## Implementing the `CharacterView` refactor

D6 is the remaining work, and it is a per-character view split done in two safe phases.

**Scope note (from the code).** `can_reuse_observe_scene` already keeps the loaded scene when the two characters
share the profile, zone and instance — the common crew case — so `switch_to_crew` skips `unload_zone()` and both
directions only `rebind_observe_scene()`. The reload, and the scene loss, is the **cross-zone or cross-instance**
switch, plus the plugin windows (not drawn while parked) and plugin world batches (retired and re-recorded). Phase 1
is therefore needed for cross-zone switches; the same-zone path already avoids the reload.

**Phase 1, a scene bundle (no behaviour change).** `unload_zone()` (`src/app/client_app_zone.cpp`) destroys the
zone's scene: `zone_renderer_`, `world_geometry_renderer_`, `flora_renderer_`, `zone_particles_`,
`switch_particles_`, `sky_renderer_`, `actor_renderer_` (with `actor_kept_`), `actor_library_`, `collision_`,
`world_visual_collision_`, `animated_objects_`, `zone_`, the fire lists, `zone_ladders_`, `zone_line_`'s state,
`missiles_`, the preview slots and `ui_host_.zone_exits`/`char_library`. It also tells `world_objects_`, `nav_`,
`audio_` and the weather that the zone is gone; those are shared and stay. Phase 1 adds a `ParkedScene` holding the
per-zone bundle, moves it aside on a crew switch instead of destroying it, and moves it back on release, re-running
only the non-loading part of `load_zone` (no archive reads, no re-fired zone hooks). This removes the reload and the
state churn; it does not yet let both characters render at once.

**Phase 2, `CharacterView`.** Move the per-character members into a `CharacterView`: `conn_`, `game_view_`,
`stats_ui_`, `item_ui_`, `spell_ui_`, the group/social/actions/progress/market panels, chat, the UI directory and
layout, and the phase-1 scene bundle. `ClientApp` keeps the shell: window, `VkContext`, workers, audio, config,
`GameDir`, `ImGuiLayer`. `switch_to_crew` becomes `active_view = &view`; A's view keeps updating (and its plugins
keep their world bindings) while L's is shown, so Release is a pointer swap, not a reload.

What to check: compile and the mock-server `GameConnection` tests; then a live display run — View/Control L, move,
open windows, switch to another member, Release, and check A's extended targets, plugins and camera are exactly as
they were. Every scene resource must be destroyed only with its own view, and `vk_.wait_idle()` must gate a move.

## Review notes

- The `headless` branch tracks build outputs that do not belong in the tree:
  `hailstone_headless.exe`, `zlib1.dll` and the compiled `shaders/*.spv`. They
  are removed from this branch's history by the commit that adds this page, and
  `.gitignore` keeps a later build from adding them again.
- `docs/headless.md` is accurate for the shipped headless host. This page and
  [drive-headless.md](drive-headless.md) are the reference for the crew/drive
  work.
