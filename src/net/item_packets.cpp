// Written from EQEmu rof2.cpp / rof2_structs.h (read as reference, no code
// copied) and the original client's item handling. OpenEQ crates/openeq-net/src/inventory.rs
// was studied to cross-check the block sizes (THIRD_PARTY.md).

#include "net/item_packets.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "net/opcodes.h"
#include "net/packet_io.h"

namespace hailstone::net {

using detail::decode_with;
using detail::write_cstring;

namespace {

constexpr int kMaxItemDepth = 4;          // a bag's content is depth 1; an item's augments are its children
                                          // too, each with its socket as the index (child i = socket i)
constexpr std::uint32_t kMaxContents = kMaxBagContents;
constexpr std::uint32_t kMaxInventoryItems = 4096;
// Fewer bytes than any serialized item takes (an empty item is several hundred: its fixed fields and the empty
// strings' terminators; item_packets_test checks this stays below it). The vectors reserve no more entries than the
// bytes left could hold, so a few-byte packet that claims thousands of items does not allocate megabytes first.
constexpr std::size_t kMinItemBytes = 64;
std::size_t reserve_for(std::uint32_t count, const ByteReader& r, std::size_t per_entry) {
    return std::min<std::size_t>(count, r.remaining() / per_entry);
}

double read_f64(ByteReader& r) {
    std::uint64_t bits = r.le<std::uint64_t>();
    double d;
    std::memcpy(&d, &bits, sizeof d);
    return d;
}
void write_f64(ByteWriter& w, double d) {
    std::uint64_t bits;
    std::memcpy(&bits, &d, sizeof bits);
    w.le(bits);
}

template <typename T> void get(ByteReader& r, T& v) { v = r.le<T>(); }
void get(ByteReader& r, float& v) { v = r.f32(); }
template <typename T> void put(ByteWriter& w, T v) { w.le(v); }
void put(ByteWriter& w, float v) { w.f32(v); }

void read_effect(ByteReader& r, ItemEffect& e) {
    get(r, e.effect);
    get(r, e.level2);
    get(r, e.type);
    get(r, e.level);
    for (auto& x : e.extra) get(r, x);
    e.name = r.cstring();
    get(r, e.trailer);
}

void write_effect(ByteWriter& w, const ItemEffect& e) {
    put(w, e.effect);
    put(w, e.level2);
    put(w, e.type);
    put(w, e.level);
    for (auto x : e.extra) put(w, x);
    write_cstring(w, e.name);
    put(w, e.trailer);
}

Item read_item_at(ByteReader& r, int depth) {
    if (depth > kMaxItemDepth) throw std::out_of_range("item nesting too deep");
    Item it;
    // ItemSerializationHeader
    it.serial = r.fixed_string(17);
    get(r, it.stack_count);
    get(r, it.unknown004);
    it.slot.type = static_cast<std::int16_t>(r.le<std::uint8_t>());
    it.slot.slot = r.le<std::int16_t>();
    it.slot.sub = r.le<std::int16_t>();
    it.slot.aug = r.le<std::int16_t>();
    get(r, it.price);
    get(r, it.merchant_slot);
    get(r, it.scaled_value);
    get(r, it.instance_id);
    get(r, it.parcel_item_id);
    get(r, it.last_cast_time);
    get(r, it.charges);
    get(r, it.attuned);
    get(r, it.unknown044);
    get(r, it.unknown048);
    get(r, it.unknown052);
    get(r, it.is_evolving);
    if (it.is_evolving) {
        auto& e = it.evolving;
        get(r, e.final_item_id);
        get(r, e.evolve_level);
        e.progress = read_f64(r);
        get(r, e.activated);
        get(r, e.evolve_max_level);
        for (auto& b : e.unknown005) get(r, b);
    }
    it.ornament_main = r.cstring();
    it.ornament_off = r.cstring();
    // ItemSerializationHeaderFinish
    get(r, it.ornament_icon);
    get(r, it.unknowna1);
    get(r, it.ornament_hero_model);
    get(r, it.unknown063);
    get(r, it.copied);
    get(r, it.unknowna4);
    get(r, it.unknowna5);
    get(r, it.item_class);
    // SerializeItem writes the name only when it is not empty (no terminator
    // either); every real item has one.
    it.name = r.cstring();
    it.lore = r.cstring();
    it.id_file = r.cstring();
    it.id_file2 = r.cstring();
    // ItemBodyStruct
    get(r, it.id);
    get(r, it.weight);
    get(r, it.no_rent);
    get(r, it.no_drop);
    get(r, it.attuneable);
    get(r, it.size);
    get(r, it.equip_slots);
    get(r, it.value);
    get(r, it.icon);
    get(r, it.unknown1);
    get(r, it.unknown2);
    get(r, it.benefit_flag);
    get(r, it.tradeskills);
    for (std::int8_t* v : {&it.cr, &it.dr, &it.pr, &it.mr, &it.fr, &it.sv_corruption, &it.astr, &it.asta, &it.aagi,
                           &it.adex, &it.acha, &it.aint, &it.awis})
        get(r, *v);
    get(r, it.hp);
    get(r, it.mana);
    get(r, it.endurance);
    get(r, it.ac);
    get(r, it.regen);
    get(r, it.mana_regen);
    get(r, it.end_regen);
    get(r, it.classes);
    get(r, it.races);
    get(r, it.deity);
    get(r, it.skill_mod_value);
    get(r, it.skill_mod_max);
    get(r, it.skill_mod_type);
    get(r, it.skill_mod_extra);
    get(r, it.bane_dmg_race);
    get(r, it.bane_dmg_body);
    get(r, it.bane_dmg_race_amt);
    get(r, it.bane_dmg_amt);
    get(r, it.magic);
    get(r, it.cast_time_);
    get(r, it.req_level);
    get(r, it.rec_level);
    get(r, it.rec_skill);
    get(r, it.bard_type);
    get(r, it.bard_value);
    get(r, it.light);
    get(r, it.delay);
    get(r, it.elem_dmg_type);
    get(r, it.elem_dmg_amt);
    get(r, it.range);
    get(r, it.damage);
    get(r, it.color);
    get(r, it.prestige);
    get(r, it.item_type);
    get(r, it.material);
    get(r, it.material_unknown1);
    get(r, it.elite_material);
    get(r, it.heros_forge_model);
    get(r, it.material_unknown2);
    get(r, it.sell_rate);
    for (std::int32_t* v : {&it.combat_effects, &it.shielding, &it.stun_resist, &it.strike_through,
                            &it.extra_dmg_skill, &it.extra_dmg_amt, &it.spell_shield, &it.avoidance, &it.accuracy})
        get(r, *v);
    get(r, it.charm_file_id);
    for (int i = 0; i < 4; ++i) {
        get(r, it.faction_mod[static_cast<std::size_t>(i)]);
        get(r, it.faction_amt[static_cast<std::size_t>(i)]);
    }
    it.charm_file = r.cstring();
    // ItemSecondaryBodyStruct
    get(r, it.aug_type);
    get(r, it.aug_restrict2);
    get(r, it.aug_restrict);
    for (auto& s : it.aug_sockets) {
        get(r, s.type);
        get(r, s.visible);
        get(r, s.unknown);
    }
    get(r, it.ldon_point_type);
    get(r, it.ldon_theme);
    get(r, it.ldon_price);
    get(r, it.ldon_sellback_rate);
    get(r, it.ldon_sold);
    get(r, it.bag_type);
    get(r, it.bag_slots);
    get(r, it.bag_size);
    get(r, it.weight_reduction);
    get(r, it.book);
    get(r, it.book_type);
    it.book_file = r.cstring();
    // ItemTertiaryBodyStruct
    get(r, it.lore_group);
    get(r, it.artifact);
    get(r, it.summoned);
    get(r, it.favor);
    get(r, it.fv_no_drop);
    get(r, it.dot_shield);
    get(r, it.attack);
    get(r, it.haste);
    get(r, it.damage_shield);
    get(r, it.guild_favor);
    get(r, it.aug_distiller);
    get(r, it.unknown3);
    get(r, it.unknown4);
    get(r, it.no_pet);
    get(r, it.unknown5);
    get(r, it.potion_belt_enabled);
    get(r, it.potion_belt_slots);
    get(r, it.max_stack);
    get(r, it.no_transfer);
    get(r, it.expendable_arrow);
    for (auto& v : it.unknown8_11) get(r, v);
    for (auto& v : it.unknown12_14) get(r, v);
    // Click, proc, worn, focus, scroll, bard.
    for (auto& e : it.effects) read_effect(r, e);
    // ItemQuaternaryBodyStruct
    get(r, it.script_file_id);
    get(r, it.quest_item);
    get(r, it.power);
    get(r, it.purity);
    get(r, it.unknown16);
    get(r, it.backstab_dmg);
    get(r, it.ds_mitigation);
    for (std::int32_t* v : {&it.heroic_str, &it.heroic_int, &it.heroic_wis, &it.heroic_agi, &it.heroic_dex,
                            &it.heroic_sta, &it.heroic_cha, &it.heroic_mr, &it.heroic_fr, &it.heroic_cr, &it.heroic_dr,
                            &it.heroic_pr, &it.heroic_sv_corrup, &it.heal_amt, &it.spell_dmg, &it.clairvoyance,
                            &it.sub_type})
        get(r, *v);
    get(r, it.evolve_string);
    get(r, it.unknown19);
    get(r, it.unknown20);
    get(r, it.unknown21);
    get(r, it.heirloom);
    get(r, it.placeable);
    for (auto& v : it.unknown22) get(r, v);
    for (auto& v : it.unknown23_25) get(r, v);
    get(r, it.unknown26);
    get(r, it.unknown27);
    get(r, it.unknown_rof_6);
    get(r, it.unknown28);
    get(r, it.unknown29);
    get(r, it.unknown30);
    get(r, it.unknown31);
    get(r, it.unknown32);
    get(r, it.unknown33);
    for (auto& v : it.unknown34_37) get(r, v);
    get(r, it.no_zone);
    for (auto& v : it.unknown_rof_7) get(r, v);
    get(r, it.no_ground);
    for (auto& v : it.unknown_rof_8) get(r, v);
    get(r, it.unknown37a);
    get(r, it.unknown38);
    get(r, it.unknown39);
    // Contents.
    std::uint32_t count = r.le<std::uint32_t>();
    if (count > kMaxContents) throw std::out_of_range("too many bag contents");
    it.contents.reserve(reserve_for(count, r, 4 + kMinItemBytes)); // each entry: its u32 index and the item
    for (std::uint32_t i = 0; i < count; ++i) {
        BagItem b;
        b.index = r.le<std::uint32_t>();
        if (b.index >= kMaxContents) throw std::out_of_range("bag index out of range");
        b.item = read_item_at(r, depth + 1);
        it.contents.push_back(std::move(b));
    }
    return it;
}

} // namespace

// ---------------------------------------------------------------- slots

void write_slot(ByteWriter& w, const ItemSlot& s) {
    w.le(s.type);
    w.le(s.unknown02);
    w.le(s.slot);
    w.le(s.sub);
    w.le(s.aug);
    w.le(s.unknown01);
}

ItemSlot read_slot(ByteReader& r) {
    ItemSlot s;
    s.type = r.le<std::int16_t>();
    s.unknown02 = r.le<std::int16_t>();
    s.slot = r.le<std::int16_t>();
    s.sub = r.le<std::int16_t>();
    s.aug = r.le<std::int16_t>();
    s.unknown01 = r.le<std::int16_t>();
    return s;
}

const char* possessions_slot_name(int slot) {
    static constexpr const char* kNames[] = {
        "Charm", "Left Ear", "Head", "Face", "Right Ear", "Neck", "Shoulders", "Arms", "Back", "Left Wrist",
        "Right Wrist", "Range", "Hands", "Primary", "Secondary", "Left Finger", "Right Finger", "Chest", "Legs",
        "Feet", "Waist", "Power Source", "Ammo", "General 1", "General 2", "General 3", "General 4", "General 5",
        "General 6", "General 7", "General 8", "General 9", "General 10", "Cursor"};
    return slot >= 0 && slot < static_cast<int>(std::size(kNames)) ? kNames[slot] : "?";
}

// ---------------------------------------------------------------- items

const Item* Item::content(std::uint32_t index) const {
    for (const auto& b : contents)
        if (b.index == index) return &b.item;
    return nullptr;
}

Item* Item::content(std::uint32_t index) {
    for (auto& b : contents)
        if (b.index == index) return &b.item;
    return nullptr;
}

Item read_item(ByteReader& r) { return read_item_at(r, 0); }

void write_item(ByteWriter& w, const Item& it) {
    w.fixed_string(it.serial, 17);
    put(w, it.stack_count);
    put(w, it.unknown004);
    put(w, static_cast<std::uint8_t>(it.slot.type));
    put(w, it.slot.slot);
    put(w, it.slot.sub);
    put(w, it.slot.aug);
    put(w, it.price);
    put(w, it.merchant_slot);
    put(w, it.scaled_value);
    put(w, it.instance_id);
    put(w, it.parcel_item_id);
    put(w, it.last_cast_time);
    put(w, it.charges);
    put(w, it.attuned);
    put(w, it.unknown044);
    put(w, it.unknown048);
    put(w, it.unknown052);
    put(w, it.is_evolving);
    if (it.is_evolving) {
        const auto& e = it.evolving;
        put(w, e.final_item_id);
        put(w, e.evolve_level);
        write_f64(w, e.progress);
        put(w, e.activated);
        put(w, e.evolve_max_level);
        for (auto b : e.unknown005) put(w, b);
    }
    write_cstring(w, it.ornament_main);
    write_cstring(w, it.ornament_off);
    put(w, it.ornament_icon);
    put(w, it.unknowna1);
    put(w, it.ornament_hero_model);
    put(w, it.unknown063);
    put(w, it.copied);
    put(w, it.unknowna4);
    put(w, it.unknowna5);
    put(w, it.item_class);
    write_cstring(w, it.name);
    write_cstring(w, it.lore);
    write_cstring(w, it.id_file);
    write_cstring(w, it.id_file2);
    put(w, it.id);
    put(w, it.weight);
    put(w, it.no_rent);
    put(w, it.no_drop);
    put(w, it.attuneable);
    put(w, it.size);
    put(w, it.equip_slots);
    put(w, it.value);
    put(w, it.icon);
    put(w, it.unknown1);
    put(w, it.unknown2);
    put(w, it.benefit_flag);
    put(w, it.tradeskills);
    for (std::int8_t v : {it.cr, it.dr, it.pr, it.mr, it.fr, it.sv_corruption, it.astr, it.asta, it.aagi, it.adex,
                          it.acha, it.aint, it.awis})
        put(w, v);
    put(w, it.hp);
    put(w, it.mana);
    put(w, it.endurance);
    put(w, it.ac);
    put(w, it.regen);
    put(w, it.mana_regen);
    put(w, it.end_regen);
    put(w, it.classes);
    put(w, it.races);
    put(w, it.deity);
    put(w, it.skill_mod_value);
    put(w, it.skill_mod_max);
    put(w, it.skill_mod_type);
    put(w, it.skill_mod_extra);
    put(w, it.bane_dmg_race);
    put(w, it.bane_dmg_body);
    put(w, it.bane_dmg_race_amt);
    put(w, it.bane_dmg_amt);
    put(w, it.magic);
    put(w, it.cast_time_);
    put(w, it.req_level);
    put(w, it.rec_level);
    put(w, it.rec_skill);
    put(w, it.bard_type);
    put(w, it.bard_value);
    put(w, it.light);
    put(w, it.delay);
    put(w, it.elem_dmg_type);
    put(w, it.elem_dmg_amt);
    put(w, it.range);
    put(w, it.damage);
    put(w, it.color);
    put(w, it.prestige);
    put(w, it.item_type);
    put(w, it.material);
    put(w, it.material_unknown1);
    put(w, it.elite_material);
    put(w, it.heros_forge_model);
    put(w, it.material_unknown2);
    put(w, it.sell_rate);
    for (std::int32_t v : {it.combat_effects, it.shielding, it.stun_resist, it.strike_through, it.extra_dmg_skill,
                           it.extra_dmg_amt, it.spell_shield, it.avoidance, it.accuracy})
        put(w, v);
    put(w, it.charm_file_id);
    for (std::size_t i = 0; i < 4; ++i) {
        put(w, it.faction_mod[i]);
        put(w, it.faction_amt[i]);
    }
    write_cstring(w, it.charm_file);
    put(w, it.aug_type);
    put(w, it.aug_restrict2);
    put(w, it.aug_restrict);
    for (const auto& s : it.aug_sockets) {
        put(w, s.type);
        put(w, s.visible);
        put(w, s.unknown);
    }
    put(w, it.ldon_point_type);
    put(w, it.ldon_theme);
    put(w, it.ldon_price);
    put(w, it.ldon_sellback_rate);
    put(w, it.ldon_sold);
    put(w, it.bag_type);
    put(w, it.bag_slots);
    put(w, it.bag_size);
    put(w, it.weight_reduction);
    put(w, it.book);
    put(w, it.book_type);
    write_cstring(w, it.book_file);
    put(w, it.lore_group);
    put(w, it.artifact);
    put(w, it.summoned);
    put(w, it.favor);
    put(w, it.fv_no_drop);
    put(w, it.dot_shield);
    put(w, it.attack);
    put(w, it.haste);
    put(w, it.damage_shield);
    put(w, it.guild_favor);
    put(w, it.aug_distiller);
    put(w, it.unknown3);
    put(w, it.unknown4);
    put(w, it.no_pet);
    put(w, it.unknown5);
    put(w, it.potion_belt_enabled);
    put(w, it.potion_belt_slots);
    put(w, it.max_stack);
    put(w, it.no_transfer);
    put(w, it.expendable_arrow);
    for (auto v : it.unknown8_11) put(w, v);
    for (auto v : it.unknown12_14) put(w, v);
    for (const auto& e : it.effects) write_effect(w, e);
    put(w, it.script_file_id);
    put(w, it.quest_item);
    put(w, it.power);
    put(w, it.purity);
    put(w, it.unknown16);
    put(w, it.backstab_dmg);
    put(w, it.ds_mitigation);
    for (std::int32_t v : {it.heroic_str, it.heroic_int, it.heroic_wis, it.heroic_agi, it.heroic_dex, it.heroic_sta,
                           it.heroic_cha, it.heroic_mr, it.heroic_fr, it.heroic_cr, it.heroic_dr, it.heroic_pr,
                           it.heroic_sv_corrup, it.heal_amt, it.spell_dmg, it.clairvoyance, it.sub_type})
        put(w, v);
    put(w, it.evolve_string);
    put(w, it.unknown19);
    put(w, it.unknown20);
    put(w, it.unknown21);
    put(w, it.heirloom);
    put(w, it.placeable);
    for (auto v : it.unknown22) put(w, v);
    for (auto v : it.unknown23_25) put(w, v);
    put(w, it.unknown26);
    put(w, it.unknown27);
    put(w, it.unknown_rof_6);
    put(w, it.unknown28);
    put(w, it.unknown29);
    put(w, it.unknown30);
    put(w, it.unknown31);
    put(w, it.unknown32);
    put(w, it.unknown33);
    for (auto v : it.unknown34_37) put(w, v);
    put(w, it.no_zone);
    for (auto v : it.unknown_rof_7) put(w, v);
    put(w, it.no_ground);
    for (auto v : it.unknown_rof_8) put(w, v);
    put(w, it.unknown37a);
    put(w, it.unknown38);
    put(w, it.unknown39);
    put(w, static_cast<std::uint32_t>(it.contents.size()));
    for (const auto& b : it.contents) {
        put(w, b.index);
        write_item(w, b.item);
    }
}

std::optional<std::vector<Item>> decode_char_inventory(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) -> std::vector<Item> {
        std::uint32_t count = r.le<std::uint32_t>();
        if (count > kMaxInventoryItems) throw std::out_of_range("too many items");
        std::vector<Item> items;
        items.reserve(reserve_for(count, r, kMinItemBytes));
        for (std::uint32_t i = 0; i < count; ++i) items.push_back(read_item(r));
        if (!r.empty()) throw std::out_of_range("trailing bytes after the inventory");
        return items;
    });
}

