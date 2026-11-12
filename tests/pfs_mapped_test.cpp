// platform::MappedFile and the file-backed PfsArchive storage: the whole archive is no longer a private heap copy.

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

#include "assets/character.h"
#include "assets/pfs.h"
#include "assets/wld.h"
#include "core/game_dir.h"
#include "pfs_fixtures.h"
#include "platform/durable_file.h"
#include "platform/mapped_file.h"
#include "scratch_dir.h"
#include "wld_fixtures.h"

using hailstone::assets::PfsArchive;
using hailstone::platform::MappedFile;

namespace {

void write_bytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    REQUIRE(f);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(f);
}

} // namespace

TEST_CASE("MappedFile maps a file read-only and moves like a handle") {
    static_assert(!std::is_copy_constructible_v<MappedFile>);
    static_assert(!std::is_copy_assignable_v<MappedFile>);
    static_assert(std::is_move_constructible_v<MappedFile>);
    hailstone::test::ScratchDir dir{"mapped"};
    auto path = dir.path() / "a.bin";
    std::vector<std::byte> bytes = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}};
    write_bytes(path, bytes);
    auto m = MappedFile::open(path);
    CHECK(m.mapped());
    CHECK(m.size() == bytes.size());
    CHECK(std::ranges::equal(m.view(), bytes));
    auto moved = std::move(m);
    CHECK_FALSE(m.mapped());
    CHECK(m.size() == 0);
    CHECK(moved.mapped());
    CHECK(std::ranges::equal(moved.view(), bytes));
}

TEST_CASE("MappedFile: a missing file throws and an empty file is unmapped, not an error") {
    hailstone::test::ScratchDir dir{"mapped_empty"};
    CHECK_THROWS(MappedFile::open(dir.path() / "missing.bin"));
    auto path = dir.path() / "empty.bin";
    std::ofstream(path, std::ios::binary) << "";
    auto m = MappedFile::open(path);
    CHECK_FALSE(m.mapped());
    CHECK(m.size() == 0);
    CHECK(m.view().empty());
}

TEST_CASE("MappedFile keeps the bytes it was opened from when the path is replaced") {
    hailstone::test::ScratchDir dir{"mapped_replace"};
    auto path = dir.path() / "a.bin";
    std::vector<std::byte> first = {std::byte{'1'}, std::byte{'2'}, std::byte{'3'}};
    std::vector<std::byte> second = {std::byte{'4'}, std::byte{'5'}, std::byte{'6'}, std::byte{'7'}};
    write_bytes(path, first);
    auto old = MappedFile::open(path);
    auto staging = dir.path() / "staging.bin";
    write_bytes(staging, second);
    std::error_code ec;
    REQUIRE(hailstone::platform::replace_file_synced(staging, path, ec));
    CHECK(std::ranges::equal(old.view(), first));
    auto fresh = MappedFile::open(path);
    CHECK(std::ranges::equal(fresh.view(), second));
}

TEST_CASE("PfsArchive::open matches from_memory but holds no heap copy of the archive") {
    using namespace hailstone::test::pfs_fixtures;
    std::vector<std::byte> text(10000);
    for (std::size_t i = 0; i < text.size(); ++i) text[i] = static_cast<std::byte>(i * 7 % 251);
    auto archive = make_pfs({{"thing.mod", text}, {"tiny.txt", {std::byte{'A'}}}}, 3000);
    hailstone::test::ScratchDir dir{"pfs_open"};
    auto path = dir.path() / "test.s3d";
    write_bytes(path, archive);
    auto disk = PfsArchive::open(path);
    auto heap = PfsArchive::from_memory(archive, "test.s3d");
    CHECK(disk.mapped());
    CHECK(disk.storage_bytes() == archive.size());
    CHECK(disk.owned_bytes() == 0);
    CHECK_FALSE(heap.mapped());
    CHECK(heap.storage_bytes() == archive.size());
    CHECK(heap.owned_bytes() == archive.size());
    CHECK(disk.names() == heap.names());
    CHECK(disk.contains("THING.MOD"));
    CHECK(disk.read("thing.mod").value() == heap.read("thing.mod").value());
    CHECK(disk.read_prefix("thing.mod", 4).value() == heap.read_prefix("thing.mod", 4).value());
    CHECK(disk.read_prefix("thing.mod", 4500).value() == heap.read_prefix("thing.mod", 4500).value());
    CHECK_FALSE(disk.read("missing.txt"));
}

