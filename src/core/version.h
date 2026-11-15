#pragma once

// Release versions as SemVer 2.0.0 writes them ("0.1.0", "0.2.0-alpha", "1.0.0-beta.2"; a leading 'v' as git tags
// have it is taken too), compared by SemVer's precedence: the numbers, then a pre-release below its release, and
// pre-release fields one by one (numeric ones as numbers, below any text). Build metadata ("+...") must be
// dot-separated [0-9A-Za-z-] identifiers, as SemVer has it, and is then ignored.
// The updater uses it to tell whether a release is newer than this build (docs/updater.md).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone {

struct Version {
    std::uint32_t major = 0, minor = 0, patch = 0;
    std::vector<std::string> pre; // the pre-release fields ("alpha", "2"), empty for a release

    std::string to_string() const;
};

// Nullopt when `text` is not a version.
std::optional<Version> parse_version(std::string_view text);

// <0, 0 or >0 as a comes before, with or after b.
int compare_versions(const Version& a, const Version& b);

} // namespace hailstone
