#include <doctest/doctest.h>

#include <atomic>
#include <latch>
#include <thread>
#include <stdexcept>

#include "assets/wld.h"
#include "wld_fixtures.h"

using namespace hailstone;
using namespace hailstone::assets;

TEST_CASE("deferred WLD validates once and materializes only on payload access") {
    const auto bytes = test::wld_fixtures::mesh_wld(false);
    auto eager = wld::WldFile::parse(bytes);
    int reads = 0;
    auto lazy = wld::WldFile::deferred([&] { ++reads; return bytes; }, "deferred");
    CHECK(reads == 1);
    CHECK_FALSE(lazy.materialized());
    CHECK(lazy.retained_bytes() < eager.retained_bytes());
    std::vector<wld::Ref> eager_refs, lazy_refs;
    eager.visit_character_index([&](auto ref, auto type, const auto& name, auto track) {
        eager_refs.push_back(ref);
        CHECK(eager.at(ref)->type == type);
        CHECK(eager.at(ref)->name == name);
        CHECK((track != nullptr) == (type == 0x13));
    });
    lazy.visit_character_index([&](auto ref, auto, const auto&, auto) { lazy_refs.push_back(ref); });
    CHECK(eager_refs == lazy_refs);
    CHECK_FALSE(lazy.materialized());
    CHECK(reads == 1);
    auto copy = lazy;
    REQUIRE_FALSE(lazy_refs.empty());
    auto* fragment = lazy.at(lazy_refs.front());
    CHECK(lazy.materialized());
    CHECK(copy.materialized());
    CHECK(reads == 2);
    CHECK(copy.at(lazy_refs.front()) == fragment);
    CHECK(lazy.fragments().size() == eager.fragments().size());
    CHECK(lazy.new_format() == eager.new_format());
    CHECK(lazy.string_at(-1) == eager.string_at(-1));
    for (std::size_t i = 1; i <= eager.fragments().size(); ++i) {
        auto ref = static_cast<wld::Ref>(i);
        REQUIRE(lazy.at(ref));
        CHECK(lazy.at(ref)->type == eager.at(ref)->type);
        CHECK(lazy.at(ref)->name == eager.at(ref)->name);
        CHECK(lazy.at(ref)->data.index() == eager.at(ref)->data.index());
        if (const auto* mesh = lazy.get<wld::MeshDef>(ref)) {
            const auto* expected = eager.get<wld::MeshDef>(ref);
            REQUIRE(expected);
            CHECK(mesh->positions == expected->positions);
            CHECK(mesh->uvs == expected->uvs);
            CHECK(mesh->polygons.size() == expected->polygons.size());
        }
    }
    CHECK(reads == 2);
}

TEST_CASE("deferred WLD first use from concurrent readers decodes only once") {
    auto bytes = test::wld_fixtures::mesh_wld(true);
    std::atomic<int> reads{0};
    auto lazy = wld::WldFile::deferred([&] { ++reads; return bytes; }, "parallel");
    std::vector<const wld::Fragment*> pointers(8);
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < pointers.size(); ++i)
        threads.emplace_back([&, i] { pointers[i] = lazy.at(1); });
    for (auto& thread : threads) thread.join();
    REQUIRE(pointers.front());
    for (const auto* p : pointers) CHECK(p == pointers.front());
    CHECK(reads.load() == 2);
}

TEST_CASE("deferred WLD retains validation errors and excludes invalid resources from its index") {
    using namespace test::wld_fixtures;
    WldBuilder b;
    ByteWriter bad;
    b.add(0x14, b.str("BAD_ACTORDEF"), bad);
    auto bytes = b.build();
    auto lazy = wld::WldFile::deferred([bytes] { return bytes; }, "bad-actor");
    REQUIRE(lazy.errors().size() == 1);
    int entries = 0;
    lazy.visit_character_index([&](auto, auto, const auto&, auto) { ++entries; });
    CHECK(entries == 0);
    CHECK_FALSE(lazy.materialized());
    CHECK(std::holds_alternative<std::monostate>(lazy.at(1)->data));
    CHECK_THROWS(wld::WldFile::deferred([] { return std::vector<std::byte>(8); }, "truncated"));
}

TEST_CASE("deferred WLD retries a failed reload without publishing partial data") {
    auto bytes = test::wld_fixtures::mesh_wld(false);
    int reads = 0;
    auto lazy = wld::WldFile::deferred([&] {
        if (++reads == 2) throw std::runtime_error("read failed");
        return bytes;
    }, "retry");
    CHECK_THROWS(lazy.at(1));
    CHECK_FALSE(lazy.materialized());
    REQUIRE(lazy.at(1));
    CHECK(lazy.materialized());
    CHECK(reads == 3);
}

TEST_CASE("deferred WLD index preserves track references and timing without loading frames") {
    using namespace test::wld_fixtures;
    WldBuilder b;
    ByteWriter def;
    def.le(std::uint32_t{0x8});
    def.le(std::uint32_t{1});
    def.le(std::int16_t{16384});
    for (int i = 0; i < 6; ++i) def.le(std::int16_t{0});
    def.le(std::uint16_t{256});
    auto track = b.add(0x12, 0, def);
    ByteWriter instance;
    instance.le(track);
    instance.le(std::uint32_t{1});
    instance.le(std::uint32_t{125});
    auto ref = b.add(0x13, b.str("P01HUM_TRACK"), instance);
    auto bytes = b.build();
    auto lazy = wld::WldFile::deferred([bytes] { return bytes; }, "track-index");
    int count = 0;
    lazy.visit_character_index([&](auto indexed_ref, auto type, const auto& name, const auto* t) {
        ++count;
        CHECK(indexed_ref == ref);
        CHECK(type == 0x13);
        CHECK(name == "P01HUM_TRACK");
        REQUIRE(t);
        CHECK(t->track == track);
        CHECK(t->flags == 1);
        CHECK(t->frame_ms == 125);
    });
    CHECK(count == 1);
    CHECK_FALSE(lazy.materialized());
    const auto* decoded = lazy.get<wld::TrackDef>(track);
    REQUIRE(decoded);
    REQUIRE(decoded->frames.size() == 1);
    CHECK(decoded->frames.front().scale == 1.0f);
    CHECK(lazy.find("P01HUM_TRACK") == lazy.at(ref));
}

