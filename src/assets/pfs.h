#pragma once

// PFS archives: .s3d, .eqg, .pak. Layout (OpenEQ docs/ARCHITECTURE.md, and
// widely documented by community tools):
//   u32 directory offset, "PFS " magic, u32 version (0x20000)
//   at the directory offset: u32 count, then count × {u32 crc, u32 offset, u32 size}
//   each file is a run of {u32 deflated, u32 inflated, zlib stream} blocks
// Each directory crc is pfs_name_crc() of the lower-case file name, and the
// file-name table is the entry with crc 0x61580ac9 (or, in some third-party
// archives, 0xffffffff at the highest offset). Names are matched to entries by
// crc. Some shipped archives list a name with no entry (greatdivide_chr.s3d,
// velketor_chr.s3d); those names are dropped.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "platform/mapped_file.h"

namespace hailstone::assets {

class PfsArchive {
public:
    // Throws std::runtime_error on a malformed archive.
    // open() maps the file read-only (platform::MappedFile) instead of copying it into the heap: only the index
    // below is private memory, and the mapping keeps the bytes it was opened from when the path is replaced.
    static PfsArchive open(const std::filesystem::path& path);
    static PfsArchive from_memory(std::vector<std::byte> data, std::string label = "<memory>");

    // File names as stored (lower case), in data-offset order.
    const std::vector<std::string>& names() const { return names_; }
    bool contains(std::string_view name) const;

    // Inflated contents of a file (name matched case-insensitively).
    std::optional<std::vector<std::byte>> read(std::string_view name) const;
    // The first min(max, size) bytes of a file, inflating no more of it than that (a magic test before reading a
    // whole member). Throws on a malformed file as read() does, over what it inflates.
    std::optional<std::vector<std::byte>> read_prefix(std::string_view name, std::size_t max) const;

    const std::string& label() const { return label_; }

    // Storage accounting for the archive bytes themselves (the index below and anything parsed from the members,
    // such as wld::WldFile data, is separate private memory and not counted here). storage_bytes() is the
    // underlying file or buffer size; owned_bytes() is the part in private heap (the buffer's size, or 0 when
    // mapped); mapped() is true for a file-backed archive.
    bool mapped() const;
    std::size_t storage_bytes() const;
    std::size_t owned_bytes() const;

private:
    struct Entry {
        std::uint32_t crc = 0;
        std::uint32_t offset = 0;
        std::uint32_t size = 0;
    };

    // The archive's bytes, shared between copies: an owned buffer (from_memory) or a read-only file mapping
    // (open). Copies share it; a moved-from archive shares nothing.
    struct Storage {
        std::vector<std::byte> owned;
        platform::MappedFile mapped_file;
        std::span<const std::byte> view() const {
            return mapped_file.mapped() ? mapped_file.view() : std::span<const std::byte>(owned);
        }
    };

    void parse_index();
    std::vector<std::byte> inflate_entry(const Entry& e) const;
    std::span<const std::byte> bytes() const { return storage_ ? storage_->view() : std::span<const std::byte>{}; }

    std::string label_;
    std::shared_ptr<const Storage> storage_;
    std::vector<std::string> names_;
    std::unordered_map<std::string, Entry> by_name_;
};

// CRC-32 (polynomial 0x04c11db7, not reflected, initial value 0) over the name
// including its NUL terminator.
std::uint32_t pfs_name_crc(std::string_view name);

// Inflates one zlib stream into exactly `out.size()` bytes. Throws on error.
void zlib_inflate(std::span<const std::byte> in, std::span<std::byte> out);

} // namespace hailstone::assets
