#pragma once

// Minimal zip reader (the updater's packages, docs/updater.md): the central directory is read once, and each entry is
// streamed out a piece at a time, so an archive of a few hundred MB is never held in memory. Stored and deflated
// entries only; no zip64 and no encryption, which the release script never makes. The archive comes from a download,
// so nothing in it is trusted: every offset and length is checked against the archive's size, a name that could
// leave the folder it is extracted into refuses the whole archive, and an entry is cut off as soon as it would
// inflate past its declared size (a zip bomb) and checked against its CRC-32 at the end.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone {

struct ZipEntry {
    std::string name;              // relative, '/' separators (is_safe_zip_path); a folder ends in '/'
    std::uint16_t method = 0;      // 0 stored, 8 deflated; extract() refuses any other
    std::uint32_t crc32 = 0;
    std::uint32_t packed = 0;      // the size in the archive
    std::uint32_t size = 0;        // the size extracted
    std::uint32_t offset = 0;      // of the entry's local header
    std::uint32_t unix_mode = 0;   // st_mode when the archive was made on Unix (its permission bits), else 0
    bool is_directory() const { return !name.empty() && name.back() == '/'; }
};

// Whether `name` stays inside the folder it is extracted into: not empty, relative, '/' separators only, no drive
// (':'), no "." or ".." part, no empty part (a trailing '/' for a folder aside), no control character, and no part
// Windows would alias (one ending in '.' or ' ', or a device name such as NUL or com1.txt: core/windows_names.h).
bool is_safe_zip_path(std::string_view name);

class ZipReader {
public:
    // Called with each piece of an entry as it is extracted; false stops the extraction (an error).
    using Sink = std::function<bool(std::span<const std::byte>)>;

    // Reads the central directory. False (with error() set) when the file cannot be read, is not a zip this reads,
    // or holds an unsafe name.
    bool open(const std::filesystem::path& path);
    bool open(std::vector<std::byte> bytes);

    const std::vector<ZipEntry>& entries() const { return entries_; }
    // The entry named `name`, or null.
    const ZipEntry* find(std::string_view name) const;

    // Streams the entry's contents to `sink`; false (with error() set) on a read error, an unsupported method, a size
    // or CRC-32 that does not match, or when the sink refused a piece.
    bool extract(const ZipEntry& entry, const Sink& sink);
    // The entry written to `path` (its parent folders made) through a temporary file beside it, renamed into place
    // once complete, checked and flushed to the disk (platform/durable_file.h); `path` is left as it was on failure. On
    // Unix the entry's permission bits (rwx for owner, group and others) are applied when the archive has them. A
    // folder entry makes the folder.
    bool extract_to_file(const ZipEntry& entry, const std::filesystem::path& path);

    const std::string& error() const { return error_; }

private:
    bool fail(std::string why);
    bool read_at(std::uint64_t offset, std::span<std::byte> out);
    bool read_directory();

    std::ifstream file_;
    std::vector<std::byte> memory_;
    bool in_memory_ = false;
    std::uint64_t size_ = 0;
    std::vector<ZipEntry> entries_;
    std::string error_;
};

} // namespace hailstone
