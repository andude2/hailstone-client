#include "assets/character.h"
#include "assets/eqg_character.h"

#include <functional>
#include <unordered_set>

namespace hailstone::assets {
namespace {

template <class T> std::size_t vector_bytes(const std::vector<T>& v) { return v.size() * sizeof(T); }

std::size_t strings_bytes(const std::vector<std::string>& strings) {
    std::size_t bytes = vector_bytes(strings);
    for (const auto& s : strings) bytes += s.size();
    return bytes;
}

// The Material objects are already in the parent's vector; count only their dynamic payloads here.
std::size_t material_bytes(const std::vector<Material>& materials) {
    std::size_t bytes = vector_bytes(materials);
    for (const auto& m : materials) {
        bytes += m.name.size() + strings_bytes(m.frames) + m.tint_layer.size() + m.detail_palette.size();
        bytes += strings_bytes(m.effect.textures) + m.effect.normal_map.size() + vector_bytes(m.detail_layers);
        for (const auto& layer : m.detail_layers) bytes += layer.texture.size();
    }
    return bytes;
}

std::size_t point_bytes(const std::vector<ModelPoint>& points) {
    std::size_t bytes = vector_bytes(points);
    for (const auto& p : points) bytes += p.name.size() + p.bone.size();
    return bytes;
}

} // namespace

std::size_t CharacterMesh::retained_bytes() const {
    std::size_t bytes = sizeof(*this) + vector_bytes(vertices) + vector_bytes(indices) + vector_bytes(parts) +
                        material_bytes(materials) + vector_bytes(tints) + vector_bytes(emitters);
    for (const auto& e : emitters) bytes += e.emitter.point.size();
    return bytes;
}

std::size_t CharacterModel::retained_bytes() const {
    std::size_t bytes = sizeof(*this) + code.size() + source.size() + animation_donor.size() + strings_bytes(bone_names) +
                        vector_bytes(parents) + vector_bytes(order) + vector_bytes(vertices) + vector_bytes(indices) +
                        vector_bytes(parts) + material_bytes(materials) + vector_bytes(inverse_bind) + point_bytes(points) +
                        vector_bytes(emitters);
    for (const auto& e : emitters) bytes += e.point.size();
    for (const auto& [name, clip] : clips) {
        bytes += sizeof(decltype(clips)::value_type) + name.size() + vector_bytes(clip.tracks);
        for (const auto& track : clip.tracks) bytes += vector_bytes(track);
    }
    for (const auto& [name, alias] : clip_aliases)
        bytes += sizeof(decltype(clip_aliases)::value_type) + name.size() + alias.size();
    for (const auto& [name, alias] : held_aliases)
        bytes += sizeof(decltype(held_aliases)::value_type) + name.size() + alias.size();
    return bytes;
}

CharacterLibrary::CacheMemory CharacterLibrary::cache_memory() const {
    // Libraries made by CharacterArchiveCache share this mutex. Standalone libraries have their own and are
    // reached in the same parent -> child order as model()/dress(); diagnostics never change the caches.
    CacheMemory out;
    std::unordered_set<const CharacterModel*> models;
    std::unordered_set<const CharacterMesh*> appearances;
    std::unordered_set<const ItemMesh*> items;
    std::unordered_set<const CharacterLibrary*> libraries;
    std::unordered_set<const EqgCharacterStore*> eqg;
    auto add_model = [&](const auto& model) {
        if (!model || !models.insert(model.get()).second) return;
        ++out.models;
        out.model_bytes += model->retained_bytes();
    };
    auto add_appearance = [&](const auto& mesh) {
        if (!mesh || !appearances.insert(mesh.get()).second) return;
        ++out.appearances;
        out.appearance_bytes += mesh->retained_bytes();
    };
    auto add_item = [&](const auto& item) {
        if (!item || !items.insert(&*item).second) return;
        ++out.items;
        out.item_bytes += sizeof(ItemMesh) - sizeof(CharacterMesh) + item->mesh.retained_bytes() +
                          point_bytes(item->points) + vector_bytes(item->emitters);
        for (const auto& e : item->emitters) out.item_bytes += e.point.size();
    };
    std::function<void(const CharacterLibrary&)> visit = [&](const CharacterLibrary& lib) {
        if (!libraries.insert(&lib).second) return;
        Lock lock(*lib.mutex_);
        for (const auto& kv : lib.cache_) add_model(kv.second.value);
        for (const auto& kv : lib.dress_cache_) add_appearance(kv.second.value);
        for (const auto& kv : lib.item_cache_) add_item(kv.second.value);
        if (lib.shared_) {
            for (const auto& kv : lib.shared_->models) add_model(kv.second.value);
            for (const auto& kv : lib.shared_->dresses) add_appearance(kv.second.value);
            for (const auto& kv : lib.shared_->items) add_item(kv.second.value);
        }
        if (lib.eqg_ && eqg.insert(lib.eqg_.get()).second)
            for (const auto& kv : lib.eqg_->models_) add_model(kv.second);
        if (lib.own_)
            for (const auto& kv : lib.own_->libraries_)
                if (kv.second) visit(*kv.second);
    };
    visit(*this);
    return out;
}

} // namespace hailstone::assets
