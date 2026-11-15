#include "game/conversation_journal.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unordered_set>

#include "core/msvc_atoi.h"
#include "game/spell_record.h"
#include "platform/process.h"

namespace hailstone::game {

std::string journal_upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::uint32_t ConversationJournal::crc(const std::string& text) {
    return mem_checker1(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

bool JournalNpc::add_entry(std::int32_t time, float x, float y, float z, std::uint32_t text, std::int32_t category) {
    JournalEntry* e = nullptr;
    for (auto& p : entries)
        if (p->text == text) {
            e = p.get();
            break;
        }
    bool made = false;
    if (!e) {
        auto n = std::make_unique<JournalEntry>();
        n->time = time;
        n->x = static_cast<std::int32_t>(x);
        n->y = static_cast<std::int32_t>(y);
        n->z = static_cast<std::int32_t>(z);
        n->category = category;
        n->text = text;
        e = n.get();
        entries.push_back(std::move(n));
        made = true;
    } else if (e->time < time) {
        e->x = static_cast<std::int32_t>(x);
        e->y = static_cast<std::int32_t>(y);
        e->z = static_cast<std::int32_t>(z);
        e->time = time;
    }
    if (latest < e->time) latest = e->time;
    return made;
}

int JournalNpc::remove_entry(std::uint32_t text) {
    auto it = std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e->text == text; });
    if (it == entries.end()) return -1;
    entries.erase(it);
    latest = 0;
    for (const auto& e : entries) latest = std::max(latest, e->time);
    return static_cast<int>(entries.size());
}

const JournalEntry* JournalNpc::entry_in_category(std::int32_t category, std::size_t from) const {
    for (std::size_t i = from; i < entries.size(); ++i)
        if (entries[i]->category == category) return entries[i].get();
    return nullptr;
}

namespace {

// CJ.txt and CJ.hdr hold one value per line, as the original writes them raw (fprintf "%s\n"). A new line in a
// name or a dialogue line would split it on reload: the text's CRC no longer matches its entry, and a name holding
// "\n+ENTRY\n..." would read back as records. The original's journal cannot hold a new line either, so each CR or
// LF becomes a space before the value is stored or looked up (Hailstone's; the original does not check).
std::string journal_line(std::string s) {
    std::replace_if(s.begin(), s.end(), [](char c) { return c == '\r' || c == '\n'; }, ' ');
    return s;
}

// The name as it is stored: one line, at most 0x3f characters (the original's name buffer).
std::string journal_npc_name(const std::string& name) { return journal_line(name.substr(0, 0x3f)); }

} // namespace

JournalNpc* ConversationJournal::npc(const std::string& raw_name, std::int32_t zone, bool create) {
    // (truncated before the lookup too: a name past 0x3f characters finds the NPC it made)
    const std::string name = journal_npc_name(raw_name);
    for (auto& n : npcs_)
        if (n->zone == zone && n->name == name) return n.get();
    if (!create) return nullptr;
    auto n = std::make_unique<JournalNpc>();
    n->name = name;
    n->zone = zone;
    npcs_.push_back(std::move(n));
    return npcs_.back().get();
}

void ConversationJournal::add(const std::string& npc_name, std::int32_t zone, std::int32_t time, float x, float y,
                              float z, const std::string& text, std::int32_t category) {
    JournalNpc* n = npc(npc_name, zone, true);
    std::string line = journal_line(text);
    std::uint32_t c = crc(line);
    texts_.try_emplace(c, std::move(line));
    n->add_entry(time, x, y, z, c, category);
}

bool ConversationJournal::remove_npc(const std::string& raw_name, std::int32_t zone) {
    const std::string name = journal_npc_name(raw_name);
    auto it = std::find_if(npcs_.begin(), npcs_.end(), [&](const auto& n) { return n->zone == zone && n->name == name; });
    if (it == npcs_.end()) return false;
    npcs_.erase(it);
    return true;
}

JournalCategory* ConversationJournal::category(std::int32_t id) {
    for (auto& c : categories_)
        if (c->id == id) return c.get();
    return nullptr;
}

JournalCategory* ConversationJournal::category_named(const std::string& name) {
    for (auto& c : categories_)
        if (c->name == name) return c.get();
    return nullptr;
}

JournalCategory* ConversationJournal::set_category(const std::string& name, const std::string& description, std::int32_t id) {
    JournalCategory* c = id != -1 ? category(id) : nullptr;
    if (!c) {
        auto n = std::make_unique<JournalCategory>();
        std::int32_t free = 0; // 0x47a550: the lowest id not in use
        while (free < 1000000 && category(free)) ++free;
        n->id = free;
        categories_.push_back(std::move(n));
        c = categories_.back().get();
    }
    c->name = journal_line(name.substr(0, 0xf));
    c->description = journal_line(description.substr(0, 0xff));
    return c;
}

void ConversationJournal::remove_category(std::int32_t id) {
    auto it = std::find_if(categories_.begin(), categories_.end(), [&](const auto& c) { return c->id == id; });
    if (it == categories_.end()) return;
    categories_.erase(it);
    for (auto& n : npcs_)
        for (auto& e : n->entries)
            if (e->category == id) e->category = -1;
}

const std::string& ConversationJournal::text(std::uint32_t c) const {
    static const std::string none;
    auto it = texts_.find(c);
    return it == texts_.end() ? none : it->second;
}

int ConversationJournal::find_text(const JournalNpc& n, const std::string& upper, std::size_t from) const {
    for (std::size_t i = from; i < n.entries.size(); ++i) {
        auto it = texts_.find(n.entries[i]->text);
        if (it == texts_.end()) continue;
        if (journal_upper(it->second.substr(0, 0x7ff)).find(upper) != std::string::npos) return static_cast<int>(i);
    }
    return -1;
}

void ConversationJournal::clear() {
    npcs_.clear();
    categories_.clear();
}

namespace {

// fgets of at most `max` - 1 characters, the new line kept.
bool read_line(std::FILE* f, std::string& out, int max) {
    std::vector<char> buf(static_cast<std::size_t>(max) + 1, '\0');
    if (!std::fgets(buf.data(), max, f)) {
        out.clear();
        return false;
    }
    out = buf.data();
    return true;
}

// <base><suffix> ("CJ" + ".txt"), joined as a path: path::string() throws on Windows for a name the ANSI code page
// cannot show (the data folder under a user name in another script), and a narrow fopen cannot open it.
std::filesystem::path with_suffix(const std::filesystem::path& base, const char* suffix) {
    std::filesystem::path p = base;
    p += suffix;
    return p;
}

std::string strip_newline(std::string s) {
    if (!s.empty() && s.back() == '\n') s.pop_back();
    return s;
}

} // namespace

int ConversationJournal::load(const std::filesystem::path& base) {
    std::FILE* txt = platform::open_c_file(with_suffix(base, ".txt"), "rt");
    if (!txt) return -1; // "Warning: Unable to open journal, creating new file."
    std::FILE* hdr = platform::open_c_file(with_suffix(base, ".hdr"), "rt");
    if (!hdr) {
        std::fclose(txt);
        return -2; // "Error: Unable to open Journal."
    }
    std::unordered_map<std::uint32_t, std::string> file_texts;
    std::string line;
    while (!std::feof(txt)) {
        if (!read_line(txt, line, 0xfff)) break;
        if (line.size() < 2) continue;
        line = strip_newline(line);
        file_texts.try_emplace(crc(line), line);
    }
    line.clear();
    while (!std::feof(hdr)) {
        if (line.rfind("+CAT", 0) == 0) { // 0x47ad30
            std::string id, name, desc;
            if (read_line(hdr, id, 0x7ff) && read_line(hdr, name, 0x7ff) && read_line(hdr, desc, 0x7ff))
                set_category(strip_newline(name.substr(0, 0xf)), strip_newline(desc.substr(0, 0xff)), core::msvc_atoi(id));
            while (!std::feof(hdr)) {
                if (!read_line(hdr, line, 0xfff)) break;
                if (line.rfind("+NPC", 0) == 0 || line.rfind("+CAT", 0) == 0) break;
            }
        } else if (line.rfind("+NPC", 0) == 0) { // JournalNPC_ReadFromFile
            JournalNpc* n = nullptr;
            std::string name = "ErrorNPC", zone;
            if (read_line(hdr, name, 0x7ff)) {
                name = strip_newline(name);
                if (read_line(hdr, zone, 0x7ff)) {
                    while (!std::feof(hdr)) {
                        std::string l;
                        if (!read_line(hdr, l, 0x7ff) || l.rfind("-NPC", 0) == 0) break;
                    }
                    n = npc(name, core::msvc_atoi(zone), true);
                }
            }
            while (!std::feof(hdr)) {
                if (!read_line(hdr, line, 0xfff)) break;
                if (line.rfind("+NPC", 0) == 0 || line.rfind("+CAT", 0) == 0) break;
                if (line.rfind("+ENTRY", 0) == 0 && n) { // ConversationJournal_ReadEntries
                    std::string f[6];
                    bool ok = true;
                    for (auto& s : f) ok = ok && read_line(hdr, s, 0x7ff);
                    if (!ok) break;
                    std::uint32_t time = 0, text = 0;
                    std::sscanf(f[0].c_str(), "%x", &time);
                    std::sscanf(f[5].c_str(), "%x", &text);
                    n->add_entry(static_cast<std::int32_t>(time), static_cast<float>(core::msvc_atoi(f[1])),
                                 static_cast<float>(core::msvc_atoi(f[2])), static_cast<float>(core::msvc_atoi(f[3])), text,
                                 core::msvc_atoi(f[4]));
                    while (!std::feof(hdr)) {
                        std::string l;
                        if (!read_line(hdr, l, 0x7ff) || l.rfind("-ENTRY", 0) == 0) break;
                    }
                }
            }
        } else {
            if (!read_line(hdr, line, 0xfff)) break;
        }
    }
    // the texts the entries use
    for (const auto& n : npcs_)
        for (const auto& e : n->entries)
            if (auto it = file_texts.find(e->text); it != file_texts.end()) texts_.try_emplace(e->text, it->second);
    std::fclose(hdr);
    std::fclose(txt);
    return 0;
}

int ConversationJournal::save(const std::filesystem::path& base) const {
    std::FILE* txt = platform::open_c_file(with_suffix(base, ".txt"), "wt");
    if (!txt) return -1;
    std::FILE* hdr = platform::open_c_file(with_suffix(base, ".hdr"), "wt");
    if (!hdr) {
        std::fclose(txt);
        return -1;
    }
    std::fprintf(hdr, "EQJOURNAL\n");
    for (const auto& c : categories_) {
        std::fprintf(hdr, "%s\n", "+CAT");
        std::fprintf(hdr, "%d\n", c->id);
        std::fprintf(hdr, "%s\n", c->name.c_str());
        std::fprintf(hdr, "%s\n", c->description.c_str());
        std::fprintf(hdr, "%s\n", "-CAT");
    }
    std::vector<std::uint32_t> written;
    std::unordered_set<std::uint32_t> seen;
    for (const auto& n : npcs_) {
        std::fprintf(hdr, "%s\n", "+NPC");
        std::fprintf(hdr, "%s\n", n->name.c_str());
        std::fprintf(hdr, "%d\n", n->zone);
        std::fprintf(hdr, "%s\n", "-NPC");
        for (const auto& e : n->entries) {
            std::fprintf(hdr, "%s\n", "+ENTRY");
            std::fprintf(hdr, "%08X\n", static_cast<std::uint32_t>(e->time));
            std::fprintf(hdr, "%d\n", e->x);
            std::fprintf(hdr, "%d\n", e->y);
            std::fprintf(hdr, "%d\n", e->z);
            std::fprintf(hdr, "%d\n", e->category);
            std::fprintf(hdr, "%08X\n", e->text);
            std::fprintf(hdr, "%s\n", "-ENTRY");
            if (texts_.count(e->text) && seen.insert(e->text).second) written.push_back(e->text);
        }
    }
    for (std::uint32_t c : written) std::fprintf(txt, "%s\n", texts_.at(c).c_str());
    std::fclose(hdr);
    std::fclose(txt);
    return 0;
}

} // namespace hailstone::game
