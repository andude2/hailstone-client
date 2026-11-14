#pragma once

// Which asset folder the client starts with, and the folders the asset-folder picker remembers. hailstone.ini keys:
//   assets.path    the folder last chosen in the picker
//   assets.recent  the folders chosen, most recent first, '|'-separated (at most kMaxRecentAssetFolders)
//   assets.pick    1: show the picker on the idle login screen, or at the next start, even when assets.path is
//                  fine (the Options window's "Change asset folder")
// --game-dir and EQ_GAME_DIR come first; they are never saved.

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/asset_folder.h"

namespace hailstone {

class Config;

inline constexpr std::size_t kMaxRecentAssetFolders = 8;

enum class AssetFolderSource {
    Override, // --game-dir or EQ_GAME_DIR
    Config,   // hailstone.ini assets.path
    Found,    // the EverQuest folder Hailstone's own folder sits in (`beside`), when nothing saved works; not saved
    Picker,   // nothing usable: the picker asks
};

struct AssetFolderResolution {
    AssetFolderSource source = AssetFolderSource::Picker;
    // The folder to use; for the picker the one it starts on (the saved assets.path, which may be wrong), or empty.
    std::filesystem::path path;
    // Why the picker shows, for its status line ("" on a first start, or when a folder was found).
    std::string reason;
    // The check of a saved assets.path that failed it (what the log names), else empty.
    AssetFolderCheck check;
};

// Checks a folder; validate_asset_folder() unless a test passes its own.
using AssetFolderValidator = std::function<AssetFolderCheck(const std::filesystem::path&)>;

// The order: the override (`override_dir`, from --game-dir or EQ_GAME_DIR) when it is a folder at all, as the player
// asked for it (what the check finds missing is only logged); then assets.path when it passes the check, no
// assets.pick is set and it does not hold `data_folder` (data_folder_conflict; an empty one is not checked); else
// `beside` (when not empty) next: the folder that holds Hailstone's own folder, used when it passes the check and
// does not hold `data_folder` by data_folder_conflict's rule. It is only a default, never saved, so a moved
// installation is found again; the caller passes it only in a HAILSTONE_ALLOW_IN_EQ_FOLDER build, when its own
// folder is named GameDir::kOwnFolder. Else
// the picker. An override that is no folder goes to the picker, saying so.
AssetFolderResolution resolve_asset_folder(const std::filesystem::path& override_dir, const Config& config,
                                           const AssetFolderValidator& validate = {},
                                           const std::filesystem::path& data_folder = {},
                                           const std::filesystem::path& beside = {});

// True in a build made with the CMake option HAILSTONE_ALLOW_IN_EQ_FOLDER (off by default): Hailstone's folder may
// then be a subfolder of the EverQuest folder (data_folder_conflict).
bool data_folder_subfolder_allowed();

// Why `asset_folder` cannot be the asset folder when Hailstone writes into `data_folder` (platform::data_dir()):
// the data folder is that folder or inside it, so Hailstone would write into the installation it must only read.
// "" when they are apart. With `allow_subfolder` a data folder directly in it named GameDir::kOwnFolder
// (<EverQuest>/hailstone/, as a server's patcher may install it; the name's case is ignored) is "" too: GameDir never
// indexes that folder. Any other place inside it, at any depth, is refused all the same, as is the folder itself.
// Both are made absolute and normal first (the disk is asked for links where it can be).
std::string data_folder_conflict(const std::filesystem::path& asset_folder, const std::filesystem::path& data_folder,
                                 bool allow_subfolder = data_folder_subfolder_allowed());

// True when two folder names are the same folder as written: compared after lexically_normal() and without a
// trailing separator, case-insensitively on Windows. The disk is not asked.
bool same_folder(const std::filesystem::path& a, const std::filesystem::path& b);

// assets.recent split into its folders, empty ones and repeats left out.
std::vector<std::string> recent_asset_folders(const Config& config);
// A stored list (as join_recent_folders writes it) split into its folders, empty ones and repeats left out.
std::vector<std::string> split_recent_folders(std::string_view all);
// `folder` put first in `recent`, an earlier entry for the same folder removed, cut to `max` entries.
std::vector<std::string> push_recent_folder(std::vector<std::string> recent, std::string_view folder,
                                            std::size_t max = kMaxRecentAssetFolders);
// `folder` taken out of `recent` (the picker's "Remove" on a folder that is gone).
std::vector<std::string> remove_recent_folder(std::vector<std::string> recent, std::string_view folder);
// The list as assets.recent stores it.
std::string join_recent_folders(const std::vector<std::string>& recent);

// The picker's choice: assets.path = `folder` (made absolute against the current directory), it first in
// assets.recent, and assets.pick cleared. The caller saves the config.
void remember_asset_folder(Config& config, const std::filesystem::path& folder);
// A folder as assets.path and a server's game_files keep it (core/server_profile.h): made absolute against the current
// directory, in a normal form, without a trailing separator.
std::string asset_folder_text(const std::filesystem::path& folder);
// Sets (or clears) assets.pick. The caller saves the config.
void request_asset_folder_pick(Config& config, bool pick);
bool asset_folder_pick_requested(const Config& config);

} // namespace hailstone
