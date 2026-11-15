#include "game/barter.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <format>

#include "game/bazaar.h"
#include "game/inventory.h"

namespace hailstone::game {

namespace {

// GetDelimitedToken(text, out, size, sep, 1, 0): the text up to the separator (at most size - 1 characters kept)
// and how far it read, the separator included.
std::size_t token(std::string_view text, char sep, std::string& out, std::size_t size = 0x800) {
    std::size_t n = text.find(sep);
    std::size_t len = n == std::string_view::npos ? text.size() : n;
    out.assign(text.substr(0, std::min(len, size - 1)));
    return n == std::string_view::npos ? len : len + 1;
}

// A number field: hex after "0x", else atoi. The original's MSVC atoi and strtol saturate at the 32-bit long
// range; read 64 bits and clamp, so an overflowing field is defined and the same whatever the size of long.
std::int64_t number(const std::string& s) {
    const bool hex = s.size() > 1 && s[0] == '0' && s[1] == 'x';
    const long long v = std::strtoll(s.c_str(), nullptr, hex ? 16 : 10);
    return std::clamp<long long>(v, INT32_MIN, INT32_MAX);
}

// ParseDelimitedInt(text, &out, sep): a token read as a number.
std::size_t int_field(std::string_view text, std::int64_t& out, char sep = '^') {
    std::string t;
    std::size_t used = token(text, sep, t);
    out = number(t);
    return used;
}

void add_item(std::vector<BarterInventoryItem>& out, const net::Item& item) {
    std::uint32_t n = item.stack_count;
    for (auto& e : out)
        if (e.item_id == item.id) {
            e.count += n;
            return;
        }
    BarterInventoryItem e;
    e.item_id = item.id;
    e.icon = item.icon;
    if (item.effects[net::kEffectClick].max_charges() > 0) e.charges = item.charges;
    e.count = n;
    e.name = item.name.substr(0, 63);
    out.push_back(std::move(e));
}

// One slot's item and its bag's contents, the eligible ones.
void collect_slot(std::vector<BarterInventoryItem>& out, const Inventory& inventory, std::int16_t slot, bool krono) {
    const net::Item* item = inventory.at(net::ItemSlot::possessions(slot));
    if (!item) return;
    auto take = [&](const net::Item& i) {
        if (!barter_item_eligible(i) || (!krono && i.id == net::kKronoItemId)) return;
        add_item(out, i);
    };
    take(*item);
    if (item->is_bag())
        for (const auto& c : item->contents) take(c.item);
}

} // namespace

std::string buy_line_ini(const net::BuyLine& line) {
    std::string s = std::format("*{}^{}^{}^", 2, line.slot, line.enabled ? 1 : 0);
    if (!line.enabled) return s;
    s += std::format("{}^{}^{}^{}^{}^{}^{}", line.toggle ? 1 : 0, line.item_id, line.item_name, line.icon, line.cost,
                     line.quantity, line.trade_items.size());
    for (const auto& t : line.trade_items) s += std::format("^{}^{}^{}^{}", t.item_id, t.quantity, t.name, t.icon);
    return s;
}

net::BuyLine parse_buy_line_ini(std::string_view text) {
    net::BuyLine v;
    std::int64_t version = 1;
    if (!text.empty() && text[0] == '*') {
        text.remove_prefix(1);
        text.remove_prefix(int_field(text, version));
    }
    v.enabled = true;
    if (version > 1) {
        std::int64_t slot = 0, enabled = 0;
        text.remove_prefix(int_field(text, slot));
        text.remove_prefix(int_field(text, enabled));
        v.slot = static_cast<std::uint32_t>(slot);
        v.enabled = enabled != 0;
    }
    if (!v.enabled) return v;
    std::int64_t active = 0, id = 0, icon = 0, cost = 0, quantity = 0, n = 0;
    text.remove_prefix(int_field(text, active));
    v.toggle = active != 0;
    text.remove_prefix(int_field(text, id));
    v.item_id = static_cast<std::uint32_t>(id);
    text.remove_prefix(token(text, '^', v.item_name, 0x40));
    text.remove_prefix(int_field(text, icon));
    v.icon = static_cast<std::uint32_t>(icon);
    text.remove_prefix(int_field(text, cost));
    v.cost = static_cast<std::uint32_t>(cost);
    text.remove_prefix(int_field(text, quantity));
    v.quantity = static_cast<std::uint32_t>(quantity);
    text.remove_prefix(int_field(text, n));
    // The count comes from the server (kBuyerLinesText) or the barter ini: kept to the binary decoder's bound, and
    // no entries are made up once the text has run out.
    n = std::clamp<std::int64_t>(n, 0, net::kMaxBuyLineTrades);
    for (std::int64_t i = 0; i < n && !text.empty(); ++i) {
        net::BuyLineTrade t;
        std::int64_t tid = 0, tq = 0, ti = 0;
        text.remove_prefix(int_field(text, tid));
        text.remove_prefix(int_field(text, tq));
        text.remove_prefix(token(text, '^', t.name, 0x40));
        text.remove_prefix(int_field(text, ti));
        t.item_id = static_cast<std::uint32_t>(tid);
        t.quantity = static_cast<std::uint32_t>(tq);
        t.icon = static_cast<std::uint32_t>(ti);
        v.trade_items.push_back(std::move(t));
    }
    return v;
}

std::vector<std::pair<std::uint32_t, net::BuyLine>> parse_buy_lines_text(std::string_view text) {
    // 0x423450 reads the '$'-ended count; each record's second '^' field is its slot (the first, "*2", is read and
    // dropped).
    std::vector<std::pair<std::uint32_t, net::BuyLine>> out;
    std::int64_t count = 0;
    text.remove_prefix(int_field(text, count, '$'));
    // The count is the server's: once the text runs out no empty records are made (each would land in slot 0 and
    // be saved over the line there).
    for (std::uint16_t left = static_cast<std::uint16_t>(count); left != 0 && !text.empty(); --left) {
        std::string record;
        text.remove_prefix(token(text, '$', record, 0x4a6));
        std::string first, second;
        std::size_t used = token(record, '^', first);
        token(std::string_view(record).substr(used), '^', second);
        auto slot = static_cast<std::uint32_t>(number(second));
        out.emplace_back(slot, parse_buy_line_ini(record));
    }
    return out;
}

std::string buy_line_price_text(const net::BuyLine& line) {
    std::string s = coin_compact(line.cost) + " ";
    if (!line.trade_items.empty()) s += "+ items";
    return s;
}

std::uint32_t barter_set_price_coin(std::uint32_t price, int coin, std::uint32_t v) {
    // In 64 bits, then the cap: a large answer (a script's, the dev window's) wrapped in 32 bits could come out under
    // it as some other price.
    const std::uint64_t p = price, n = v;
    std::uint64_t out = p;
    switch (coin) {
    case 0: out = p % 1000 + n * 1000; break;
    case 1: out = (n + (p / 1000) * 10) * 100 + p % 100; break;
    case 2: out = p % 10 + (n + (p / 100) * 10) * 10; break;
    case 3: out = p - p % 10 + n; break;
    default: break;
    }
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(out, kMaxBuyPrice));
}

