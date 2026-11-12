#include <doctest/doctest.h>

#include "assets/character.h"

using namespace hailstone::assets;

TEST_CASE("character payload accounting includes animation keys and mesh buffers") {
    CharacterModel model;
    const auto empty = model.retained_bytes();
    model.vertices.resize(10);
    model.indices.resize(12);
    CHECK(model.retained_bytes() == empty + 10 * sizeof(SkinnedVertex) + 12 * sizeof(std::uint32_t));
    auto& clip = model.clips["P01"];
    clip.tracks.resize(2);
    const auto before_keys = model.retained_bytes();
    clip.tracks[0].resize(7);
    clip.tracks[1].resize(3);
    CHECK(model.retained_bytes() == before_keys + 10 * sizeof(BonePose));
    CharacterMesh mesh;
    const auto empty_mesh = mesh.retained_bytes();
    mesh.vertices.resize(4);
    mesh.indices.resize(6);
    CHECK(mesh.retained_bytes() == empty_mesh + 4 * sizeof(SkinnedVertex) + 6 * sizeof(std::uint32_t));
}

TEST_CASE("character cache accounting deduplicates shared payloads and counts item copies") {
    CharacterLibrary lib;
    auto shared = std::make_shared<SharedCharacterCache>();
    lib.set_shared_cache(shared);
    auto model = std::make_shared<CharacterModel>();
    model->vertices.resize(11);
    auto mesh = std::make_shared<CharacterMesh>();
    mesh->vertices.resize(13);
    shared->models["AAA"] = {model, {}, 1};
    shared->models["BBB"] = {model, {}, 1};
    shared->dresses[{model.get(), "a"}] = {mesh, {}, 1};
    shared->dresses[{model.get(), "b"}] = {mesh, {}, 1};
    CharacterLibrary::ItemMesh item;
    item.mesh.vertices.resize(17);
    shared->items[1] = {item, {}, 1};
    shared->items[2] = {item, {}, 1};
    const auto stats = lib.cache_memory();
    CHECK(stats.models == 1);
    CHECK(stats.model_bytes == model->retained_bytes());
    CHECK(stats.appearances == 1);
    CHECK(stats.appearance_bytes == mesh->retained_bytes());
    CHECK(stats.items == 2);
    CHECK(stats.item_bytes == 2 * (sizeof(CharacterLibrary::ItemMesh) - sizeof(CharacterMesh) + item.mesh.retained_bytes()));
}