AppPacket encode_char_inventory(const std::vector<Item>& items) {
    ByteWriter w;
    w.le(static_cast<std::uint32_t>(items.size()));
    for (const auto& it : items) write_item(w, it);
    return make_packet(Op::CharInventory, std::move(w));
}

std::optional<ItemPacket> decode_item_packet(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        ItemPacket v;
        v.type = r.le<std::uint32_t>();
        v.item = read_item(r);
        if (v.type == kItemPacketParcel) {
            v.parcel_sent_time = r.le<std::uint32_t>();
            auto n = r.le<std::uint32_t>();
            auto s = r.bytes(n);
            v.parcel_from.assign(reinterpret_cast<const char*>(s.data()), s.size());
            n = r.le<std::uint32_t>();
            s = r.bytes(n);
            v.parcel_note.assign(reinterpret_cast<const char*>(s.data()), s.size());
        }
        if (!r.empty()) throw std::out_of_range("trailing bytes after the item");
        return v;
    });
}

AppPacket encode_item_packet(bool link_response, const ItemPacket& v) {
    ByteWriter w;
    w.le(v.type);
    write_item(w, v.item);
    if (v.type == kItemPacketParcel) {
        w.le(v.parcel_sent_time);
        w.le(static_cast<std::uint32_t>(v.parcel_from.size()));
        w.bytes({reinterpret_cast<const std::byte*>(v.parcel_from.data()), v.parcel_from.size()});
        w.le(static_cast<std::uint32_t>(v.parcel_note.size()));
        w.bytes({reinterpret_cast<const std::byte*>(v.parcel_note.data()), v.parcel_note.size()});
    }
    return make_packet(link_response ? Op::ItemLinkResponse : Op::ItemPacket, std::move(w));
}

