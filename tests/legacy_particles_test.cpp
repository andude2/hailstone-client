#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "assets/legacy_particles.h"
#include "assets/pfs.h"
#include "core/byte_io.h"
#include "fuzz_support.h"
#include "game_files.h"

using namespace hailstone;
using namespace hailstone::assets;

namespace {

constexpr std::size_t kStage = 8 + 2 * 0x3a0;

std::vector<std::byte> table() {
    return std::vector<std::byte>(kLegacyParticleEffectCount * kLegacyParticleEffectSize);
}

void write(std::vector<std::byte>& bytes, std::size_t effect, std::size_t offset, const ByteWriter& value) {
    std::copy(value.data().begin(), value.data().end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(effect * kLegacyParticleEffectSize + kStage + offset));
}

void word(std::vector<std::byte>& bytes, std::size_t effect, std::size_t offset, std::uint32_t value) {
    ByteWriter w;
    w.le(value);
    write(bytes, effect, offset, w);
}

void real(std::vector<std::byte>& bytes, std::size_t effect, std::size_t offset, float value) {
    ByteWriter w;
    w.f32(value);
    write(bytes, effect, offset, w);
}

void slot(std::vector<std::byte>& bytes, std::size_t effect, std::size_t index, std::string name) {
    ByteWriter w;
    w.fixed_string(name, 32);
    write(bytes, effect, index * 32, w);
    word(bytes, effect, 0x8c + index * 4, 1);
}

} // namespace

TEST_CASE("legacy switch particles retain effect and slot ordinals and stage overrides") {
    auto bytes = table();
    slot(bytes, 7, 0, "FIRST_SPRITE");
    slot(bytes, 7, 2, "THIRD_SPRITE");
    slot(bytes, 255, 1, "LAST_SPRITE");
    word(bytes, 7, 0x220 + 8, 0x7f123456);
    real(bytes, 7, 0x22c + 8, 3.25f);
    real(bytes, 7, 0x238 + 24, 1.0f);
    real(bytes, 7, 0x23c + 24, 2.0f);
    real(bytes, 7, 0x240 + 24, -3.0f);
    real(bytes, 7, 0x25c + 8, 4.5f);
    real(bytes, 7, 0x268 + 8, 24.0f);
    word(bytes, 7, 0x274 + 8, 3210);
    real(bytes, 7, 0x280 + 8, 6.0f);
    word(bytes, 7, 0x28c + 8, 37);
    real(bytes, 7, 0x298 + 8, 2.5f);
    const auto parsed = parse_legacy_switch_effects(bytes);
    REQUIRE(parsed.size() == 256);
    CHECK_FALSE(parsed[0][0]);
    REQUIRE(parsed[7][0]);
    CHECK_FALSE(parsed[7][1]);
    REQUIRE(parsed[7][2]);
    const auto& s = *parsed[7][2];
    CHECK(s.sprite == "THIRD_SPRITE");
    CHECK(s.color == 0x7f123456);
    CHECK(s.gravity == 3.25f);
    CHECK(s.direction == glm::vec3(1.0f, 2.0f, -3.0f));
    CHECK(s.radius == 4.5f);
    CHECK(s.angle == 24.0f);
    CHECK(s.lifetime_ms == 3210);
    CHECK(s.velocity == 6.0f);
    CHECK(s.interval_ms == 37);
    CHECK(s.scale == 2.5f);
    REQUIRE(parsed[255][1]);
    CHECK(parsed[255][1]->sprite == "LAST_SPRITE");
}

TEST_CASE("legacy switch particles skip actor-dependent slots and other stages") {
    auto bytes = table();
    slot(bytes, 1, 0, "ACTOR_SPRITE");
    slot(bytes, 1, 1, "POINT_SPRITE");
    slot(bytes, 1, 2, "POSITION_SPRITE");
    word(bytes, 1, 0x8c, 0);
    word(bytes, 1, 0x90, 2);
    // Identical-looking bytes in the cast stage must not become switch emitters.
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(kLegacyParticleEffectSize + kStage), 0x3a0,
                bytes.begin() + static_cast<std::ptrdiff_t>(2 * kLegacyParticleEffectSize + 8));
    const auto parsed = parse_legacy_switch_effects(bytes);
    CHECK_FALSE(parsed[1][0]);
    CHECK_FALSE(parsed[1][1]);
    REQUIRE(parsed[1][2]);
    CHECK_FALSE(parsed[2][2]);
}

