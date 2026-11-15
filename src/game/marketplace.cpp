#include "game/marketplace.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <format>

#include "core/msvc_atoi.h"
#include "game/eq_strings.h"

namespace hailstone::game {

bool marketplace_can_buy(const net::MarketplaceEntry& e, std::uint32_t wallet) {
    return e.kind == 4 || e.kind == 6 || e.price <= wallet;
}

bool marketplace_can_raise_quantity(const net::MarketplaceEntry& e, std::uint32_t quantity, std::uint32_t wallet) {
    if (e.max_quantity != 0 && quantity >= e.max_quantity) return false;
    if (quantity >= kMarketplaceMaxQuantity || e.kind == 6) return false;
    // The product wraps in 32 bits, as the original multiplies.
    const auto total = static_cast<std::uint32_t>((quantity + 1u) * e.price);
    return total <= wallet;
}

std::string marketplace_price_text(std::uint32_t price, std::uint32_t money_type) {
    if (money_type == 0) return std::to_string(price);
    if (money_type == 1) return std::format("{:.2f}", static_cast<double>(static_cast<float>(price) * 0.01f));
    return "N/A";
}

std::string marketplace_funds_text(std::uint32_t wallet) {
    // sprintf("%d"): a signed print of the dword.
    return std::to_string(wallet == kMarketplaceFundsUnknown ? 0 : static_cast<std::int32_t>(wallet));
}

int marketplace_item_of_week(const std::vector<net::MarketplaceEntry>& catalog) {
    int best = -1;
    for (std::size_t i = 0; i < catalog.size(); ++i) {
        const auto& e = catalog[i];
        if (!e.item_of_week) continue;
        // The rank compares signed (the ctor's -1 is the lowest).
        if (best < 0 || e.item_of_week_rank > catalog[static_cast<std::size_t>(best)].item_of_week_rank)
            best = static_cast<int>(i);
    }
    return best;
}

std::string marketplace_purchase_text(const EqStrings& strings, const net::MarketplacePurchaseResult& r,
                                      const net::MarketplaceEntry* entry) {
    auto eq = [&](std::uint32_t id) { return strings.format(id, std::vector<std::string>{}); };
    const std::uint32_t code = r.success ? 0 : r.code;
    switch (code) {
    case 0: {
        // HWM: the entry's +0xce2 text ("%s") when the entry is known and the purchase succeeded.
        std::string msg = entry ? entry->purchase_message : std::string();
        std::string text = msg.empty() ? std::string() : msg + "\n\n";
        // The exe's own sentences @0x9ef570 / @0x9ef59c / @0x9ef5e0.
        std::string where = r.placed == 1 ? "An item has been placed in your inventory!"
                          : r.placed == 2 ? "Your inventory is full.  An item has been placed in your bank!"
                          : r.placed == 4 ? "Your inventory is full.  An item has been placed in your shared bank!"
                                          : std::string();
        text += eq(r.station_cash ? 7621 : 5361);
        if (!where.empty()) text += " " + where;
        if (!r.note_f0.empty()) text += "<BR><BR>\n\n" + r.note_f0;
        if (!r.note_4f0.empty()) text += "<BR><BR>\n\n" + r.note_4f0;
        return text;
    }
    case 1:
    case 4: return eq(7622);
    case 2: return eq(7623);
    case 3: return eq(7624);
    case 6: return eq(7632);
    case 8: return eq(714);
    default: return eq(7626);
    }
}

std::string marketplace_strip_breaks(std::string text) {
    for (std::size_t at; (at = text.find("<BR>")) != std::string::npos;) text.erase(at, 4);
    return text;
}


std::string marketplace_total_text(const net::MarketplaceEntry& e, std::uint32_t quantity) {
    const auto total = static_cast<std::uint32_t>(e.price * quantity);
    if (e.money_type == 0) return std::to_string(static_cast<std::int32_t>(total));
    if (e.money_type == 1) return std::format("{:.2f}", static_cast<double>(static_cast<float>(total) * 0.01f));
    return "N/A";
}

std::optional<std::string> marketplace_countdown(std::int64_t until, std::int64_t now) {
    if (until < 0 || static_cast<std::uint32_t>(until) <= static_cast<std::uint32_t>(now)) return std::nullopt;
    const std::uint32_t left = static_cast<std::uint32_t>(until) - static_cast<std::uint32_t>(now);
    const std::uint32_t d = left / 86400, h = left % 86400 / 3600, m = left % 3600 / 60, sec = left % 60;
    std::string out = d >= 1 ? std::format("{}:", d) : std::string();
    return out + std::format("{:02}:{:02}:{:02}", h, m, sec);
}

std::pair<std::int64_t, std::int64_t> marketplace_tile_timers(const net::MarketplaceEntry& e) {
    std::int64_t a = e.available_until, b = e.sale_until;
    if (a > -1 && b > -1) {
        if (a <= b) b = -1;
        else a = -1;
    }
    return {a, b};
}

std::string marketplace_details_text(const net::MarketplaceEntry& e, const std::string& category_name) {
    return "Category: " + category_name + "<BR>" + e.description + "<BR>";
}

std::optional<std::uint32_t> marketplace_link_entry(std::string_view link) {
    const std::size_t at = link.find("marketid=");
    if (at == std::string_view::npos) return std::nullopt;
    std::string_view rest = link.substr(at + 9);
    rest = rest.substr(0, rest.find('&'));
    return static_cast<std::uint32_t>(core::msvc_atoi(rest));
}

// ---------------------------------------------------------------- the window's model

namespace {

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& ch : out) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return out;
}

