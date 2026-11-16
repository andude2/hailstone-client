#include <doctest/doctest.h>

#include "assets/character.h"
#include "assets/object_animation.h"

using namespace hailstone::assets;

TEST_CASE("character cloud definitions share storage across bones and dressed meshes") {
    CharacterModel model;
    auto cloud = std::make_shared<EmitterDefinition>();
    cloud->name = "cloud";
    cloud->texture = "cloud.dds";
    cloud->texture_frames = {"cloud.dds", "next.dds"};
    const auto before = model.retained_bytes();
    model.clouds = {{0, glm::mat4(1.0f), {}, "TST", cloud}, {1, glm::mat4(1.0f), {}, "TST", cloud}};
    const auto definition_bytes = sizeof(EmitterDefinition) + cloud->name.size() + cloud->texture.size() +
                                  2 * sizeof(std::string) + cloud->texture_frames[0].size() + cloud->texture_frames[1].size();
    CHECK(model.retained_bytes() == before + 2 * sizeof(MeshEmitter) + 6 + definition_bytes);
    auto mesh = model.default_mesh();
    std::unordered_set<const EmitterDefinition*> shared;
    const auto shared_model = model.memory_breakdown(nullptr, &shared).total();
    CHECK(shared_model == model.retained_bytes());
    CHECK(mesh.retained_bytes(&shared) + definition_bytes == mesh.retained_bytes());
    auto animation = std::make_shared<ObjectAnimation>();
    animation->nodes.resize(2);
    animation->rest_inverse.resize(2);
    animation->nodes[0].keys.resize(3);
    const auto animation_bytes = sizeof(ObjectAnimation) + 2 * sizeof(ObjectAnimationNode) + 2 * sizeof(glm::mat4) +
                                 3 * sizeof(ObjectTrackKey);
    const auto before_animation = model.retained_bytes();
    model.clouds[0].cloud_animation = model.clouds[1].cloud_animation = animation;
    CHECK(model.retained_bytes() == before_animation + animation_bytes);
    std::unordered_set<const EmitterDefinition*> definitions;
    std::unordered_set<const ObjectAnimation*> animations;
    model.memory_breakdown(nullptr, &definitions, &animations);
    auto animated_mesh = model.default_mesh();
    CHECK(animated_mesh.retained_bytes(&definitions, &animations) + definition_bytes + animation_bytes ==
          animated_mesh.retained_bytes());
}

TEST_CASE("character payload accounting includes animation keys and mesh buffers") {
    CharacterModel model;
    const auto empty = model.retained_bytes();
    CHECK(model.memory_breakdown().animation_bytes == 0);
    CHECK(model.memory_breakdown().geometry_bytes == 0);
    CHECK(model.memory_breakdown().other_bytes == empty);
    model.vertices.resize(10);
    model.indices.resize(12);
    CHECK(model.retained_bytes() == empty + 10 * sizeof(SkinnedVertex) + 12 * sizeof(std::uint32_t));
    CHECK(model.memory_breakdown().geometry_bytes == 10 * sizeof(SkinnedVertex) + 12 * sizeof(std::uint32_t));
    auto& clip = model.clips["P01"];
    clip.tracks.resize(2);
    const auto before_keys = model.retained_bytes();
    const auto before_animation = model.memory_breakdown().animation_bytes;
    clip.tracks[0].resize(7);
    clip.tracks[1].resize(3);
    CHECK(model.retained_bytes() == before_keys + 10 * sizeof(BonePose) + 2 * sizeof(std::vector<BonePose>));
    const auto breakdown = model.memory_breakdown();
    CHECK(breakdown.animation_bytes == before_animation + 10 * sizeof(BonePose) + 2 * sizeof(std::vector<BonePose>));
    CHECK(breakdown.geometry_bytes == 10 * sizeof(SkinnedVertex) + 12 * sizeof(std::uint32_t));
    CHECK(breakdown.other_bytes == empty);
    CHECK(breakdown.total() == model.retained_bytes());
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
    model->clips["P01"].tracks.resize(1);
    model->clips["P01"].tracks[0].resize(6);
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
    CHECK(stats.model_animation_bytes == model->memory_breakdown().animation_bytes);
    CHECK(stats.model_geometry_bytes == model->memory_breakdown().geometry_bytes);
    CHECK(stats.model_other_bytes == model->memory_breakdown().other_bytes);
    CHECK(stats.model_bytes == stats.model_animation_bytes + stats.model_geometry_bytes + stats.model_other_bytes);
    CHECK(stats.appearances == 1);
    CHECK(stats.appearance_bytes == mesh->retained_bytes());
    CHECK(stats.items == 2);
    CHECK(stats.item_bytes == 2 * (sizeof(CharacterLibrary::ItemMesh) - sizeof(CharacterMesh) + item.mesh.retained_bytes()));
}

TEST_CASE("model memory categories include skeletons, aliases and dynamic materials") {
    CharacterModel model;
    model.bone_names = {"ROOT"};
    model.parents = {-1};
    model.order = {0};
    model.inverse_bind.resize(1);
    const auto skeleton = model.memory_breakdown();
    CHECK(skeleton.animation_bytes == sizeof(std::string) + 4 + sizeof(std::int32_t) +
                                     sizeof(std::uint32_t) + sizeof(glm::mat4));
    model.clip_aliases["C01"] = "ATTACK";
    model.held_aliases["P01"] = "P01B";
    const auto aliases = model.memory_breakdown();
    CHECK(aliases.animation_bytes > skeleton.animation_bytes);
    CHECK(aliases.geometry_bytes == 0);
    model.materials.resize(1);
    model.materials[0].frames = {"skin.dds"};
    const auto material = model.memory_breakdown();
    CHECK(material.geometry_bytes == sizeof(Material) + sizeof(std::string) + 8);
    CHECK(material.animation_bytes == aliases.animation_bytes);
    model.code = "HUM";
    const auto named = model.memory_breakdown();
    CHECK(named.other_bytes == material.other_bytes + 3);
    CHECK(named.total() == model.retained_bytes());
    model.materials[0].effect.coverage_map = "coverage.dds";
    CHECK(model.memory_breakdown().geometry_bytes == named.geometry_bytes + 12);
    CHECK(model.retained_bytes() == named.total() + 12);
}
