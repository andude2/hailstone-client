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
  `character=`) for a supervisor to watch; the log is
  `logs/headless.log` in the data folder.
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

The player's own movement is the graphical client's: `Navigator`
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
rules with no camera behind them.

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
- The Ability Book accessors the graphical client serves from its UI host
  (`AutomationEnv` ability, autoskill list, `use_ability` and the bulk
  ability read), `/keypress` and `/face`.
- Auto-relogin after a link death: the run ends and the supervisor decides.
- Saved-server selection (`--server` / `--account` and the keyring) as the
  graphical client's login has it: headless takes `--host` and credentials
  only, and the character's files go under the profile the world name makes.

The open items are tracked in docs/todo.md ("Headless client") and
docs/unknowns.md.