bool new_or_featured(const net::MarketplaceEntry& e) { return e.is_new || e.is_featured; }

// Index of "All Items" (id 1) in the array; see the class comment for why it stands in for array[1].
constexpr int kAllItemsIndex = 2;
constexpr int kNewAndFeaturedIndex = 0;

} // namespace

const net::MarketplaceEntry* MarketplaceWindow::selected_entry(const Catalog& c) const {
    const MarketplaceRow* r = selected_row();
    return r && r->entry >= 0 && r->entry < static_cast<int>(c.size()) ? &c[static_cast<std::size_t>(r->entry)] : nullptr;
}

int MarketplaceWindow::category_index(std::int32_t id) const {
    for (std::size_t i = 0; i < categories.size(); ++i)
        if (static_cast<std::int32_t>(categories[i].id) == id) return static_cast<int>(i);
    return -1;
}

void MarketplaceWindow::pre_populate() {
    selected = -1;
    item_of_week = -1;
    rows.clear();
    visible.clear();
    new_items.clear();
    featured_items.clear();
    new_shown.clear();
    featured_shown.clear();
    categories.clear();
    path_index_.clear();
    tree.clear();
    current = -1;
}

int MarketplaceWindow::find_or_create(std::string_view path) {
    if (path.empty()) return -1;
    if (auto hint = path_index_.find(path); hint != path_index_.end() && hint->second >= 0 &&
                                            hint->second < static_cast<int>(categories.size()) &&
                                            categories[static_cast<std::size_t>(hint->second)].path == path)
        return hint->second;
    for (std::size_t i = 0; i < categories.size(); ++i)
        if (categories[i].path == path) {
            path_index_.insert_or_assign(std::string(path), static_cast<int>(i));
            return static_cast<int>(i);
        }
    const std::size_t slash = path.rfind('/');
    const std::string_view leaf = slash == std::string_view::npos ? path : path.substr(slash + 1);
    int parent = slash == std::string_view::npos || slash == 0 ? category_index(1) : find_or_create(path.substr(0, slash));
    MarketplaceCategory n;
    n.id = next_id_++;
    n.name = std::string(leaf);
    n.path = std::string(path);
    n.parent = parent;
    categories.push_back(std::move(n));
    const int at = static_cast<int>(categories.size()) - 1;
    path_index_.insert_or_assign(std::string(path), at);
    if (parent >= 0) categories[static_cast<std::size_t>(parent)].children.push_back(at);
    return at;
}

