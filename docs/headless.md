# The headless client

`hailstone_headless` runs one character with its plugins and MacroQuest Lua
scripts and no window, Vulkan device, audio device or UI pack. It is a second
host for the same layers the graphical client uses (`src/app/headless_app.cpp`):
`net::LoginClient` and `net::GameConnection` for the world, `app::Automation`
for the compiled plugins (`src/plugin/`), the MQ script host (`src/mq/`) and
their shared hook bus. Nothing about the protocol or the automation differs;
only the frame's rendering half is missing, and `Automation::imgui()` is never
called, so plugin and script windows simply never draw.

```sh
hailstone_headless --host HOST[:PORT] --user U --password P --character NAME \
    [--world NAME|ID] [--game-dir DIR] [--data-dir DIR] [--exec LINE]... [--exec-zone LINE]... \
    [--seconds N] [--status-file FILE] [--no-collision]
hailstone_headless --host HOST[:PORT] --user U --password P --list-servers
hailstone_headless --server NAME|ID --account USER [--character NAME] [--world NAME|ID]
hailstone_headless --crew crew.ini
```

- `--game-dir` (or `EQ_GAME_DIR`) is still required: the connection reads the
  spell and string tables from the game folder and answers the checksums the
  world asks for. The folder is read-only, as everywhere in Hailstone.
- `--world NAME|ID` picks the server to play, as the graphical client's
  `--world` does: its id, its exact name, or a part of the name only one
  server carries (a part of two names is refused, with the list printed).
  Without it the first server on the list is played — on a public login
  server, name yours. `--list-servers` prints the list (id and name) and
  exits, for finding the name. It then enters `--character` and runs until
  SIGINT/SIGTERM, a camp, or the link's end. `--seconds N` camps after
  N seconds in the first zone.
- `--exec` lines run once after the first zone-in, `--exec-zone` lines at
  every zone-in, both through the same command path a typed line takes. The
  leading `/` of an --exec line is optional (a shell may eat it, and the line
  must not turn into a say). For
  the HSMono plugin (the C# Sentinel host, `plugins/HSMono` in the data
  folder): `--exec "/mono load Sentinel"`. HSMono restarts Sentinel itself
  at later zone-ins; its windows do not draw headless and it says so in the
  log, while its pulses, commands and broker traffic run as usual.
- `--status-file` is rewritten once a second (`state=`, `zone=`,
  `character=`) for a supervisor to watch; each character's log is
  `logs/headless-<character>.log` in the data folder, so a crew's logs
  do not overwrite each other.
- `--data-dir DIR` (or the `HAILSTONE_DATA_DIR` environment variable) shares
  another Hailstone folder — normally the graphical client's — as the data
  folder: its `plugins/`, per-character plugin configs and `servers/`
  profiles. A crew automation's own settings live there (HSMono's
  `sentinel.json`, which starts Sentinel's broker comms, and the character
  configs under it), so a headless run meant to join a crew should point at
  the same folder its graphical clients use; on its own worktree folder it
  finds none of them and the automation runs on shipped defaults.
- One process runs one character. Run several for a crew, as with the
  graphical client's box sets.

## Assets

A zone's files are loaded only as a collision mesh (`game::CollisionMesh`:
floor, line of sight and zone-line regions), so `Spawn.LineOfSight` and the
spawns' grounding work without a scene. No zone renderer, actor models,
textures, sky, flora or particles are loaded. `--no-collision` loads no zone
geometry at all: line of sight then reports "unknown", as it does in a zone
whose files cannot be read (the run continues, with a warning in the log).

## Movement

