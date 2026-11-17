#pragma once

// A builder for small PFS (S3D) archives in memory, shared by the fuzz tests and the character library tests.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>
#include <zlib.h>

#include "assets/pfs.h"
#include "core/byte_io.h"

namespace hailstone::test::pfs_fixtures {

using namespace hailstone;
using namespace hailstone::assets;

// `level` is zlib's: Z_NO_COMPRESSION makes an archive's size depend on its files' sizes alone.
inline std::vector<std::byte> deflate_blocks(const std::vector<std::byte>& data, std::size_t block,
                                             int level = Z_DEFAULT_COMPRESSION) {
    ByteWriter w;
    std::size_t at = 0;
    do {
        std::size_t n = std::min(block, data.size() - at);
        uLongf cap = compressBound(static_cast<uLong>(n));
        std::vector<Bytef> out(cap);
        REQUIRE(compress2(out.data(), &cap, reinterpret_cast<const Bytef*>(data.data()) + at, static_cast<uLong>(n),
                          level) == Z_OK);
        w.le<std::uint32_t>(static_cast<std::uint32_t>(cap));
        w.le<std::uint32_t>(static_cast<std::uint32_t>(n));
        w.bytes({reinterpret_cast<const std::byte*>(out.data()), cap});
        at += n;
    } while (at < data.size());
    return w.take();
}

inline std::vector<std::byte> make_pfs(const std::vector<std::pair<std::string, std::vector<std::byte>>>& files, std::size_t block = 3000,
                                       int level = Z_DEFAULT_COMPRESSION) {
    ByteWriter body; // after the 12-byte header
    struct E { std::uint32_t crc, offset, size; };
    std::vector<E> entries;
    for (const auto& [name, data] : files) {
        entries.push_back({pfs_name_crc(name), static_cast<std::uint32_t>(12 + body.size()), static_cast<std::uint32_t>(data.size())});
        body.bytes(deflate_blocks(data, block, level));
    }
    ByteWriter names;
    names.le<std::uint32_t>(static_cast<std::uint32_t>(files.size()));
    for (const auto& [name, data] : files) {
        names.le<std::uint32_t>(static_cast<std::uint32_t>(name.size() + 1));
        names.fixed_string(name, name.size() + 1);
    }
    entries.push_back({0x61580ac9u, static_cast<std::uint32_t>(12 + body.size()), static_cast<std::uint32_t>(names.size())});
    body.bytes(deflate_blocks(names.data(), 3000, level));
    ByteWriter w;
    w.le<std::uint32_t>(static_cast<std::uint32_t>(12 + body.size()));
    w.le<std::uint32_t>(0x20534650);
    w.le<std::uint32_t>(0x20000);
    w.bytes(body.data());
    w.le<std::uint32_t>(static_cast<std::uint32_t>(entries.size()));
    for (const auto& e : entries) {
        w.le(e.crc);
        w.le(e.offset);
        w.le(e.size);
    }
    return w.take();
}

} // namespace hailstone::test::pfs_fixtures