void MarketplaceWindow::populate(const Catalog& c, bool in_game) {
    categories.clear();
    path_index_.clear();
    next_id_ = 3;
    auto builtin = [&](std::uint32_t id, const char* name, bool shown, int count) {
        MarketplaceCategory n;
        n.id = id;
        n.name = n.path = name;
        n.visible = shown;
        n.count = count;
        categories.push_back(std::move(n));
    };
    builtin(0, "New and Featured", in_game, 1); // always in the tree in game
    builtin(2, "Player Studio", in_game, 0);
    builtin(1, "All Items", true, 0);
    rows.clear();
    for (std::size_t i = 0; i < c.size(); ++i) {
        MarketplaceRow r;
        r.entry = static_cast<int>(i);
        r.order = i;
        std::string_view path = c[i].category;
        // strncmp(.., "Player Studio", 13) == 0 skips 14 characters (bounded here).
        if (path.substr(0, kMarketplacePlayerStudio.size()) == kMarketplacePlayerStudio)
            path = path.size() > 14 ? path.substr(14) : std::string_view();
        const int node = find_or_create(path);
        if (node >= 0) r.category = static_cast<std::int32_t>(categories[static_cast<std::size_t>(node)].id);
        rows.push_back(std::move(r)); // state 2: counted by nothing until the state feed
    }
    item_of_week = marketplace_item_of_week(c);
    if (item_of_week >= 0) item_of_week_shown = c[static_cast<std::size_t>(item_of_week)];
    new_items.clear();
    featured_items.clear();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (c[i].is_new) new_items.push_back(static_cast<int>(i));
        if (c[i].is_featured) featured_items.push_back(static_cast<int>(i));
    }
    rebuild_tree(in_game);
    refilter(c);
}

int MarketplaceWindow::adjust(std::int32_t id, bool add) {
    const int at = category_index(id);
    if (at < 0) return 0;
    for (int n = categories[static_cast<std::size_t>(at)].parent; n >= 0; n = categories[static_cast<std::size_t>(n)].parent)
        categories[static_cast<std::size_t>(n)].count += add ? 1 : -1;
    int& count = categories[static_cast<std::size_t>(at)].count;
    count += add ? 1 : -1;
    return add ? count == 1 : count == 0;
}

int MarketplaceWindow::find_row(const Catalog& c, std::uint32_t entry_id) const {
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const int e = rows[i].entry;
        if (e >= 0 && e < static_cast<int>(c.size()) && c[static_cast<std::size_t>(e)].id == entry_id) return static_cast<int>(i);
    }
    return -1;
}

MarketplaceWindow::StateResult MarketplaceWindow::apply_state(const Catalog& c, const net::MarketplaceRowState& s) {
    StateResult out;
    const int at = find_row(c, s.entry_id);
    if (at < 0) return out; // no row: nothing kept for later
    MarketplaceRow& r = rows[static_cast<std::size_t>(at)];
    if (!s.category.empty()) {
        // "Player Studio/%s" (0x9ef988) of the base category's full path, the base read as an index of the array.
        // The original throws its array exception for a row with no base (-1); such a row gets no mirror here.
        if (r.category >= 0) {
            const std::string base = r.category < static_cast<std::int32_t>(categories.size())
                                       ? categories[static_cast<std::size_t>(r.category)].path : std::string();
            const int m = find_or_create("Player Studio/" + base);
            if (m >= 0) r.mirror = static_cast<std::int32_t>(categories[static_cast<std::size_t>(m)].id);
        }
    }
    if (r.state != s.state) {
        if (r.state == kMarketplaceUncounted || s.state == kMarketplaceUncounted) {
            const bool add = s.state != kMarketplaceUncounted;
            if (adjust(r.category, add)) out.categories = true;
            if (r.mirror != -1 && adjust(r.mirror, add)) out.categories = true;
        }
        r.state = s.state;
        out.changed = true;
    }
    if (r.item_id != s.item_id) {
        r.item_id = s.item_id;
        out.changed = true;
    }
    if (r.u10 != s.u10) {
        r.u10 = s.u10;
        out.changed = true;
    }
    if (r.maker != s.category) {
        r.maker = s.category;
        out.changed = true;
    }
    return out;
}

void MarketplaceWindow::tree_rows(int node, int depth) {
    const auto& n = categories[static_cast<std::size_t>(node)];
    tree.push_back(MarketplaceTreeRow{n.id, n.name, depth, !n.children.empty()});
    for (int ch : n.children) {
        const auto& k = categories[static_cast<std::size_t>(ch)];
        if (k.visible && k.count > 0) tree_rows(ch, depth + 1);
    }
}

