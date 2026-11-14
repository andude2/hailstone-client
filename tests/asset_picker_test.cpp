// The asset-folder choice at start-up and the picker's recent folders (core/asset_folder_choice.h).
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/asset_folder.h"
#include "core/asset_folder_choice.h"
#include "core/config.h"
#include "core/game_dir.h"

namespace fs = std::filesystem;
using namespace hailstone;

namespace {
struct TempDir {
    fs::path path;
    explicit TempDir(const char* tag) {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / (std::string("hailstone_picker_") + tag + "_" + std::to_string(stamp));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// A validator that passes only `good`, so the tests need no RoF2 files.
AssetFolderValidator only(const fs::path& good) {
    return [good](const fs::path& p) {
        AssetFolderCheck c;
        c.is_directory = true;
        c.ok = same_folder(p, good);
        if (!c.ok) c.missing.push_back("spells_us.txt");
        return c;
    };
}
} // namespace

TEST_CASE("Asset folder: --game-dir / EQ_GAME_DIR first, then assets.path, then the picker") {
    TempDir a("a"), b("b");
    Config config;

    SUBCASE("nothing known: the picker, with no complaint") {
        auto r = resolve_asset_folder({}, config, only(a.path));
        CHECK(r.source == AssetFolderSource::Picker);
        CHECK(r.path.empty());
        CHECK(r.reason.empty());
    }
    SUBCASE("a usable assets.path") {
        config.set("assets.path", a.path.string());
        auto r = resolve_asset_folder({}, config, only(a.path));
        CHECK(r.source == AssetFolderSource::Config);
        CHECK(r.path == a.path);
    }
    SUBCASE("the override wins over assets.path, even when its check fails") {
        config.set("assets.path", a.path.string());
        auto r = resolve_asset_folder(b.path, config, only(a.path));
        CHECK(r.source == AssetFolderSource::Override);
        CHECK(r.path == b.path);
    }
    SUBCASE("an override that is no folder goes to the picker, starting on assets.path") {
        config.set("assets.path", a.path.string());
        auto r = resolve_asset_folder(a.path / "nope", config, only(a.path));
        CHECK(r.source == AssetFolderSource::Picker);
        CHECK(r.path == a.path);
        CHECK(r.reason.find("--game-dir") != std::string::npos);
    }
    SUBCASE("an assets.path that fails the check: the picker, saying what is missing") {
        config.set("assets.path", b.path.string());
        auto r = resolve_asset_folder({}, config, only(a.path));
        CHECK(r.source == AssetFolderSource::Picker);
        CHECK(r.path == b.path);
        CHECK_FALSE(r.reason.empty());
        CHECK(r.check.missing == std::vector<std::string>{"spells_us.txt"});
    }
    SUBCASE("assets.pick shows the picker over a usable folder, until a folder is remembered") {
        config.set("assets.path", a.path.string());
        request_asset_folder_pick(config, true);
        CHECK(asset_folder_pick_requested(config));
        auto r = resolve_asset_folder({}, config, only(a.path));
        CHECK(r.source == AssetFolderSource::Picker);
        CHECK(r.path == a.path);
        CHECK_FALSE(r.reason.empty());
        // An override still comes first.
        CHECK(resolve_asset_folder(b.path, config, only(a.path)).source == AssetFolderSource::Override);
        remember_asset_folder(config, a.path);
        CHECK_FALSE(asset_folder_pick_requested(config));
        CHECK_FALSE(config.get("assets.pick"));
        CHECK(resolve_asset_folder({}, config, only(a.path)).source == AssetFolderSource::Config);
    }
}

TEST_CASE("Asset folder: the recent folders") {
    using V = std::vector<std::string>;
    CHECK(push_recent_folder({}, "/eq") == V{"/eq"});
    CHECK(push_recent_folder({"/a", "/b"}, "/c") == V{"/c", "/a", "/b"});
    // The same folder moves to the front, also when written with a trailing separator or a dot.
    CHECK(push_recent_folder({"/a", "/b", "/c"}, "/b") == V{"/b", "/a", "/c"});
    CHECK(push_recent_folder({"/a", "/b/", "/c"}, "/./b") == V{"/./b", "/a", "/c"});
    CHECK(push_recent_folder({"/a"}, "") == V{"/a"});
    // At most `max`, the oldest dropped.
    V many;
    for (int i = 0; i < 10; ++i) many = push_recent_folder(many, "/f" + std::to_string(i));
    REQUIRE(many.size() == kMaxRecentAssetFolders);
    CHECK(many.front() == "/f9");
    CHECK(many.back() == "/f2");
    CHECK(push_recent_folder({"/a", "/b", "/c"}, "/d", 2) == V{"/d", "/a"});

    CHECK(remove_recent_folder({"/a", "/b", "/c"}, "/b/") == V{"/a", "/c"});
    CHECK(remove_recent_folder({"/a"}, "/x") == V{"/a"});

    CHECK(same_folder("/eq/rof2", "/eq/rof2/"));
    CHECK(same_folder("/eq/x/../rof2", "/eq/rof2"));
    CHECK_FALSE(same_folder("/eq/rof2", "/eq/rof"));
#ifndef _WIN32
    CHECK_FALSE(same_folder("/eq/RoF2", "/eq/rof2")); // case matters off Windows
#endif

    // assets.recent: '|'-separated, empty entries and repeats skipped on reading.
    Config config;
    config.set("assets.recent", "/a||/b|/a/|/c");
    CHECK(recent_asset_folders(config) == V{"/a", "/b", "/c"});
    CHECK(join_recent_folders({"/a", "", "/b|c", "/d"}) == "/a|/d");

    // remember_asset_folder: assets.path, the folder first in assets.recent, both absolute.
    const std::string b = fs::absolute("/b").string(), fresh = fs::absolute("/new").string();
    config.set("assets.recent", "/a|" + b + "|/c");
    remember_asset_folder(config, "/b/");
    CHECK(config.get_string("assets.path") == b);
    CHECK(config.get_string("assets.recent") == b + "|/a|/c");
    remember_asset_folder(config, "/new");
    CHECK(recent_asset_folders(config) == V{fresh, b, "/a", "/c"});
    // A typed relative folder is kept as the absolute one it meant when it was chosen.
    remember_asset_folder(config, fs::path("games") / ".." / "RoF2");
    CHECK(config.get_string("assets.path") == (fs::current_path() / "RoF2").string());
    // A server's game_files is kept the same way (core/server_profile.h).
    CHECK(asset_folder_text("/b/") == b);
    CHECK(asset_folder_text(fs::path("games") / ".." / "RoF2") == (fs::current_path() / "RoF2").string());
}

TEST_CASE("Asset folder: the EverQuest folder Hailstone sits in is the default when nothing saved works") {
    TempDir tmp("beside");
    const fs::path eq = tmp.path / "EverQuest";
    const fs::path hs = eq / "hailstone";
    fs::create_directories(hs);
    Config config;
    // A first start: found, and not saved.
    auto r = resolve_asset_folder({}, config, only(eq), hs, eq);
    CHECK(r.source == AssetFolderSource::Found);
    CHECK(same_folder(r.path, eq));
    CHECK(r.reason.empty());
    CHECK(config.get_string("assets.path", "").empty());
    // Not an installation: the picker, as before.
    CHECK(resolve_asset_folder({}, config, only(tmp.path / "other"), hs, eq).source == AssetFolderSource::Picker);
    // No `beside` (a build without HAILSTONE_ALLOW_IN_EQ_FOLDER): the picker.
    CHECK(resolve_asset_folder({}, config, only(eq), hs).source == AssetFolderSource::Picker);
    // Hailstone's folder is the EverQuest folder itself: refused even as a default.
    CHECK(resolve_asset_folder({}, config, only(eq), eq, eq).source == AssetFolderSource::Picker);
    // A saved folder that works wins; one that is gone falls back to the found one.
    const fs::path other = tmp.path / "RoF2";
    fs::create_directories(other);
    config.set("assets.path", other.string());
    auto both = [&](const fs::path& p) {
        AssetFolderCheck c;
        c.is_directory = true;
        c.ok = same_folder(p, eq) || same_folder(p, other);
        return c;
    };
    CHECK(resolve_asset_folder({}, config, both, hs, eq).source == AssetFolderSource::Config);
    config.set("assets.path", (tmp.path / "moved").string());
    CHECK(resolve_asset_folder({}, config, both, hs, eq).source == AssetFolderSource::Found);
    // "Change asset folder" asked for the picker: it shows.
    request_asset_folder_pick(config, true);
    CHECK(resolve_asset_folder({}, config, both, hs, eq).source == AssetFolderSource::Picker);
    request_asset_folder_pick(config, false);
    // An override that is no folder is not replaced by the found one: the picker says why.
    auto bad = resolve_asset_folder(tmp.path / "nope", config, both, hs, eq);
    CHECK(bad.source == AssetFolderSource::Picker);
    CHECK_FALSE(bad.reason.empty());
}

TEST_CASE("Asset folder: never one Hailstone writes into") {
    TempDir tmp("conflict");
    const fs::path eq = tmp.path / "RoF2";
    fs::create_directories(eq / "hailstone");
    for (bool allow : {false, true}) {
        CAPTURE(allow);
        // The data folder is the asset folder itself: refused, saying why, whatever the build.
        CHECK_FALSE(data_folder_conflict(eq, eq, allow).empty());
        CHECK_FALSE(data_folder_conflict(eq / "", eq / ".", allow).empty());
        // Inside it: refused, unless the build allows a subfolder of Hailstone's own (HAILSTONE_ALLOW_IN_EQ_FOLDER).
        CHECK(data_folder_conflict(eq, eq / "hailstone", allow).empty() == allow);
        CHECK(data_folder_conflict(eq / "", eq / "hailstone" / ".", allow).empty() == allow);
        CHECK(data_folder_conflict(eq, eq / "HailStone", allow).empty() == allow); // the name's case is ignored
        // Anywhere else inside it, at any depth, folders the game reads among them: refused whatever the build.
        for (const fs::path& inside : {eq / "maps", eq / "maps" / "hailstone", eq / "uifiles" / "x", eq / "Resources",
                                       eq / "hailstone" / "sub", eq / "hailstone2", eq / "my hailstone"}) {
            CAPTURE(inside);
            CHECK_FALSE(data_folder_conflict(eq, inside, allow).empty());
        }
        // Apart, or the asset folder inside the data folder (Hailstone never writes there): fine.
        CHECK(data_folder_conflict(eq, tmp.path / "hailstone", allow).empty());
        CHECK(data_folder_conflict(eq / "hailstone", eq, allow).empty());
        CHECK(data_folder_conflict(tmp.path / "RoF", tmp.path / "RoF2", allow).empty());
        CHECK(data_folder_conflict(eq, {}, allow).empty());
    }

    // A saved assets.path that holds the data folder goes to the picker, saying why (this build's rule).
    Config config;
    config.set("assets.path", eq.string());
    auto r = resolve_asset_folder({}, config, only(eq), eq);
    CHECK(r.source == AssetFolderSource::Picker);
    CHECK(r.reason.find("this same folder") != std::string::npos);
    auto nested = resolve_asset_folder({}, config, only(eq), eq / "hailstone");
    if (data_folder_subfolder_allowed()) {
        CHECK(nested.source == AssetFolderSource::Config);
    } else {
        CHECK(nested.source == AssetFolderSource::Picker);
        CHECK(nested.reason.find("inside this folder") != std::string::npos);
    }
    CHECK(resolve_asset_folder({}, config, only(eq), tmp.path / "data").source == AssetFolderSource::Config);
}

TEST_CASE("Asset folder: Hailstone's own folder inside a game folder is not indexed") {
    TempDir base("own"), overlay("own_overlay"), classic("own_classic");
    auto touch = [](const fs::path& p) {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << "x";
    };
    touch(base.path / "spells_us.txt");
    touch(base.path / "hailstone" / "hailstone.ini");
    touch(base.path / "hailstone" / "servers" / "x" / "assets" / "zone.eqg");
    touch(base.path / "maps" / "hailstone" / "qeynos.txt"); // deeper: a game folder's own, indexed as before
    touch(overlay.path / "hailstone" / "kept.txt");         // an overlay is Hailstone's folder already: all of it
    touch(classic.path / "HAILSTONE" / "logs" / "a.txt");
    touch(classic.path / "old.txt");
    GameDir game(base.path, overlay.path);
    game.set_classic(classic.path);
    CHECK(game.exists("spells_us.txt"));
    CHECK_FALSE(game.exists("hailstone/hailstone.ini"));
    CHECK_FALSE(game.exists("hailstone/servers/x/assets/zone.eqg"));
    CHECK_FALSE(game.exists("hailstone/logs/a.txt"));
    CHECK(game.exists("maps/hailstone/qeynos.txt"));
    CHECK(game.exists("hailstone/kept.txt"));
    CHECK(game.exists("old.txt"));
    CHECK(game.list("hailstone").size() == 1); // the overlay's
    game.rescan();
    CHECK_FALSE(game.exists("hailstone/hailstone.ini"));
}

TEST_CASE("Asset folder: a live base needs the server-matched files from the server's assets folder") {
    TempDir base("live"), overlay("overlay");
    auto touch = [](const fs::path& p) {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << "x";
    };
    touch(base.path / "spells_us.txt");
    touch(base.path / "eqstr_us.txt");
    GameDir rof2(base.path, overlay.path);
    CHECK_FALSE(is_live_asset_folder(rof2));
    CHECK(missing_server_files(rof2).empty()); // a RoF2 base has the right ones

    touch(base.path / "spells_us_str.txt"); // only the live client ships it
    touch(overlay.path / "spells_us.txt");
    touch(overlay.path / "RESOURCES" / "skillcaps.txt");
    GameDir live(base.path, overlay.path);
    CHECK(is_live_asset_folder(live));
    CHECK(missing_server_files(live) ==
          std::vector<std::string>{"eqstr_us.txt", "dbstr_us.txt", "Resources/BaseData.txt", "Resources/GlobalLoad.txt"});
}
