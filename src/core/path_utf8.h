#pragma once

// A path as UTF-8 text, for showing, logging and keys. path::string() and generic_string() convert to the ANSI code
// page on Windows and throw std::system_error for a character it lacks; the programs run with UTF-8 as that code
// page (cmake/utf8.manifest, Windows 10 1903 and later), but a build without the manifest, or an older Windows, would
// throw from a user's profile or game folder name. These never throw, and give the same text for ASCII names.

#include <filesystem>
#include <string>

namespace hailstone {

inline std::string path_utf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

inline std::string generic_path_utf8(const std::filesystem::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

} // namespace hailstone