The player's own movement is the graphical client's: the full mechanics
Character (sheet from profile, inventory, buffs and owned AA, as the GUI's
StatsUi builds it) is built each frame, so its TotalEffect SPA totals —
AA and worn-item effects included — drive the movement. `Navigator`
(`src/app/navigator.h`) steers the movement body
(`src/app/player_movement.h`) once per game step over the zone's collision
mesh, exactly as a drawn frame drives it there, so `/nav`, `/moveto`,
`/stick`, `/follow` and zone lines all walk and zone. The nav mesh is built
from the collision mesh at the zone-in and cached under
`cache/navmesh/<profile>/`, as in the graphical client. Doors have no models
headless (no `WorldObjects`), so a walk passes through a closed door's
doorway on the mesh and Navigator's clicks for a door in the way do not
trigger; a direct door click still goes out when Navigator asks for one.
Swimming pitch and vehicle/boat handling follow the movement body's own
rules with no camera behind them. The server asking the client to move
(`OP_RequestClientZoneChange`: a summon, `#goto`, a gate) goes through the
same DoTeleportB the zone lines use, so a summon inside the zone moves the
character there instead of zoning.

## Crew mode

One process per character does not scale to a crew by hand, so
`hailstone_headless --crew crew.ini` supervises them: one child per
member, staggered launches, and a child whose link died (exit 3) is
relaunched after 30 s. A clean camp (exit 0) or a failure (exit 1) stays
down. Stopping the supervisor stops the children (gently, then forcibly
after 5 s on Windows they end outright). Passwords are never in the file
or on a command line: every member logs in by a saved account
(`--server`/`--account` below), and before any child starts the
supervisor proves every member's saved password is in the keyring —
a crew that cannot all log in starts none.

```ini
[crew]
data_dir=C:/Hailstone
game_dir=C:/EQ/EZ Server
stagger=10
autoload=HSMono
chat_log=0

[Okhealz]
server=ez-server
account=crew1
exec=/mono load Sentinel

[Tankguy]
server=EZ Server
account=crew2
character=Tankguy
seconds=0
```

Each child also runs `--quiet`: its whole log goes to its per-character
log file and nothing to the supervisor's console, which shows only the
supervisor's own started/ended/relaunch lines.

A member names a saved `server` (its id or name) and `account`; its
`character` and the account's saved `world` are the defaults from the
profile, so most members need only those three lines plus their `exec`.
Member keys: `character`, `world`, `exec`, `exec_zone`, `game_dir`,
`seconds` (camp after this long; 0 is until stopped), `stagger`
(seconds before this member launches, over the shared one) and `observe`
(1/0, over the shared one). Shared
keys in `[crew]`: `data_dir`, `game_dir`, `stagger` (default 10 s
between launches, so 23 logins do not land at once), `autoload`
(the plugins each member loads — a crew driving Sentinel wants only
its host, e.g. `autoload=HSMono`, so no member also opens a second
command bridge or broker client of its own) and `chat_log` (0, the
crew default: chat lines are omitted from each worker's diagnostic log.
Set `chat_log=1` to write each chat event once. For a standalone worker,
`--no-chat-log` suppresses diagnostic chat. In both cases plugins still
receive chat, and the in-game `/log` setting controls its own file).
`observe=1` in `[crew]` gives every member `--drive-observe` and a
`drive-<label>.txt` file, so the graphical client's `/crew` window can
show any of them; `drive_dir` chooses where those files go (empty:
`<data dir>/run`). A member's file is removed when its child exits and
before a relaunch, so a dead endpoint is not left in `/crew`.

Single characters get the same saved login without a crew file:
`hailstone_headless --server ez-server --account crew1` reads the
login host from the profile, the password from the system keyring,
the server's own `assets/` as the overlay and the character's files
under its profile, exactly as the graphical client's `--account`.
Log in once in the graphical client with **Save password** ticked for
each account first; a missing saved password is reported, never asked
for on the command line.

## Viewing and interacting with a headless character

Character A, already running in the graphical client, can show a headless
character's UI in the same window. Give the single-character headless run
`--drive-observe` and a new private file in the client's `run` folder:

```sh
hailstone_headless --server ez-server --account crew1 --drive-observe \
    --drive-file C:/Hailstone/run/drive-L.txt
```

The file holds the local endpoint and a token, the character, the profile and
the game folder; it is created owner-only and must not exist yet. The headless
keeps playing, and the token admits one viewer at a time.

