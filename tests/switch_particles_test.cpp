#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "app/switch_particles.h"
#include "assets/legacy_particles.h"
#include "core/byte_io.h"
#include "core/game_dir.h"
#include "game_files.h"
#include "pfs_fixtures.h"
#include "scratch_dir.h"
#include "wld_fixtures.h"

using namespace hailstone;

namespace {
std::vector<app::SwitchParticles::Effect> effects() {
    std::vector<app::SwitchParticles::Effect> out(5);
    assets::EmitterDefinition d;
    d.texture = "synthetic.bmp";
    d.frames = 1;
    d.lifetime = 2.0f;
    d.per_tick = 1;
    d.ticks_per_second = 10;
    d.gravity = 8.0f;
    d.additive = 1;
    out[4][0] = d;
    out[4][2] = d;
    return out;
}

game::Door emitter_door(std::uint8_t id = 7) {
    game::Door d;
    d.id = id;
    d.type = 0x35;
    d.state = d.default_state = game::kSwitchOpen;
    d.y = 10;
    d.x = 20;
    d.z = 30;
    d.param = 4;
    return d;
}
// An old spells.eff whose effect 4 has one explicit-position slot drawing `sprite`.
std::vector<std::byte> legacy_table(const std::string& sprite) {
    std::vector<std::byte> out(assets::kLegacyParticleEffectCount * assets::kLegacyParticleEffectSize);
    const std::size_t stage = 4 * assets::kLegacyParticleEffectSize + 8 + 2 * 0x3a0;
    ByteWriter name;
    name.fixed_string(sprite, 32);
    std::copy(name.data().begin(), name.data().end(), out.begin() + static_cast<std::ptrdiff_t>(stage));
    out[stage + 0x8c] = std::byte{1}; // selector 1: an explicit position
    return out;
}

// gequip.s3d with one 0x26 sprite named `sprite` on GENG00.DDS, stored uncompressed: its size depends on the
// sprite name's length alone.
std::vector<std::byte> gequip(const std::string& sprite) {
    using namespace test::wld_fixtures;
    WldBuilder b;
    const auto bmp_name = b.str("GENG00.DDS");
    auto bmp = b.add(0x03, bmp_name, bitmap_name("GENG00.DDS"));
    ByteWriter info;
    info.le(std::uint32_t{0x18});
    info.le(std::uint32_t{1});
    info.le(std::uint32_t{100});
    info.le(bmp);
    auto def = b.add(0x04, b.str("SWITCH_SPRITE"), info);
    ByteWriter ref;
    ref.le(def);
    ref.le(std::uint32_t{0});
    auto simple = b.add(0x05, 0, ref);
    ByteWriter blit;
    blit.le(std::uint32_t{0});
    blit.le(simple);
    blit.le(std::uint32_t{0x80000017});
    b.add(0x26, b.str(sprite), blit);
    return test::pfs_fixtures::make_pfs({{"gequip.wld", b.build()}, {"geng00.dds", std::vector<std::byte>(16)}}, 3000,
                                        Z_NO_COMPRESSION);
}

void write_file(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
} // namespace

TEST_CASE("Switch particles keep independent owners and preserve duplicate door snapshots") {
    app::SwitchParticles particles(effects());
    auto first = emitter_door(), second = emitter_door(8);
    REQUIRE(particles.start(first, first.param));
    REQUIRE(particles.start(second, second.param));
    REQUIRE(particles.simulation().emitters().size() == 4);
    particles.update(0.1f, first.world_position());
    const auto age = particles.simulation().emitters()[0].age;
    REQUIRE(particles.start(first, first.param));
    CHECK(particles.simulation().emitters().size() == 4);
    CHECK(particles.simulation().emitters()[0].age == age);
    particles.stop(first);
    CHECK(particles.simulation().emitters().size() == 2);
    CHECK(particles.start(first, first.param));
    CHECK(particles.simulation().emitters().size() == 4);
    particles.clear();
    CHECK(particles.simulation().emitters().empty());
}

TEST_CASE("Switch particle removal and replacement retire the old scene emitters") {
    app::SwitchParticles particles(effects());
    auto door = emitter_door();
    REQUIRE(particles.start(door, door.param));
    particles.reconcile(std::span<const game::Door>(&door, 1));
    CHECK(particles.simulation().emitters().size() == 2);
    door.z += 100;
    particles.reconcile(std::span<const game::Door>(&door, 1));
    CHECK(particles.simulation().emitters().empty());
    REQUIRE(particles.start(door, door.param));
    CHECK(particles.simulation().emitters()[0].origin.z == 130);
    particles.reconcile({});
    CHECK(particles.simulation().emitters().empty());
    REQUIRE(particles.start(door, door.param));
    door.state = game::kSwitchClosed;
    particles.reconcile(std::span<const game::Door>(&door, 1));
    CHECK(particles.simulation().emitters().empty());
}

TEST_CASE("A delayed door tick keeps running particle switches through transitional states") {
    app::SwitchParticles particles(effects());
    game::DoorParticles callbacks;
    callbacks.local_player = [] { return true; };
    callbacks.start = [&](game::Door& d, std::int32_t effect) { return particles.start(d, effect); };
    callbacks.stop = [&](game::Door& d) { particles.stop(d); };
    std::vector<game::Door> doors{emitter_door()};
    game::SwitchClock clock;
    game::process_doors(doors, clock, 100, 0, nullptr, &callbacks);
    game::process_doors(doors, clock, 120, 0, nullptr, &callbacks);
    REQUIRE(doors[0].particle);
    doors[0].state = game::kSwitchOpening;
    game::process_doors(doors, clock, 2000, 0, nullptr, &callbacks); // native long-frame guard
    particles.reconcile(doors);
    REQUIRE(particles.active(doors[0]));
    game::process_doors(doors, clock, 2020, 0, nullptr, &callbacks);
    CHECK(doors[0].state == game::kSwitchOpen);
    particles.reconcile(doors);
    CHECK(particles.active(doors[0]));
    CHECK(particles.simulation().emitters().size() == 2);
}

TEST_CASE("Switch particles reject unavailable effects and recover from zero density") {
    app::SwitchParticles particles(effects());
    auto door = emitter_door();
    CHECK_FALSE(particles.start(door, -1));
    CHECK_FALSE(particles.start(door, 0));
    CHECK_FALSE(particles.start(door, 3));
    CHECK_FALSE(particles.start(door, 256));
    door.z = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(particles.start(door, 4));
    door = emitter_door();
    particles.set_density(0);
    CHECK_FALSE(particles.start(door, 4));
    CHECK(particles.simulation().emitters().empty());
    particles.set_density(1);
    REQUIRE(particles.start(door, 4));
    particles.update(0.1f, door.world_position());
    particles.update(0.1f, door.world_position());
    REQUIRE_FALSE(particles.simulation().emitters()[0].particles.empty());
    CHECK(particles.simulation().emitters()[0].particles[0].position.z < door.z);
    CHECK(particles.simulation().emitters()[0].origin == glm::vec3(10, 20, 30));
    particles.set_density(0);
    particles.update(0.1f, door.world_position());
    CHECK(particles.active(door));
    particles.set_density(1);
    particles.update(0.1f, door.world_position());
    CHECK(particles.active(door));
    CHECK(particles.simulation().emitters()[0].density == 1);
}

TEST_CASE("Legacy switch secondary slots retain native player-level gates") {
    auto definitions = effects();
    definitions[4][1] = definitions[4][0];
    app::SwitchParticles particles(std::move(definitions));
    const auto door = emitter_door();
    for (const auto level : {1, 23, 24, 38, 39, 60}) {
        REQUIRE(particles.start(door, 4, static_cast<std::uint8_t>(level)));
        CHECK(particles.simulation().emitters().size() == static_cast<std::size_t>(level < 24 ? 1 : level < 39 ? 2 : 3));
        particles.clear();
    }
}

TEST_CASE("Door switch callbacks drive the production particle manager lifecycle") {
    app::SwitchParticles particles(effects());
    game::DoorParticles callbacks;
    callbacks.local_player = [] { return true; };
    callbacks.start = [&](game::Door& d, std::int32_t effect) { return particles.start(d, effect); };
    callbacks.stop = [&](game::Door& d) { particles.stop(d); };
    std::vector<game::Door> doors{emitter_door()};
    game::SwitchClock clock;
    game::process_doors(doors, clock, 100, 0, nullptr, &callbacks);
    game::process_doors(doors, clock, 120, 0, nullptr, &callbacks);
    CHECK(doors[0].particle);
    CHECK(particles.simulation().emitters().size() == 2);
    game::change_state(doors[0], game::kSwitchClosed, 125);
    game::process_doors(doors, clock, 140, 0, nullptr, &callbacks);
    CHECK_FALSE(doors[0].particle);
    CHECK(particles.simulation().emitters().empty());
    game::change_state(doors[0], game::kSwitchOpen, 145);
    game::process_doors(doors, clock, 160, 0, nullptr, &callbacks);
    CHECK(doors[0].particle);
    CHECK(particles.simulation().emitters().size() == 2);
}

TEST_CASE("Installed legacy waterfall effects resolve sprites and fall from server switch positions") {
    const auto* game = test::game_dir();
    if (!game) return;
    if (!game->exists("spells.eff") || !game->exists("gequip.s3d")) return;
    app::SwitchParticles particles(*game);
    for (const auto effect : {132, 142, 143, 144}) {
        auto door = emitter_door();
        door.param = effect;
        REQUIRE(particles.start(door, effect));
        REQUIRE_FALSE(particles.simulation().emitters().empty());
        const auto& definition = particles.simulation().definitions()[particles.simulation().emitters()[0].definition];
        CHECK(definition.additive == 1);
        CHECK(definition.gravity > 0);
        CHECK(definition.frames == 1);
        // Long-lived cascade particles must move below the emitter rather than form a static sheet.
        if (effect == 142 || effect == 143) {
            for (int step = 0; step < 100; ++step) particles.update(0.05f, door.world_position());
            const auto& emitted = particles.simulation().emitters()[0].particles;
            REQUIRE_FALSE(emitted.empty());
            const auto lowest = std::min_element(emitted.begin(), emitted.end(),
                [](const game::Particle& a, const game::Particle& b) { return a.position.z < b.position.z; });
            CHECK(lowest->position.z < door.z - 20);
        }
        particles.clear();
    }
}

TEST_CASE("Switch particle effects are read again only when spells.eff or gequip.s3d changes") {
    test::ScratchDir dir("switch_particles");
    const auto s3d = dir.path() / "gequip.s3d";
    write_file(dir.path() / "spells.eff", legacy_table("SWITCH_SPRITE_A"));
    write_file(s3d, gequip("SWITCH_SPRITE_A"));
    const auto written = std::filesystem::last_write_time(s3d);
    const GameDir game(dir.path());
    const auto door = emitter_door();
    {
        app::SwitchParticles particles(game);
        CHECK(particles.start(door, door.param));
    }
    // The same size and time with the sprite renamed: the parsed effects are kept, as nothing was read again.
    const auto renamed = gequip("SWITCH_SPRITE_B");
    REQUIRE(renamed.size() == std::filesystem::file_size(s3d));
    write_file(s3d, renamed);
    std::filesystem::last_write_time(s3d, written);
    {
        app::SwitchParticles particles(game);
        CHECK(particles.start(door, door.param));
    }
    // A new time reads the archive again, where the effect's sprite is no longer found.
    std::filesystem::last_write_time(s3d, written + std::chrono::seconds(10));
    {
        app::SwitchParticles particles(game);
        CHECK_FALSE(particles.start(door, door.param));
    }
    // So does a changed spells.eff naming the new sprite.
    write_file(dir.path() / "spells.eff", legacy_table("SWITCH_SPRITE_B"));
    std::filesystem::last_write_time(dir.path() / "spells.eff", written + std::chrono::seconds(20));
    app::SwitchParticles particles(game);
    CHECK(particles.start(door, door.param));
}
