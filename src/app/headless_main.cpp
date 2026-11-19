// hailstone_headless: the headless client (docs/headless.md). Logs one
// character in and runs its plugins and scripts with no window or GPU.
#include <cstdio>
#include <string>
#include <vector>

#include "app/headless_app.h"
#include "core/log.h"
#include "core/wipe.h"

int main(int argc, char** argv) {
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    auto parsed = hailstone::app::parse_headless_args(args);
    if (!parsed.error.empty()) {
        std::fprintf(stderr,
                     "hailstone_headless: %s\n"
                     "usage: hailstone_headless --host H[:P] --user U --password P --character C [--game-dir DIR]\n"
                     "  --world NAME|ID   the server to play: its id, its exact name, or a part of the name\n"
                     "                    only one server carries (default: the first on the list)\n"
                     "  --list-servers    print the login server list (id and name) and exit\n"
                     "  --server S --account A\n"
                     "                    log in as saved account A of saved server S (its password from\n"
                     "                    the system keyring; S by id or name), entering A's character\n"
                     "                    (or --character / --world named here)\n"
                     "  --crew FILE       supervise one child per member of FILE (saved accounts and\n"
                     "                    servers only; docs/headless.md \"Crew mode\")\n"
                     "  --data-dir DIR    share this Hailstone folder's plugins, configs and server\n"
                     "                    profiles (e.g. the graphical client's folder) instead of the\n"
                     "                    binary's own (HAILSTONE_DATA_DIR does the same)\n"
                     "  --exec LINE       command sent once after the first zone-in (repeatable;\n"
                     "                    e.g. --exec \"/mono load Sentinel\" for the HSMono plugin)\n"
                     "  --exec-zone LINE  command sent at every zone-in (repeatable)\n"
                     "  --seconds N       camp after N seconds in the first zone (default: run until stopped)\n"
                     "  --status-file F   rewrite F once a second with the state, zone and character\n"
                     "  --no-collision    do not load the zone's collision mesh\n"
                     "The password shows in the process list; use a test or crew account's.\n",
                     parsed.error.c_str());
        return 2;
    }
    int rc = hailstone::app::run_headless(std::move(parsed.options));
    hailstone::core::wipe(parsed.options.password);
    return rc;
}