TEST_CASE("PfsArchive::open keeps the old archive when the path is replaced under it") {
    using namespace hailstone::test::pfs_fixtures;
    hailstone::test::ScratchDir dir{"pfs_replace"};
    auto path = dir.path() / "test.s3d";
    auto one = make_pfs({{"a.txt", std::vector<std::byte>(10, std::byte{'1'})}});
    auto two = make_pfs({{"a.txt", std::vector<std::byte>(10, std::byte{'2'})}});
    write_bytes(path, one);
    auto old = PfsArchive::open(path);
    auto staging = dir.path() / "staging.s3d";
    write_bytes(staging, two);
    std::error_code ec;
    // Replacing while mapped must succeed: the mapping shares delete (see platform/mapped_file.h).
    REQUIRE(hailstone::platform::replace_file_synced(staging, path, ec));
    REQUIRE(old.read("a.txt"));
    CHECK((*old.read("a.txt"))[0] == std::byte{'1'});
    auto fresh = PfsArchive::open(path);
    REQUIRE(fresh.read("a.txt"));
    CHECK((*fresh.read("a.txt"))[0] == std::byte{'2'});
}

TEST_CASE("PfsArchive copies share storage and stay valid when the original dies") {
    using namespace hailstone::test::pfs_fixtures;
    auto archive = make_pfs({{"a.txt", std::vector<std::byte>(20, std::byte{'x'})}});
    hailstone::test::ScratchDir dir{"pfs_copy"};
    auto path = dir.path() / "test.s3d";
    write_bytes(path, archive);
    std::optional<PfsArchive> copy;
    {
        auto original = PfsArchive::open(path);
        copy = original; // a copy, not a move: it must share the storage
        CHECK(copy->mapped());
    }
    REQUIRE(copy);
    CHECK(copy->read("a.txt").value() == std::vector<std::byte>(20, std::byte{'x'}));
    auto moved = std::move(*copy);
    CHECK(moved.read("a.txt").value() == std::vector<std::byte>(20, std::byte{'x'}));
}

TEST_CASE("PfsArchive::open refuses malformed archives the way from_memory does") {
    hailstone::test::ScratchDir dir{"pfs_bad"};
    auto bad = dir.path() / "bad.s3d";
    write_bytes(bad, std::vector<std::byte>(64, std::byte{0x11}));
    CHECK_THROWS(PfsArchive::open(bad));
    CHECK_THROWS(PfsArchive::from_memory(std::vector<std::byte>(64, std::byte{0x11})));
    CHECK_THROWS(PfsArchive::open(dir.path() / "missing.s3d"));
    auto empty = dir.path() / "empty.s3d";
    std::ofstream(empty, std::ios::binary) << "";
    CHECK_THROWS(PfsArchive::open(empty));
    // A directory offset past the end of the mapped file is bounds-checked as it was in a heap buffer.
    using namespace hailstone::test::pfs_fixtures;
    auto good = make_pfs({{"a.txt", std::vector<std::byte>(10, std::byte{'w'})}});
    auto past = dir.path() / "past.s3d";
    write_bytes(past, good);
    std::fstream f(past, std::ios::binary | std::ios::in | std::ios::out);
    REQUIRE(f);
    hailstone::ByteWriter offset;
    offset.le(std::uint32_t{0x7fffffffu});
    f.seekp(0);
    f.write(reinterpret_cast<const char*>(offset.data().data()), 4);
    REQUIRE(f);
    f.close();
    CHECK_THROWS(PfsArchive::open(past));
}