void MarketplaceWindow::rebuild_tree(bool in_game) {
    current = -1;
    tree.clear();
    for (std::size_t i = 0; i < categories.size(); ++i) {
        const auto& n = categories[i];
        if (n.visible && n.count > 0 && n.parent < 0) tree_rows(static_cast<int>(i), 0);
    }
    // Fresh branches start closed; the build then toggles "All Items" open.
    expanded = {1};
    if (in_game) highlight = 0;
}

bool MarketplaceWindow::in_subtree(int node, std::int32_t id) const {
    if (node < 0 || id < 0) return false;
    const auto& n = categories[static_cast<std::size_t>(node)];
    if (static_cast<std::int32_t>(n.id) == id) return true;
    for (int ch : n.children)
        if (in_subtree(ch, id)) return true;
    return false;
}

bool MarketplaceWindow::row_shown(int row) const {
    if (row < 0 || row >= static_cast<int>(rows.size())) return false;
    const auto& r = rows[static_cast<std::size_t>(row)];
    if (!r.find_match || r.state == kMarketplaceUncounted) return false;
    return current < 0 || in_subtree(current, r.category) || (r.mirror != -1 && in_subtree(current, r.mirror));
}

void MarketplaceWindow::refilter(const Catalog& c) {
    selected = -1;
    if (stale) return;
    visible.clear();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (!row_shown(static_cast<int>(i))) continue;
        visible.push_back(static_cast<int>(i));
        if (selected >= 0) continue;
        const int e = rows[i].entry;
        if (pending_entry == 0) show_details(c, static_cast<int>(i));
        else if (e >= 0 && e < static_cast<int>(c.size()) && c[static_cast<std::size_t>(e)].id == pending_entry) {
            show_details(c, static_cast<int>(i));
            pending_entry = 0;
        }
    }
    // The panels hold catalog indices (their tiles were made in populate's order); the row of each by `order`.
    std::vector<int> row_of(rows.size(), -1);
    for (std::size_t k = 0; k < rows.size(); ++k)
        if (rows[k].order < row_of.size()) row_of[rows[k].order] = static_cast<int>(k);
    auto panel = [&](const std::vector<int>& from, std::vector<int>& shown) {
        shown.clear();
        for (int e : from) {
            const int i = e >= 0 && e < static_cast<int>(row_of.size()) ? row_of[static_cast<std::size_t>(e)] : -1;
            if (i < 0) continue;
            const auto& r = rows[static_cast<std::size_t>(i)];
            if (!r.find_match || r.state == kMarketplaceUncounted) continue;
            shown.push_back(i);
            if (selected < 0) show_details(c, i);
        }
    };
    panel(new_items, new_shown);
    panel(featured_items, featured_shown);
    if (selected < 0) clear_details();
}

bool MarketplaceWindow::show_details(const Catalog& c, int row) {
    if (row < 0 || row >= static_cast<int>(rows.size())) return false;
    const auto& r = rows[static_cast<std::size_t>(row)];
    if (r.state == kMarketplaceUncounted) return false;
    selected = row;
    set_quantity(c, 1);
    page = MarketplacePage::Main;
    preview_request = r.item_id;
    // SendItemPreview(0) goes straight to SetPreviewState(false): the details tab back to its first page.
    if (r.item_id == 0) preview_tab = false;
    return true;
}

void MarketplaceWindow::set_quantity(const Catalog& c, std::uint32_t q) {
    if (const net::MarketplaceEntry* e = selected_entry(c)) {
        if (e->max_quantity > 0 && q > e->max_quantity) q = e->max_quantity;
        else if (q < 1) q = 1;
        if (q > kMarketplaceMaxQuantity) q = kMarketplaceMaxQuantity;
    }
    quantity = q;
}

void MarketplaceWindow::sort(const Catalog& c, int choice) {
    sort_choice = choice;
    if (rows.size() > 1) {
        const int key = choice <= 0 ? 0 : (choice - 1) / 2 + 1;
        const bool down = choice > 0 && ((choice - 1) & 1) != 0;
        auto entry = [&](const MarketplaceRow& r) -> const net::MarketplaceEntry& { return c[static_cast<std::size_t>(r.entry)]; };
        auto less = [&](const MarketplaceRow& a, const MarketplaceRow& b) {
            const auto& x = entry(a);
            const auto& y = entry(b);
            if (key == 0) {
                if (x.is_new != y.is_new) return x.is_new;
                if (x.is_featured != y.is_featured) return x.is_featured;
                return a.order < b.order;
            }
            if (key == 1) {
                const int cmp = std::strcmp(x.name.c_str(), y.name.c_str());
                return down ? cmp > 0 : cmp < 0;
            }
            if (key == 2) return down ? x.price > y.price : x.price < y.price;
            return false;
        };
        std::stable_sort(rows.begin(), rows.end(), less);
    }
    refilter(c);
}