std::string barter_report(std::string_view verb, std::string_view preposition, const net::BuyLine& line,
                          std::string_view who, std::int32_t count) {
    std::uint64_t price = static_cast<std::uint64_t>(line.cost) * static_cast<std::uint64_t>(static_cast<std::int64_t>(count));
    std::string s = std::format("{}{} {}{}{} for {} ", verb, count, line.item_name, preposition, who,
                                coin_compact(static_cast<std::int64_t>(price)));
    if (!line.trade_items.empty()) {
        s += " and the following: ";
        for (std::size_t i = 0; i < line.trade_items.size(); ++i) {
            if (i != 0) s += ", ";
            s += std::format("{} {}", static_cast<std::int64_t>(line.trade_items[i].quantity) * count,
                             line.trade_items[i].name);
        }
    }
    return s + ".";
}

bool barter_item_eligible(const net::Item& item) {
    if (item.is_bag() || !item.contents.empty()) return false;
    if (item.no_trade() || item.attuned != 0) return false;
    const auto& click = item.effects[net::kEffectClick];
    if (click.effect != -1 && click.max_charges() >= 1 &&
        static_cast<std::int32_t>(item.charges) != click.max_charges())
        return false;
    return true;
}

std::vector<BarterInventoryItem> barter_window_inventory(const Inventory& inventory) {
    std::vector<BarterInventoryItem> out;
    for (std::int16_t slot = net::kSlotGeneral1; slot <= net::kSlotGeneral10; ++slot)
        collect_slot(out, inventory, slot, false);
    return out;
}