// ---------------------------------------------------------------- moves

namespace {
ByteWriter write_move(const MoveItem& v) {
    ByteWriter w;
    write_slot(w, v.from);
    write_slot(w, v.to);
    w.le(v.count);
    return w;
}
} // namespace

AppPacket encode_item_preview_request(std::uint32_t item_id, const Item* item) {
    ByteWriter w;
    w.le(item_id);
    const bool ornamented = item && (!item->ornament_main.empty() || !item->ornament_off.empty() ||
                                     item->ornament_icon != 0 || item->unknowna1 >= 0 ||
                                     item->ornament_hero_model != 0 || item->unknown063 != 0);
    auto text = [&](const std::string& s) {
        std::array<std::byte, 30> b{};
        if (ornamented)
            for (std::size_t k = 0; k < s.size() && k < 29; ++k) b[k] = static_cast<std::byte>(s[k]); // steve_strncpy
        w.bytes(b);
    };
    text(ornamented ? item->ornament_main : std::string());
    text(ornamented ? item->ornament_off : std::string());
    w.le(ornamented ? item->unknowna1 : -1);
    w.le(ornamented ? item->ornament_hero_model : 0u);
    w.le(ornamented ? static_cast<std::uint32_t>(item->unknown063) : 0u);
    w.le(std::uint8_t{0});
    w.le(static_cast<std::uint8_t>(ornamented ? 1 : 0));
    w.le(std::uint16_t{0});
    return make_packet(Op::ItemPreviewRequest, std::move(w));
}