bool MarketplaceWindow::find(const Catalog& c, std::string_view text, std::uint32_t now_ms) {
    if (!(now_ms > last_find_ms + 500u)) return false;
    last_find_ms = now_ms;
    view = toggle_view();
    if (!text.empty()) {
        current = -1;
        highlight.reset();
        const std::string needle = upper(text);
        for (auto& r : rows) {
            const auto& e = c[static_cast<std::size_t>(r.entry)];
            r.find_match = upper(e.name).find(needle) != std::string::npos;
        }
    } else {
        highlight = 1;
        for (auto& r : rows) r.find_match = true;
    }
    refilter(c);
    return true;
}

void MarketplaceWindow::click_category(const Catalog& c, std::uint32_t id) {
    const int node = category_index(static_cast<std::int32_t>(id));
    if (node < 0) return;
    highlight = id;
    if (node == current) {
        if (expanded.count(id)) expanded.erase(id);
        else expanded.insert(id);
        return;
    }
    current = node;
    for (auto& r : rows) r.find_match = true;
    refilter(c);
    view = id == 0 ? MarketplaceView::Landing : toggle_view();
}

void MarketplaceWindow::set_view(MarketplaceView v) {
    view = v;
    if (v == MarketplaceView::List) list_button = true;
    else if (v == MarketplaceView::Icons) list_button = false;
}

void MarketplaceWindow::navigate(const Catalog& c, std::uint32_t entry_id, bool in_game) {
    const int at = find_row(c, entry_id);
    if (at < 0) return;
    const auto& r = rows[static_cast<std::size_t>(at)];
    const auto& e = c[static_cast<std::size_t>(r.entry)];
    if (in_game && new_or_featured(e)) {
        highlight = 0;
        current = kNewAndFeaturedIndex;
        view = MarketplaceView::Landing;
    } else if (current < 0 || static_cast<std::int32_t>(categories[static_cast<std::size_t>(current)].id) != r.category) {
        const int node = category_index(r.category);
        if (node >= 0) {
            for (int n = node; n >= 0; n = categories[static_cast<std::size_t>(n)].parent)
                expanded.insert(categories[static_cast<std::size_t>(n)].id);
            highlight = categories[static_cast<std::size_t>(node)].id;
        }
        current = node;
        for (auto& row : rows) row.find_match = true;
        view = toggle_view();
    }
    refilter(c);
    show_details(c, find_row(c, entry_id));
}

void MarketplaceWindow::reset_view(const Catalog& c, bool in_game) {
    current = -1;
    for (auto& r : rows) r.find_match = true;
    sort(c, 0);
    const net::MarketplaceEntry* e = selected_entry(c);
    const bool landing = in_game && (!e || new_or_featured(*e));
    if (landing) current = kNewAndFeaturedIndex;
    else current = categories.size() > static_cast<std::size_t>(kAllItemsIndex) ? kAllItemsIndex : -1;
    list_button = false;
    view = landing ? MarketplaceView::Landing : MarketplaceView::Icons;
    page = MarketplacePage::Main;
    highlight = landing ? 0u : 1u;
}

void MarketplaceWindow::sync_shelf(const Catalog& c, bool in_game) {
    const net::MarketplaceEntry* e = selected_entry(c);
    const MarketplaceRow* r = selected_row();
    if (stale || !e || !r || categories.size() < 3) return;
    const bool nf = in_game && new_or_featured(*e);
    const int target = nf ? kNewAndFeaturedIndex : kAllItemsIndex;
    if (nf && current == kNewAndFeaturedIndex) return;
    if (current >= 0 && in_subtree(current, r->category)) return;
    current = target;
    view = nf ? MarketplaceView::Landing : toggle_view();
    highlight = nf ? 0u : 1u;
}

} // namespace hailstone::game