std::vector<BarterInventoryItem> barter_search_inventory(const Inventory& inventory, std::uint32_t krono) {
    std::vector<BarterInventoryItem> out;
    if (krono != 0) out.push_back({net::kKronoItemId, 0, krono, 0xe53, "Krono"});
    for (std::int16_t slot = 0; slot < 0x22; ++slot) collect_slot(out, inventory, slot, true);
    return out;
}

bool barter_has_items(const Inventory& inventory, std::uint32_t item_id, std::uint32_t count) {
    std::uint32_t n = 0;
    for (const auto& e : barter_window_inventory(inventory))
        if (e.item_id == item_id) n += e.count;
    return n >= count;
}

int BarterWindowState::free_slot() const {
    for (int i = 0; i < kBuyLineCount; ++i)
        if (lines[static_cast<std::size_t>(i)].item_name.empty()) return i;
    return -1;
}

BarterWindowButtons barter_window_buttons(const BarterWindowState& s, std::uint32_t now_seconds) {
    BarterWindowButtons b;
    b.search = s.last_search + 5 < now_seconds;
    bool item = s.item_sel >= 0, line = s.line_sel >= 0;
    bool editing = s.editor.enabled && (item || line);
    b.inventory_remove = editing && s.compensation_sel >= 0;
    b.inventory_add = editing && s.inventory_sel >= 0;
    b.offline = s.buyer_mode;
    if (s.buyer_mode) {
        b.start_stop = "Stop Barter";
        b.start_stop_color = 0xffff0000;
    }
    b.create_shown = !line;
    b.create = item;
    b.modify_shown = line;
    b.modify = line;
    b.remove = line;
    b.toggle = line;
    return b;
}

void barter_merchant_update(BarterMerchantState& m, const net::BuyLine& line) {
    for (std::size_t i = 0; i < m.lines.size(); ++i) {
        if (m.lines[i].slot != line.slot) continue;
        if (line.toggle) m.lines[i] = line;
        else m.lines.erase(m.lines.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
    m.lines.push_back(line);
}

std::vector<std::pair<std::string, std::int64_t>> BarterSearchState::combo_choices() const {
    std::vector<std::pair<std::string, std::int64_t>> out{{"All Barterers", 0}, {"Local Barterers", -1}};
    std::vector<const net::BarterBuyer*> sorted;
    for (const auto& [id, b] : buyers) sorted.push_back(&b);
    std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) { return a->name < b->name; });
    for (const auto* b : sorted) out.emplace_back(b->name, b->buyer_id);
    return out;
}

void barter_search_sold(BarterSearchState& s, const net::BuyLine& line, std::int32_t sold) {
    for (auto& l : s.lines)
        if (l.buyer_id == line.buyer_id && l.slot == line.slot) {
            l.quantity = static_cast<std::uint32_t>(static_cast<std::int64_t>(l.quantity) - sold);
            return;
        }
}

} // namespace hailstone::game