TEST_CASE("legacy switch particle parsing rejects wrong lengths and unsafe slots independently") {
    CHECK_THROWS_AS(parse_legacy_switch_effects({}), std::runtime_error);
    auto bytes = table();
    bytes.pop_back();
    CHECK_THROWS_AS(parse_legacy_switch_effects(bytes), std::runtime_error);
    bytes = table();
    bytes.push_back(std::byte{0});
    CHECK_THROWS_AS(parse_legacy_switch_effects(bytes), std::runtime_error);
    bytes = table();
    slot(bytes, 1, 0, std::string(32, 'A')); // no terminator
    slot(bytes, 1, 1, "../outside");
    slot(bytes, 1, 2, "VALID_SPRITE");
    slot(bytes, 2, 0, "NAN_SPRITE");
    real(bytes, 2, 0x22c, std::numeric_limits<float>::quiet_NaN());
    slot(bytes, 2, 1, "INF_SPRITE");
    real(bytes, 2, 0x238 + 12, std::numeric_limits<float>::infinity());
    slot(bytes, 2, 2, "HUGE_INTERVAL");
    word(bytes, 2, 0x28c + 8, 0xffffffff);
    slot(bytes, 3, 0, "HUGE_LIFE");
    word(bytes, 3, 0x274, 0xffffffff);
    slot(bytes, 3, 1, "NEGATIVE_RADIUS");
    real(bytes, 3, 0x25c + 4, -1.0f);
    slot(bytes, 3, 2, "NEGATIVE_SIZE");
    real(bytes, 3, 0x298 + 8, -1.0f);
    const auto parsed = parse_legacy_switch_effects(bytes);
    CHECK_FALSE(parsed[1][0]);
    CHECK_FALSE(parsed[1][1]);
    CHECK(parsed[1][2]);
    for (const auto effect : {2, 3})
        for (const auto& s : parsed[static_cast<std::size_t>(effect)]) CHECK_FALSE(s);
}

TEST_CASE("legacy explicit-position emitters keep the external template and nonzero overrides") {
    LegacyParticleSlot s;
    s.sprite = "TEST_SPRITE";
    ParticleTexture texture;
    texture.file = "one.bmp";
    texture.frames = 2;
    texture.files = {"one.bmp", "two.bmp"};
    texture.timing_ms = 80;
    texture.material = 0x80000017;
    auto d = legacy_switch_definition(s, texture);
    REQUIRE(d);
    CHECK(d->name == "TEST_SPRITE");
    CHECK(d->texture_frames == texture.files);
    CHECK(d->frames == 2);
    CHECK(d->frames_per_second == 12.5f);
    CHECK(d->duration == doctest::Approx(-43200.0f));
    CHECK(d->lifetime == 2.0f);
    CHECK(d->gravity == -1.0f);
    CHECK(d->shape == 4);
    CHECK(d->shape_size.x == 0.0f);
    CHECK(d->angle1 == doctest::Approx(-96.09375f));
    CHECK(d->angle2 == 0.0f);
    CHECK(d->motion[0].x == 20.0f);
    CHECK(d->motion[3].y == doctest::Approx(20.0f * particle_tan(20.0f)));
    CHECK(d->capacity == 2000);
    CHECK(d->ticks_per_second == 1000.0f);
    CHECK(d->per_tick == 1);
    CHECK(d->scale1 == doctest::Approx(0.3f));
    CHECK(d->fade_out == 2.0f);
    CHECK(d->max_opacity == 1.0f);
    CHECK(d->color_start == glm::uvec3(255));
    CHECK(d->additive == 1);
    CHECK(d->no_zwrite == 1);

    s.gravity = 4.0f;
    s.direction = {0.0f, 0.0f, -1.0f};
    s.radius = 2.5f;
    s.angle = 32.0f;
    s.lifetime_ms = 1200;
    s.velocity = 3.5f;
    s.interval_ms = 80;
    s.scale = 2.0f;
    s.color = 0xff102030;
    d = legacy_switch_definition(s, texture);
    REQUIRE(d);
    CHECK(d->gravity == 4.0f);
    CHECK(d->angle1 == 256.0f);
    CHECK(d->shape_size.x == 2.5f);
    CHECK(d->lifetime == doctest::Approx(1.2f));
    CHECK(d->fade_out == doctest::Approx(1.2f));
    CHECK(d->motion[0].x == 3.5f);
    CHECK(d->motion[3].y == doctest::Approx(3.5f * particle_tan(32.0f)));
    CHECK(d->ticks_per_second == 12.5f); // interval 80 ms, not 80 particles/s
    CHECK(d->scale1 == 3.0f);
    CHECK(d->color_start == glm::uvec3(16, 32, 48));
    s.direction = {0.0f, -2.0f, 1.0f}; // Z takes priority over Y
    s.color = 0xff000000; // the old tint test uses the RGB bytes only
    d = legacy_switch_definition(s, texture);
    REQUIRE(d);
    CHECK(d->angle1 == 0.0f);
    CHECK(d->angle2 == 0.0f);
    CHECK(d->color_start == glm::uvec3(255));
    s.direction = {0.0f, -2.0f, 0.0f};
    d = legacy_switch_definition(s, texture);
    REQUIRE(d);
    CHECK(d->angle2 == -187.5f);
    s.gravity = std::numeric_limits<float>::infinity();
    CHECK_FALSE(legacy_switch_definition(s, texture));
    s.gravity = 0.0f;
    CHECK_FALSE(legacy_switch_definition(s, ParticleTexture{}));
    s.angle = 1000000.0f;
    s.direction = {1000000.0f, 0.0f, 0.0f};
    CHECK_FALSE(legacy_switch_definition(s, texture)); // the product cannot reach the simulator's integer cast
}