std::optional<ItemPreviewReply> decode_item_preview(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        ItemPreviewReply v;
        v.item = read_item(r);
        if (!r.empty()) v.trailing = r.le<std::uint8_t>();
        return v;
    });
}

AppPacket encode(const ItemPreviewReply& v) {
    ByteWriter w;
    write_item(w, v.item);
    w.le(v.trailing);
    return make_packet(Op::ItemPreviewRequest, std::move(w));
}

AppPacket encode(const MoveItem& v) { return make_packet(Op::MoveItem, write_move(v)); }
AppPacket encode_delete_item(const MoveItem& v) { return make_packet(Op::DeleteItem, write_move(v)); }

std::optional<MoveItem> decode_move_item(const AppPacket& p) {
    if (p.data.size() != kMoveItemSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        MoveItem v;
        v.from = read_slot(r);
        v.to = read_slot(r);
        v.count = r.le<std::uint32_t>();
        return v;
    });
}

std::optional<AppPacket> encode_move_multiple(const std::vector<MultiMoveEntry>& moves) {
    if (moves.empty()) return std::nullopt;
    ByteWriter w;
    w.le(static_cast<std::uint32_t>(moves.size()));
    for (const MultiMoveEntry& m : moves) {
        write_slot(w, m.from);
        write_slot(w, m.to);
        w.le(m.count);
        w.le(m.flags);
        w.le(std::uint32_t{0});
    }
    return make_packet(Op::MoveMultipleItems, std::move(w));
}