TEST_CASE("PfsArchive::open reads concurrently from the mapping") {
    using namespace hailstone::test::pfs_fixtures;
    std::vector<std::byte> blob(50000);
    for (std::size_t i = 0; i < blob.size(); ++i) blob[i] = static_cast<std::byte>((i * 13) ^ (i >> 7));
    hailstone::test::ScratchDir dir{"pfs_threads"};
    auto path = dir.path() / "test.s3d";
    write_bytes(path, make_pfs({{"blob.bin", blob}}));
    auto archive = PfsArchive::open(path);
    constexpr int kThreads = 8;
    std::vector<std::thread> threads;
    std::atomic<bool> ok{true};
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < 40; ++i) {
                auto got = archive.read("BLOB.BIN");
                if (!got || *got != blob) ok = false;
                auto head = archive.read_prefix("blob.bin", 16);
                if (!head || head->size() != 16 || !std::equal(head->begin(), head->end(), blob.begin())) ok = false;
            }
        });
    for (auto& t : threads) t.join();
    CHECK(ok.load());
}

TEST_CASE("WldFile::retained_bytes counts the string table, fragments and payload vectors") {
    using namespace hailstone::test::wld_fixtures;
    WldBuilder b;
    auto bmp = b.add(0x03, b.str("A_SPRITE"), bitmap_name("a.bmp"));
    ByteWriter info;
    info.le(std::uint32_t{0});
    info.le(std::uint32_t{1});
    info.le(bmp);
    b.add(0x04, b.str("A_SPRITEINFO"), info);
    auto w = wld::WldFile::parse(b.build(), "test");
    CHECK(w.retained_bytes() > 0);
    CHECK(w.retained_bytes() < 4096);
    WldBuilder empty;
    auto bare = wld::WldFile::parse(empty.build(), "empty");
    CHECK(bare.retained_bytes() < w.retained_bytes());
    CHECK(bare.retained_bytes() > 0);
}

TEST_CASE("CharacterArchive accounting separates mapped archive bytes from parsed bytes") {
    using namespace hailstone::test::pfs_fixtures;
    using namespace hailstone::test::wld_fixtures;
    WldBuilder b;
    b.add(0x03, b.str("A_SPRITE"), bitmap_name("a.bmp"));
    auto wld_bytes = b.build();
    auto archive_bytes = make_pfs({{"model.wld", wld_bytes}, {"tex.bmp", std::vector<std::byte>(64, std::byte{0x42})}});
    auto from_heap = PfsArchive::from_memory(archive_bytes, "model_chr.s3d");
    hailstone::assets::CharacterArchive owned;
    owned.archive = std::make_shared<const PfsArchive>(from_heap);
    owned.wlds.push_back(wld::WldFile::parse(wld_bytes, "model_chr.s3d:model.wld"));
    CHECK(owned.archive_bytes() == archive_bytes.size());
    CHECK(owned.owned_bytes() == archive_bytes.size());
    CHECK(owned.parsed_bytes() > 0);
    CHECK(owned.parsed_bytes() == owned.wlds[0].retained_bytes());
    hailstone::test::ScratchDir dir{"char_bytes"};
    auto path = dir.path() / "model_chr.s3d";
    write_bytes(path, archive_bytes);
    hailstone::assets::CharacterArchive file_backed;
    file_backed.archive = std::make_shared<const PfsArchive>(PfsArchive::open(path));
    file_backed.wlds.push_back(wld::WldFile::parse(wld_bytes, "model_chr.s3d:model.wld"));
    CHECK(file_backed.archive->mapped());
    CHECK(file_backed.archive_bytes() == archive_bytes.size());
    CHECK(file_backed.owned_bytes() == 0);
    CHECK(file_backed.parsed_bytes() == owned.parsed_bytes());
}