TEST_CASE("scoped WLD reads reclaim decoded data after the outermost build") {
    const auto bytes = test::wld_fixtures::mesh_wld(false);
    auto lazy = wld::WldFile::deferred([bytes] { return bytes; }, "scoped");
    const auto index_bytes = lazy.retained_bytes();
    std::string copied_name;
    {
        wld::WldFile::ReadScope outer;
        auto* f = lazy.at(1);
        REQUIRE(f);
        copied_name = f->name;
        {
            wld::WldFile::ReadScope inner;
            auto copy = lazy;
            CHECK(copy.at(1) == f);
        }
        CHECK(lazy.materialized());
        CHECK(f->name == copied_name);
        CHECK(lazy.decode_stats().releases == 0);
    }
    CHECK_FALSE(lazy.materialized());
    CHECK(lazy.retained_bytes() == index_bytes);
    CHECK(lazy.decode_stats().decodes == 1);
    CHECK(lazy.decode_stats().releases == 1);
    CHECK(lazy.decode_stats().released_bytes > 0);
    {
        wld::WldFile::ReadScope again;
        CHECK(lazy.at(1)->name == copied_name);
    }
    CHECK_FALSE(lazy.materialized());
    CHECK(lazy.decode_stats().decodes == 2);
    CHECK(lazy.decode_stats().releases == 2);
}

TEST_CASE("scoped WLD reads reclaim on exceptions and preserve unscoped callers") {
    const auto bytes = test::wld_fixtures::mesh_wld(false);
    auto lazy = wld::WldFile::deferred([bytes] { return bytes; }, "unwind");
    CHECK_THROWS([&] {
        wld::WldFile::ReadScope scope;
        (void)lazy.at(1);
        throw std::runtime_error("model build failed");
    }());
    CHECK_FALSE(lazy.materialized());
    const auto* unscoped = lazy.at(1);
    REQUIRE(unscoped);
    {
        wld::WldFile::ReadScope scope;
        CHECK(lazy.at(1) == unscoped);
    }
    CHECK(lazy.materialized());
    CHECK(unscoped->type == 0x03);
    CHECK(lazy.decode_stats().releases == 1);
}

TEST_CASE("scoped WLD reads release only after the last concurrent reader") {
    const auto bytes = test::wld_fixtures::mesh_wld(false);
    auto lazy = wld::WldFile::deferred([bytes] { return bytes; }, "concurrent-lease");
    std::latch acquired{1}, finish{1};
    std::atomic<bool> valid{false};
    std::thread worker;
    {
        wld::WldFile::ReadScope scope;
        const auto* first = lazy.at(1);
        worker = std::thread([&] {
            wld::WldFile::ReadScope other;
            const auto* f = lazy.at(1);
            acquired.count_down();
            finish.wait();
            valid = f && f->type == 0x03;
        });
        acquired.wait();
        CHECK(lazy.materialized());
        CHECK(first->type == 0x03);
    }
    CHECK(lazy.materialized());
    CHECK(lazy.decode_stats().releases == 0);
    finish.count_down();
    worker.join();
    CHECK(valid.load());
    CHECK_FALSE(lazy.materialized());
    CHECK(lazy.decode_stats().decodes == 1);
    CHECK(lazy.decode_stats().releases == 1);
}

TEST_CASE("failed scoped WLD reload leaves no lease behind") {
    const auto bytes = test::wld_fixtures::mesh_wld(false);
    int calls = 0;
    auto lazy = wld::WldFile::deferred([&] {
        if (++calls == 2) throw std::runtime_error("read failed");
        return bytes;
    }, "scoped-retry");
    {
        wld::WldFile::ReadScope scope;
        CHECK_THROWS(lazy.at(1));
    }
    CHECK_FALSE(lazy.materialized());
    {
        wld::WldFile::ReadScope scope;
        REQUIRE(lazy.at(1));
    }
    CHECK_FALSE(lazy.materialized());
    CHECK(lazy.decode_stats().decodes == 1);
    CHECK(lazy.decode_stats().releases == 1);
}

TEST_CASE("unscoped reader during an active WLD lease keeps its escaped pointer alive") {
    const auto bytes = test::wld_fixtures::mesh_wld(false);
    auto lazy = wld::WldFile::deferred([bytes] { return bytes; }, "escaped-pointer");
    const wld::Fragment* escaped = nullptr;
    {
        wld::WldFile::ReadScope scope;
        REQUIRE(lazy.at(1));
        std::thread worker([&] { escaped = lazy.at(1); }); // this thread has no ReadScope
        worker.join();
    }
    REQUIRE(escaped);
    CHECK(lazy.materialized());
    CHECK(escaped->type == 0x03);
    CHECK(lazy.decode_stats().releases == 0);
}
