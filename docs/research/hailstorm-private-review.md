# Review: Hailstorm-Private (Project Hailstone 24)

Status: external implementation reviewed read-only at
`C:\Users\andy\source\repos\Hailstorm-Private`, branch `codex/hailstone-unified-review`, on 2026-11-23.
No code was copied; this note records architecture and ideas only. That repository is a fork of this
client (base `61e5135f`) plus a 99-file `hailstone24/` overlay. Its own
`hailstone24/docs/KNOWN_LIMITATIONS.md` says the patch was never compiled or run on the target
workstation (their container had no C++/Vulkan toolchain), so its container test counts are not a build
proof. Their target is Ubuntu with an Intel Arc Pro B70 (32 GiB VRAM) and 48 GiB RAM.

## What they built

- **24 independent `hailstone` processes**: one `ForegroundFull`, 23 `BackgroundHeadless`, supervised
  by `hailstone24_broker` and an interactive `hailstone24_manager`. They explicitly rejected a
  single-process 24-client monolith for the same reasons we keep one process per character: process-wide
  registries, the Vulkan dispatch table, one ImGui context, plugin/crash globals, and fault isolation.
- **`ExecutionController`** (`src/app/execution_state.h`): `Disconnected`, `BackgroundHeadless`,
  `Transitioning`, `ForegroundFull`. Background hides the window, suspends the swapchain and scene
  targets, mutes audio, skips the ImGui/Lua UI build, and runs networking, physics, navigation,
  `Automation`, RGMercs Lua and the IPC bus at `bg_tick_hz` (default **25 Hz**, 10-60). The foreground
  process renders at `max_fps`. Switching is `request_foreground()` -> `Transitioning` -> first
  presented frame (`last_switch_ms`).
- **`frame_pace_for_state`**: in `BackgroundHeadless` the loop cap is `bg_tick_hz`, `draw = false`,
  `skip_ui_build = true`; `display.fixed_step = 1` keeps 60 Hz mechanics by stepping the game clock more
  than once per frame. This is the important part: they lower the *loop/pulse* rate, not the game clock.
- **`SessionBus`** (`src/ipc/session_bus.h`): local AF_UNIX (Linux) with 1 MiB bounded frames,
  0700 directory/0600 socket and same-UID peer checks. Messages: `Hello`, `HelloAck`, `Telemetry`,
  `CommandDispatch`, `CommandAck`, `ActorSend`, `StateControl`, `RosterQuery/Reply`,
  `ObserveQuery/Reply`. Commands are scoped (`Self`, all-except-self `/bca`, all `/bcaa`, zone/group,
  target peer `/rc`, channel) with a sequence number, tracked `Pending/Acked/Partial/Failed/TimedOut`
  state and timeouts. Telemetry is a `SessionSnapshot` with character, class, level, zone, target,
  HP/mana/end, group slot/leader, execution state, combat state, script name/state, RSS, VRAM, ping,
  last switch ms and connection state.
- **Supervisor**: roster slots (account, character, group leader, settings path, `headless`, `bg_tick_hz`,
  `gpu_mode`, startup order), staggered launch, supervision of up to 24 children, restart; a manager UI.
- **Automation stack**: a native C++ combat engine (`src/combat/*`: combat profiles, movement
  controller, spell manager) plus RGMercs Lua/MQ compatibility (ImPlot/ImAnim/Zep shims, a trusted
  `scripts/rgmercs/` loader, typed `RGMercsBridge` defaults). Their `plugins/` tree contains only
  `example`: **their 24-box does not use HSMono/Mono at all**, and therefore runs no per-character
  C# runtime.
- **`shared_file`**: a read-only mapping abstraction (POSIX `mmap`, Windows section handle), cooperative
  file locks, and cache temp-file/publish helpers. Their ARCHITECTURE_DECISION credits OS page-cache
  dedup for zone/PFS assets and `VK_KHR_external_memory_fd` for optional shared VRAM; `gpu_mode=cached`
  is their default.
- They also fixed a zone-settle bug where a backgrounded client waited for rendered `scene`/`ui` frames
  that were suppressed, and added MQ `login.db` import with keyring storage (DPAPI blobs are skipped on
  Linux).

## How it differs from our control lease

Their "switch" changes which **process** is visible: the foreground is the live client, the previous
foreground drops to 25 Hz in the background. There is no state transfer; the GUI is that process's own
window.