In A's window, type `/crew`. The Crew window lists the `drive-*.txt` files in
`<data dir>/run` with their character names; **View** switches the window to
that character. A is parked with its connection alive in the background.
The window loads L's snapshot and UI layout. **View** on another member swaps
the observed character while A stays parked. **Release** restores A. A switch
within the same server profile, zone, and instance can keep the loaded scene
and model cache. A different zone or instance takes the full load path. The
window uses a shared game folder and server asset files.

**View** is read-only. Add `--drive-interact` alongside `--drive-observe`
for a standalone worker, or use `interact=1` with `observe=1` in the crew
file. A member can override the crew's interaction setting. The Crew window
then offers **Interact** for that worker. You can click possessions and bag
slots, split stacks, use item augment sockets, and answer the worker's
confirmation dialogs. The worker checks the zone, inventory revision, or
prompt identity before applying an action. The viewer refreshes from the
worker's inventory and dialog state.

The worker keeps its own plugins and scripts running. The graphical character's
plugins and scripts also keep receiving pulses and reading its original
connection while parked. Their windows and overlays return with **Release**.
Controls tied to the graphical scene, such as navigation and keypresses,
are unavailable to the parked character. The viewer follows
live server packets and the worker's position and state about ten times a
second. Movement, chat, casting, bank and trade actions, and full character
control are outside this mode. The standalone `hailstone --drive-file F`
diagnostic viewer stays read-only.

### Control lease (headless side in)

`--drive-control` (or `control=1` in a crew) serves the same authenticated pipe for a **control lease** instead of
an observe one: the viewer plays the character rather than watching it. The headless writes a `control=1` drive file
at the first zone-in; when a viewer connects it sends its snapshot, hands the live sessions to the relay and stops
updating its own connection and automation — exactly one model acts. The viewer drives a normal `GameConnection`
over the pipe, so movement, chat, inventory and NPC actions go out for real. On release the viewer sends its whole
state back, the headless builds a fresh connection on the same sessions, applies it and carries on. An unclean
viewer exit (a crash, a killed process) stops the headless rather than resuming automation on a state it cannot
vouch for.

The relay and its mock-server test are in (`src/app/drive_control.*`, `tests/drive_control_test.cpp`). The
graphical client's `/crew` window offers two ways to drive a `control=1` file:

- **Control** shows the lease in this window. **Release** (or switching away) sends the state back. This window's
  character is parked, as with View: its scene is unloaded until the `CharacterView` refactor.
- **New window** starts `hailstone --drive-file <that file>` as a window of its own (through the box set's
  `ChildProcesses`). This window's character is untouched and keeps playing; the drive window is a normal
  `ClientApp` on the lease and hands it back on `/quit`, a dropped link or close, so the headless resumes.

## Resurrection and item clicks

Sentinel can use `${Me.RezPending}` to find an actionable resurrection
offer after respawning at bind or while hovering. `/rez` accepts the offer
or selects the available resurrection destination.

Item click reads use the equipment's own click effect first, followed by
the first clickable augment in socket order. Use the equipment's name or
slot to activate a socketed effect.

## Rules and consent

Server rules (`hailstone.cfg`, docs/plugins.md) apply exactly as in the
graphical client, through `Automation`'s policy sync. A compiled plugin in
the server's own folder needs the player's recorded yes for its bytes: there
is no window to ask with, so headless never answers the question itself. It
logs the pending consent once and the plugin stays unloaded; allow it from
the graphical client on that server first. A player's own plugins (such as
HSMono in `plugins/`) need no consent and load by the usual
`plugins.autoload` rules — headless loads every allowed one (`"*"`).

## Not wired yet (scaffold)

These report "unavailable" in the log instead of silently pretending:

- Door models in movement: no `WorldObjects` headless, so a closed door
  does not stop a walk and Navigator's door-in-the-way clicks do not
  trigger (see "Movement").
- Automatic skill rules from the graphical Ability Book, `/keypress` and `/face`.
  Headless plugins can look up and activate skills, disciplines and AAs through
  the same ability helpers as the graphical client.
- A crew member ends for good when it camps or fails: only a link
  death relaunches, and only in crew mode.

The open items are tracked in docs/todo.md ("Headless client") and
docs/unknowns.md.