// ---------------------------------------------------------------- loot

AppPacket encode_loot_request(std::uint32_t corpse_id) {
    ByteWriter w;
    w.le(corpse_id);
    return make_packet(Op::LootRequest, std::move(w));
}

AppPacket encode_end_loot_request(std::uint32_t corpse_id) {
    ByteWriter w;
    w.le(corpse_id);
    return make_packet(Op::EndLootRequest, std::move(w));
}

std::optional<std::uint32_t> decode_loot_request(const AppPacket& p) {
    if (p.data.size() != 4) return std::nullopt;
    return decode_with(p, [](ByteReader& r) { return r.le<std::uint32_t>(); });
}

AppPacket encode(const LootItem& v) {
    ByteWriter w;
    w.le(v.lootee);
    w.le(v.looter);
    w.le(v.slot);
    w.le(v.unknown10);
    w.le(v.auto_loot);
    w.le(v.unknown16);
    return make_packet(Op::LootItem, std::move(w));
}

std::optional<LootItem> decode_loot_item(const AppPacket& p) {
    if (p.data.size() != kLootItemSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        LootItem v;
        v.lootee = r.le<std::uint32_t>();
        v.looter = r.le<std::uint32_t>();
        v.slot = r.le<std::uint16_t>();
        v.unknown10 = r.le<std::uint16_t>();
        v.auto_loot = r.le<std::int32_t>();
        v.unknown16 = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode(const MoneyOnCorpse& v) {
    ByteWriter w;
    w.le(v.response);
    w.le(v.unknown1);
    w.le(v.unknown2);
    w.le(v.unknown3);
    w.le(v.platinum);
    w.le(v.gold);
    w.le(v.silver);
    w.le(v.copper);
    return make_packet(Op::MoneyOnCorpse, std::move(w));
}

std::optional<MoneyOnCorpse> decode_money_on_corpse(const AppPacket& p) {
    if (p.data.size() != kMoneyOnCorpseSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        MoneyOnCorpse v;
        v.response = r.le<std::uint8_t>();
        v.unknown1 = r.le<std::uint8_t>();
        v.unknown2 = r.le<std::uint8_t>();
        v.unknown3 = r.le<std::uint8_t>();
        v.platinum = r.le<std::uint32_t>();
        v.gold = r.le<std::uint32_t>();
        v.silver = r.le<std::uint32_t>();
        v.copper = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode_split_coin(const Coins& v) {
    ByteWriter w;
    for (std::uint32_t c : {v.platinum, v.gold, v.silver, v.copper}) w.le(c);
    return AppPacket(kOpSplitCoin, w.take());
}

std::optional<Coins> decode_split_coin(const AppPacket& p) {
    if (p.opcode != kOpSplitCoin || p.data.size() < 16) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        Coins v;
        v.platinum = r.le<std::uint32_t>();
        v.gold = r.le<std::uint32_t>();
        v.silver = r.le<std::uint32_t>();
        v.copper = r.le<std::uint32_t>();
        return v;
    });
}

std::optional<Coins> decode_money_update(const AppPacket& p) {
    if (p.data.size() != 16) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        Coins v;
        v.platinum = r.le<std::uint32_t>();
        v.gold = r.le<std::uint32_t>();
        v.silver = r.le<std::uint32_t>();
        v.copper = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode_money_update(const Coins& v) {
    ByteWriter w;
    w.le(v.platinum);
    w.le(v.gold);
    w.le(v.silver);
    w.le(v.copper);
    return make_packet(Op::MoneyUpdate, std::move(w));
}

// ---------------------------------------------------------------- item links

AppPacket encode(const ItemLinkClick& v) {
    ByteWriter w;
    w.le(v.item_id);
    for (auto a : v.augments) w.le(a);
    w.le(v.hash);
    w.le(v.flags);
    w.le(v.is_evolving);
    w.le(v.evolve_group);
    w.le(v.evolve_level);
    w.le(v.ornament_icon);
    return make_packet(Op::ItemLinkClick, std::move(w));
}

AppPacket encode(const ItemLinkRequest& v) {
    ByteWriter w;
    w.le(v.item_id);
    w.le(v.flags);
    w.fixed_string(v.name.substr(0, 63), 64);
    return make_packet(Op::ItemLinkResponse, std::move(w));
}

std::optional<ItemLinkClick> decode_item_link_click(const AppPacket& p) {
    if (p.data.size() != kItemLinkClickSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        ItemLinkClick v;
        v.item_id = r.le<std::uint32_t>();
        for (auto& a : v.augments) a = r.le<std::uint32_t>();
        v.hash = r.le<std::uint32_t>();
        v.flags = r.le<std::uint32_t>();
        v.is_evolving = r.le<std::uint32_t>();
        v.evolve_group = r.le<std::uint32_t>();
        v.evolve_level = r.le<std::uint32_t>();
        v.ornament_icon = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode_consume(const ItemSlot& slot, bool automatic, bool drink) {
    ByteWriter w;
    write_slot(w, slot);
    w.le<std::uint32_t>(automatic ? 0xffffffffu : 999u);
    w.le<std::uint32_t>(drink ? 2 : 1);
    w.le<std::uint32_t>(2);
    return make_packet(Op::Consume, std::move(w));
}

AppPacket encode_consume_reply(const ItemSlot& slot, bool drink) {
    ByteWriter w;
    write_slot(w, slot);
    w.le<std::uint32_t>(0xffffffffu);
    w.le<std::uint32_t>(drink ? 2 : 1);
    w.le<std::uint32_t>(1);
    return make_packet(Op::Consume, std::move(w));
}

AppPacket encode_item_verify_request(const ItemSlot& slot, std::uint32_t target_id) {
    ByteWriter w;
    write_slot(w, slot);
    w.le(target_id);
    return make_packet(Op::ItemVerifyRequest, std::move(w));
}

std::optional<ItemVerifyReply> decode_item_verify_reply(const AppPacket& p) {
    if (p.data.size() < kItemVerifyReplySize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        ItemVerifyReply v;
        v.slot = read_slot(r);
        v.spell = r.le<std::int32_t>();
        v.target_id = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode(const ItemVerifyReply& v) {
    ByteWriter w;
    write_slot(w, v.slot);
    w.le(v.spell);
    w.le(v.target_id);
    return make_packet(Op::ItemVerifyReply, std::move(w));
}

AppPacket encode(const ApplyPoison& v) {
    ByteWriter w;
    w.le(v.slot);
    w.le(v.sub);
    w.le(v.aug);
    w.le(v.unknown);
    w.le(v.success);
    return make_packet(Op::ApplyPoison, std::move(w));
}

AppPacket encode(const EvolvingFinalResult& v) {
    // CItemDisplayWnd's 0x69ef60: hyperlink action 1 uses this action-specific layout, not the toggle's fields.
    ByteWriter w;
    w.le<std::uint32_t>(4);
    w.le(v.item_id);
    w.le(v.final_item_id);
    w.le(v.level);
    w.le(v.hash);
    w.le(v.display_flags);
    return make_packet(Op::EvolveItem, std::move(w));
}

std::optional<EvolvingItemUpdate> decode_evolving_item_update(const AppPacket& p) {
    if (p.data.size() != 24) return std::nullopt;
    auto update = decode_with(p, [](ByteReader& r) {
        const auto action = r.le<std::uint32_t>();
        r.skip(4);
        EvolvingItemUpdate v;
        v.unique_id = r.le<std::uint64_t>();
        v.percentage = r.le<std::uint32_t>();
        v.activated = r.le<std::uint32_t>();
        // A large XP reward can overshoot 100 before the replacement arrives. Native reads a signed dword.
        if (action != 0 || v.unique_id == 0 || v.percentage > 0x7fffffffu || v.activated > 1)
            throw std::out_of_range("invalid evolving item update");
        return v;
    });
    return update;
}

std::optional<ApplyPoison> decode_apply_poison(const AppPacket& p) {
    if (p.data.size() < kApplyPoisonSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        ApplyPoison v;
        v.slot = r.le<std::int16_t>();
        v.sub = r.le<std::int16_t>();
        v.aug = r.le<std::int16_t>();
        v.unknown = r.le<std::int16_t>();
        v.success = r.le<std::uint32_t>();
        return v;
    });
}

std::optional<CoinReward> decode_quest_reward(const AppPacket& p) {
    if (p.data.size() < 36) return std::nullopt;
    return detail::decode_with(p, [](ByteReader& r) {
        CoinReward v;
        r.skip(4);
        v.target_id = r.le<std::uint32_t>();
        r.skip(12);
        v.coins.copper = r.le<std::uint32_t>();
        v.coins.silver = r.le<std::uint32_t>();
        v.coins.gold = r.le<std::uint32_t>();
        v.coins.platinum = r.le<std::uint32_t>();
        return v;
    });
}

std::optional<CoinReward> decode_cash_reward(const AppPacket& p) {
    if (p.data.size() < 16) return std::nullopt;
    return detail::decode_with(p, [](ByteReader& r) {
        CoinReward v;
        v.coins.copper = r.le<std::uint32_t>();
        v.coins.silver = r.le<std::uint32_t>();
        v.coins.gold = r.le<std::uint32_t>();
        v.coins.platinum = r.le<std::uint32_t>();
        return v;
    });
}

} // namespace hailstone::net