Our Control lease takes over the **stream**: the headless freezes, lends its sessions and a snapshot to
a viewer, and the GUI becomes the controlled character while the original GUI character stays engaged.
That is the stronger model for the use case asked for here ("be that headless client while it is
selected", with A still playing), at the cost of a snapshot transfer and a more complex protocol. Their
`Transitioning` state and `last_switch_ms` are still worth borrowing as explicit state and telemetry.

What they get in return: near-zero switch cost, no snapshot, and the background client keeps its full
renderer caches (their RAM cost) while running automation at 25 Hz.

## Lessons for 24 characters, each with its own Mono runtime

Their 24-box has no Mono; ours has HSMono plus Sentinel in every headless process. That is the main
difference to learn from: **24 C# runtimes are a CPU and memory tax their design does not pay**.

1. **Cut the pulse rate, keep the game clock.** We call `Automation::frame()` every headless loop
   iteration, about 60 times a second; 24 clients is ~1440 Sentinel/C# pulses per second. Their 25 Hz is
   ~600. `StepClock` already lets the game rules run at 60 Hz fixed steps across a slower loop
   (`display.fixed_step`), so add a `--tick-hz` / `bg_tick_hz` to `hailstone_headless` (30 Hz is a
   reasonable first default) and measure Sentinel's cast/gem timing at that rate. This is the cheapest
   large win and needs no plugin change.
2. **Measure and tune the Mono cost.** Record RSS and CPU per headless with and without HSMono, and the
   time `Automation::frame()` takes (`last_lua_pulse_us` / `max_lua_pulse_us` in their metrics are the
   right shape). Tune Mono GC settings (nursery size, concurrent GC) per runtime, avoid per-frame
   allocations in Sentinel, and consider a native fast path for the common background actions
   (follow/assist/attack/stick) using the client's existing `Navigator`/`PlayerMovement`, leaving
   Sentinel for the complex decisions. AOT or one shared runtime would be a much larger change, and
   merging processes is the Option C they rejected for the same reasons we keep one process per
   character.
3. **Per-character state and telemetry.** Their `ExecutionMetrics`/`SessionSnapshot` is a good model for
   our heartbeat and Crew window: connection/execution state, zone, HP/mana, script state, plugin count,
   extended targets, pulse time, RSS, last packet age and switch time. It also makes a slow Mono runtime
   visible (a pulse-time warning) instead of a silent raid-wide slowdown.
4. **A command bus with acknowledgements.** Their `/bca`, `/bcaa`, `/rc`, `/dquery` dispatch is separate
   from their window switching: scoped commands with sequence numbers, per-target ACKs, timeouts and
   telemetry. Our lease already carries packets and a reliable release, and the GUI's
   `/crew <name>`/`/crew release` covers takeover; but cross-character actions (follow, assist, pause,
   apply profile, camp) have no ACK path in the lease. Borrow the ACK/timeout design if we add one; keep
   the stream lease for takeover.
5. **Supervisor and roster.** Their broker supervises roster slots with group roles, profiles, startup
   order and restart. Our crew file plus the relaunch-on-3/crash fix covers the essentials; the Crew
   window could grow status columns (running, zone, HP, plugins, xtargets, pulse ms, RSS) and
   Start/Stop/Restart, and the crew file could carry role/profile/leader fields.
6. **Shared read-only mappings.** Their `ReadOnlyMapping` (including a Windows section handle) is worth
   considering for navmesh/zone/cache payloads shared by 24 headless processes. Windows page cache
   already shares file pages, and our headless deliberately loads collision only (no models/textures),
   so this is a smaller win than it is for their renderer-retaining background clients; measure before
   building it.
7. **Zone settle.** Their bug (waiting on suppressed rendered frames) cannot happen in our headless,
   which never renders; check the GUI control viewer's settle path separately.

## What not to take

- Their patch is uncompiled and unbenchmarked on the target workstation; treat their cited test results
  as container-only and their 24-box memory/VRAM numbers as estimates.
- Linux `VK_KHR_external_memory_fd`, DPAPI handling and their ImPlot/ImAnim shims are platform-specific.
- 25 Hz may be too slow for Sentinel's timing (casts, gem locks); measure before defaulting below 30.
- Their server-specific teleport commands (`tp_to_me`/`tp_here`) stay out of the stock-compatible client.
- Their single-process option was rejected; ours stays one process per character.

## Suggested follow-ups

1. `hailstone_headless --tick-hz` (default 30), with `StepClock` keeping 60 Hz mechanics; log the
   effective tick and any dropped steps.
2. Add pulse-time, RSS and last-packet age to the heartbeat, and a 24-box status view in the Crew window.
3. Measure the HSMono/Mono baseline per process and the Sentinel pulse cost across 24 clients; tune GC
   and the per-frame allocation profile from that data.
4. If pulse time is dominated by simple behaviors, prototype a native follow/assist/attack fast path
   driven by the plugin, with Sentinel above it.
5. Optionally add an ACK'd command message to the lease (or a small session bus) for cross-character
   actions.