TEST_CASE("legacy switch particle tables tolerate fixed-size deterministic mutations") {
    auto seed = table();
    for (std::size_t index = 0; index < 3; ++index) {
        slot(seed, 7, index, "FUZZ_SPRITE");
        real(seed, 7, 0x22c + index * 4, 2.0f);
        real(seed, 7, 0x240 + index * 12, -1.0f);
        real(seed, 7, 0x268 + index * 4, 12.0f);
        word(seed, 7, 0x274 + index * 4, 1500);
        real(seed, 7, 0x280 + index * 4, 3.0f);
        word(seed, 7, 0x28c + index * 4, 50);
        real(seed, 7, 0x298 + index * 4, 1.0f);
    }
    const auto start = seed.begin() + static_cast<std::ptrdiff_t>(7 * kLegacyParticleEffectSize + kStage);
    const fuzz::Bytes stage(start, start + 0x3a0);
    fuzz::Rng rng(0x1a57e1);
    ParticleTexture texture;
    texture.file = "fuzz.bmp";
    for (int mutation = 0; mutation < 32; ++mutation) {
        auto bytes = seed;
        auto changed = mutation == 0 ? stage : fuzz::mutate(stage, rng);
        changed.resize(stage.size());
        std::copy(changed.begin(), changed.end(),
                  bytes.begin() + static_cast<std::ptrdiff_t>(7 * kLegacyParticleEffectSize + kStage));
        CHECK(fuzz::run_case("legacy switch effect table", bytes, fuzz::Throws::Never, [&](auto input) {
            const auto effects = parse_legacy_switch_effects(input);
            REQUIRE(effects.size() == kLegacyParticleEffectCount);
            for (const auto& effect : effects)
                for (const auto& s : effect) {
                    if (!s) continue;
                    const auto d = legacy_switch_definition(*s, texture);
                    if (!d) continue;
                    CHECK(d->capacity <= 2000);
                    CHECK(d->ticks_per_second <= 1000.0f);
                    CHECK(std::isfinite(d->angle1));
                    CHECK(std::isfinite(d->angle2));
                    CHECK(std::isfinite(d->gravity));
                    CHECK(std::isfinite(d->scale1));
                    CHECK(std::isfinite(d->motion[3].y));
                }
        }));
    }
}

TEST_CASE("legacy external emitter name exceptions preserve their case and ordering") {
    LegacyParticleSlot s;
    s.sprite = "TFIRE2_TEST";
    s.interval_ms = 40;
    s.scale = 2.0f;
    ParticleTexture texture;
    texture.file = "fixture.bmp";
    auto d = legacy_switch_definition(s, texture);
    REQUIRE(d);
    CHECK(d->lifetime == 0.08f);
    CHECK(d->capacity == 8);
    CHECK(d->shape_size.x == 0.05f);
    CHECK(d->ticks_per_second == doctest::Approx(33.333332f));
    CHECK(d->scale1 == 3.0f);
    s.sprite = "tfire2_test";
    d = legacy_switch_definition(s, texture);
    REQUIRE(d);
    CHECK(d->capacity == 2000);
    CHECK(d->lifetime == 2.0f);
    CHECK(d->ticks_per_second == 25.0f);
    s.sprite = "TFIRE2_I_TSMOKE_TEST";
    d = legacy_switch_definition(s, texture);
    REQUIRE(d);
    CHECK(d->capacity == 8);
    CHECK(d->ticks_per_second == 25.0f);
    CHECK(d->scale1 == 2.0f);
    s.sprite = "lower_i_tsmoke_test";
    d = legacy_switch_definition(s, texture);
    REQUIRE(d);
    CHECK(d->scale1 == 3.0f);
}

TEST_CASE("installed classic waterfall switch effects resolve their global sprites" *
          doctest::skip(!hailstone::test::game_dir())) {
    const auto& game = *hailstone::test::game_dir();
    const auto bytes = game.read("spells.eff");
    REQUIRE(bytes);
    const auto effects = parse_legacy_switch_effects(*bytes);
    const auto path = game.find("gequip.s3d");
    REQUIRE(path);
    const auto archive = PfsArchive::open(*path);
    const auto source = archive.read("gequip.wld");
    REQUIRE(source);
    const auto wld = wld::WldFile::parse(*source);
    for (const std::size_t effect : {132u, 142u, 143u, 144u}) {
        REQUIRE(effects[effect][0]);
        const auto& s = *effects[effect][0];
        const auto* sprite = wld.find(s.sprite);
        REQUIRE(sprite);
        const auto ref = static_cast<wld::Ref>(sprite - wld.fragments().data() + 1);
        const auto texture = particle_texture(wld, ref);
        REQUIRE(texture);
        CHECK(archive.contains(texture->file));
        const auto d = legacy_switch_definition(s, *texture);
        REQUIRE(d);
        CHECK(d->gravity > 0.0f);
        CHECK(d->ticks_per_second == doctest::Approx(1000.0f / static_cast<float>(s.interval_ms)));
        CHECK(d->texture == texture->file);
        CHECK(d->texture_frames == texture->files);
    }
}
