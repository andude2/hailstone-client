#include "core/ini_file.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include "core/atomic_file.h"

namespace hailstone {

namespace {
std::string_view trim(std::string_view s) {
    const char* ws = " \t\r\n";
    std::size_t b = s.find_first_not_of(ws);
    if (b == std::string_view::npos) return {};
    std::size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}
bool iequal(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}
// Names that reach the file from outside (a world or character name the server sends, a typed account) must stay
// on their own line: a line break in a key or section would write lines of its own ("\n[Server]\nhost=..."), an '='
// in a key would split it in the wrong place on the next load, a ']' would end a section name early, and a key that
// starts like a comment or a section header would not come back as a key. Such characters become '_'; get, set and
// erase clean the name the same way, so a cleaned name is found again. Spaces and tabs around a name are dropped, as
// parse() drops them, so a name set with them is the one found after a reload.
std::string clean_name(std::string_view name, bool section) {
    const std::size_t b = name.find_first_not_of(" \t");
    if (b == std::string_view::npos) return {};
    std::string out(name.substr(b, name.find_last_not_of(" \t") - b + 1));
    for (char& c : out)
        if (c == '\n' || c == '\r' || (section ? c == ']' : c == '=')) c = '_';
    if (!section && !out.empty() && (out[0] == ';' || out[0] == '#' || out[0] == '[')) out[0] = '_';
    return out;
}
} // namespace

std::string IniFile::clean_key(std::string_view key) { return clean_name(key, false); }

bool IniFile::load(const std::filesystem::path& path) {
    sections_.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    parse(ss.str());
    return true;
}

void IniFile::parse(std::string_view text) {
    sections_.clear();
    std::string current;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        std::string_view line = trim(text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos));
        pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[') {
            std::size_t close = line.find(']');
            current = std::string(trim(line.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1)));
            if (!find(current)) sections_.push_back({current, {}});
            continue;
        }
        std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        // Values keep their spaces after the '=' (labels such as "Melee Attack"); only the line ends are trimmed.
        set(current, trim(line.substr(0, eq)), line.substr(eq + 1));
    }
}

std::string IniFile::to_string() const {
    std::string out;
    for (const Section& s : sections_) {
        if (!s.name.empty() || !s.entries.empty()) out += "[" + s.name + "]\n";
        for (const auto& [k, v] : s.entries) out += k + "=" + v + "\n";
    }
    return out;
}

bool IniFile::save(const std::filesystem::path& path) const {
    return write_file_atomic(path, to_string()); // a crash mid-write keeps the old file
}

IniFile::Section* IniFile::find(std::string_view name) {
    return const_cast<Section*>(std::as_const(*this).find(name));
}

const IniFile::Section* IniFile::find(std::string_view name) const {
    const std::string clean = clean_name(name, true);
    for (const Section& s : sections_)
        if (iequal(s.name, clean)) return &s;
    return nullptr;
}

std::optional<std::string> IniFile::get(std::string_view section, std::string_view key) const {
    const std::string clean = clean_name(key, false);
    if (const Section* s = find(section))
        for (const auto& [k, v] : s->entries)
            if (iequal(k, clean)) return v;
    return std::nullopt;
}

std::string IniFile::get_string(std::string_view section, std::string_view key, std::string_view fallback) const {
    auto v = get(section, key);
    return v ? *v : std::string(fallback);
}

int IniFile::get_int(std::string_view section, std::string_view key, int fallback) const {
    auto v = get(section, key);
    if (!v) return fallback;
    std::string_view t = trim(*v);
    int out = 0;
    auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), out);
    return ec == std::errc() ? out : fallback;
}

std::vector<std::string> IniFile::keys(std::string_view section) const {
    std::vector<std::string> out;
    if (const Section* s = find(section))
        for (const auto& [k, v] : s->entries) out.push_back(k);
    return out;
}

std::vector<std::string> IniFile::sections() const {
    std::vector<std::string> out;
    for (const auto& s : sections_) out.push_back(s.name);
    return out;
}

void IniFile::set(std::string_view section, std::string_view key, std::string_view value) {
    std::string v(value);
    std::replace(v.begin(), v.end(), '\n', ' ');
    std::replace(v.begin(), v.end(), '\r', ' ');
    // A value's trailing spaces are written as given: the original writes some (PetButtonState ends in a space), and
    // reads them back trimmed, as GetPrivateProfileString does and parse() does here.
    std::string clean = clean_name(key, false);
    Section* s = find(section);
    if (!s) {
        sections_.push_back({clean_name(section, true), {}});
        s = &sections_.back();
    }
    for (auto& [k, val] : s->entries)
        if (iequal(k, clean)) {
            val = std::move(v);
            return;
        }
    s->entries.emplace_back(std::move(clean), std::move(v));
}

void IniFile::erase(std::string_view section, std::string_view key) {
    const std::string clean = clean_name(key, false);
    if (Section* s = find(section))
        std::erase_if(s->entries, [&](const auto& e) { return iequal(e.first, clean); });
}

void IniFile::erase_section(std::string_view section) {
    const std::string clean = clean_name(section, true);
    std::erase_if(sections_, [&](const Section& s) { return iequal(s.name, clean); });
}

bool eqclient_ini_bool(const std::optional<std::string>& text, bool fallback) {
    if (!text) return fallback;
    char c = text->empty() ? '\0' : (*text)[0];
    return c == 'T' || c == 't' || c == '1';
}

} // namespace hailstone
