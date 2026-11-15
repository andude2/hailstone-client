#include "game/option_commands.h"

#include <cctype>
#include <cstdlib>
#include <format>
#include <utility>

#include "core/msvc_atoi.h"
#include "core/wrap_math.h"
#include "game/slash_commands.h"
#include "platform/local_time.h"

namespace hailstone::game {

namespace {

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool either_case(char c, char upper) { return c == upper || c == upper + 0x20; }

// args[i], '\0' past the end (the original reads the C string's terminator).
char at(std::string_view s, std::size_t i) { return i < s.size() ? s[i] : '\0'; }

std::int32_t wrap_add(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
std::int32_t wrap_sub(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}
// value * 100 / whole: the product with wraparound, the division signed.
std::int32_t percent(std::int32_t value, std::int32_t whole) {
    if (whole == 0) return 0;
    return core::wrapped_div(static_cast<std::uint32_t>(value) * 100u, whole);
}

} // namespace

bool is_numeric_string(std::string_view text) {
    if (text.empty()) return false;
    std::size_t i = text[0] == '+' || text[0] == '-' ? 1 : 0;
    for (; i < text.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) return false;
    return true;
}

bool starts_no_case(std::string_view a, std::string_view b, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        char x = lower(at(a, i)), y = lower(at(b, i));
        if (x != y) return false;
        if (x == '\0') return true;
    }
    return true;
}

bool same_no_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

std::int32_t eq_atoi(std::string_view text) { return core::msvc_atoi(text); }

bool bare_file_name(std::string_view name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find_first_of(std::string_view("/\\:\0", 4)) == std::string_view::npos;
}

Switch off_on_toggle(std::string_view args, std::string_view off, std::string_view on) {
    if (starts_no_case(args, off, 3)) return Switch::Off;
    if (starts_no_case(args, on, 2)) return Switch::On;
    return Switch::Toggle;
}

// ---------------------------------------------------------------- /quit, /exit, /safelock

bool locked_line_allowed(std::string_view line, std::string_view safelock_word) {
    // InterpretCmd: leading spaces skipped; ';' and '[' lines refused; the first word (GetNextCmdArg for '/', a letter
    // or a digit, else the one character) against "/safelock" on 4 characters, with case.
    while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
    if (line.empty()) return true;
    if (line[0] == ';' || line[0] == '[') return false;
    std::string word;
    if (line[0] == '/' || std::isalnum(static_cast<unsigned char>(line[0]))) {
        std::string_view rest = line;
        word = next_cmd_arg(rest);
    } else {
        word = std::string(1, line[0]);
    }
    for (std::size_t i = 0; i < 4; ++i)
        if (at(word, i) != at(safelock_word, i)) return false;
    return true;
}

std::vector<std::uint32_t> safelock_command(std::string& password, std::string_view args) {
    if (args.empty()) {
        if (!password.empty()) return {kStrInterfaceLocked};
        return {13287, 13288}; // the usage
    }
    if (!password.empty()) {
        if (same_no_case(args, password)) {
            password.clear();
            return {13289}; // Interface unlocked.
        }
        return {13290}; // Incorrect password.
    }
    if (args.size() < 0x20) {
        password = std::string(args);
        return {13291}; // Interface locked.  Please use /safelock ...
    }
    return {13292}; // too long
}

bool key_command_needs_control(int command) {
    // ExecuteCmd_KeyCmdToCase's commands whose case tests the flag first: cases 0..12, 14, 16, 19, 21, 23..34, 36,
    // 38..49, 52, 89..96, 98..100, 103, 104, 109, 112..118, 134, 161, 163 and 171.
    static constexpr std::pair<int, int> kRanges[] = {
        {0, 12},    // AUTORUN .. DUCK
        {14, 14},   // INVENTORY
        {16, 16},   // USE
        {19, 19},   // CENTERVIEW
        {22, 22},   // TOGGLECAM
        {24, 268},  // TARGETME, PARTY1..5, the target keys, NETSTAT, HOT1_1..HOT10_12, HOTPAGE*, CAST1..12
        {270, 271}, // CYCLEREPLY, CYCLEREPLY_BACK
        {273, 284}, // TOGGLETARGET, SPELLBOOK, ABILITIES, COMBAT, SOCIALS, MAIN, WHO .. RUN_WALK
        {288, 288}, // SCREENCAP
        {343, 350}, // TOGGLETWOTARGETS, TOGGLE_CONTEXTMENUS, the six camera keys
        {352, 354}, // CLOSE_TOP_WINDOW, CLEAR_TARGET, CMD_TOGGLE_FIND_WINDOW
        {357, 365}, // CMD_OPENING_ATTACK1..8, CMD_TOGGLE_COMBAT_ABILITY_WIN
        {370, 370}, // CMD_STOP_CAST
        {373, 382}, // POTION_SLOT_1..4, TOGGLE_POTION_BELT, TOGGLE_BANDOLIER, the bag keys, TOGGLE_SKILLS_WND
        {403, 403}, // CMD_EXTENDED_TARGET_WINDOW
        {450, 451}, // CMD_STOP_ACTION, POTION_SLOT_5
        {453, 453}, // CMD_TOGGLE_BANK_BAGS
        {461, 461}, // CMD_LOCK_AGGRO
    };
    for (auto [first, last] : kRanges)
        if (command >= first && command <= last) return true;
    return false;
}

// ---------------------------------------------------------------- /charinfo

std::string charinfo_line(std::span<const std::uint32_t> bind_zones,
                          const std::function<std::string(std::uint32_t)>& full_zone_name) {
    std::string line = "You are currently bound in: ";
    auto bind = [&](std::size_t k) -> std::uint32_t { return k < bind_zones.size() ? bind_zones[k] : 0; };
    for (std::size_t k = 0; k < 5; ++k) {
        if (!bind(k)) continue;
        std::string zone = full_zone_name(bind(k));
        if (k == 0) {
            line += zone;
            continue;
        }
        if (bind(k - 1) != bind(k)) line = std::format("{}, {}", line, zone);
    }
    return line;
}

// ---------------------------------------------------------------- /log

LogCommand log_command(std::string_view args, std::string_view off, std::string_view on, bool logging) {
    LogCommand c;
    c.name = "eqlog";
    switch (off_on_toggle(args, off, on)) {
    case Switch::Off: c.on = false; break;
    case Switch::On: c.on = true; break;
    case Switch::Toggle: c.on = !logging; break;
    }
    if (c.on) {
        // NextArg(NextArg(args)): the second word, when there is one.
        std::string_view rest = args;
        next_cmd_arg(rest);
        std::string second = next_cmd_arg(rest);
        if (!second.empty()) c.name = second.substr(0, 0x31);
    }
    return c;
}

std::string ctime_text(std::time_t t) {
    const std::tm tm = platform::local_time(t).value_or(std::tm{});
    static constexpr const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    return std::format("{} {} {:2} {:02}:{:02}:{:02} {}", kDays[tm.tm_wday % 7], kMonths[tm.tm_mon % 12], tm.tm_mday,
                       tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_year + 1900);
}

std::string log_line(std::time_t now, std::string_view text) { return std::format("[{}] {}\n", ctime_text(now), text); }

// ---------------------------------------------------------------- /shownames

ShowNames shownames_command(std::string_view args, std::string_view off) {
    ShowNames r;
    std::string_view rest = args;
    std::string word = next_cmd_arg(rest).substr(0, 0x3f);
    if (word.empty()) {
        r.usage = true;
        r.eqstr = kStrShowNamesUsage;
        return r;
    }
    if (starts_no_case(word, off, 3)) {
        r.off = true;
        r.eqstr = kStrShowNamesOff;
        return r;
    }
    std::int32_t level = eq_atoi(word);
    if (static_cast<std::uint32_t>(level) - 1 > 5) level = 4;
    static constexpr std::uint32_t kMessages[6] = {13294, 13295, 13296, 13297, 8874, 8875};
    r.level = level;
    r.eqstr = kMessages[level - 1];
    return r;
}

// ---------------------------------------------------------------- /usercolor

namespace {

// /usercolor's help (the exe's own lines): the usage, then page1..page7.
constexpr const char* kUsage[] = {"Format: /usercolor <number> <red> <green> <blue>", "<number> -> 1 to 69",
                                  "<red> <green> <blue> -> 0 to 255",
                                  "To reset a user color use /usercolor <number> reset",
                                  "To display a list of usercolor #'s use /usercolor help"};
constexpr const char* kPage1[] = {"Here is a list of usercolor numbers:", " 1 - Say", " 2 - Tell", " 3 - Group",
                                  " 4 - Guild", " 5 - OOC", " 6 - Auction", " 7 - Shout", " 8 - Emote",
                                  " 9 - Spells (meming, scribing, casting, etc.)", " 10 - You hit other",
                                  " Type /usercolor page2 for numbers 11 - 20"};
constexpr const char* kPage2[] = {" 11 - Other hits you", " 12 - You miss other", " 13 - Other misses you",
                                  " 14 - Some broadcasts (duels)", " 15 - Skills (ups, non-combat use, etc.)",
                                  " 16 - Disciplines or special abilities", " 17 - Unused at this time",
                                  " 18 - Default text and stuff you type", " 19 - Unused at this time",
                                  " 20 - Merchant Offer Price", " Type /usercolor page3 for numbers 21 - 30"};
constexpr const char* kPage3[] = {" 21 - Merchant Buy/Sell", " 22 - Your death message", " 23 - Others death message",
                                  " 24 - Other damage other", " 25 - Other miss other", " 26 - /who command",
                                  " 27 - yell for help", " 28 - Hit for non-melee", " 29 - Spell worn off",
                                  " 30 - Money splits", " Type /usercolor page4 for numbers 31 - 40"};
constexpr const char* kPage4[] = {" 31 - Loot message", " 32 - Dice Roll (/random)", " 33 - Others spells",
                                  " 34 - Spell Failures (resists, fizzles, missing component, bad target, etc.)",
                                  " 35 - Chat Channel Messages", " 36 - Chat Channel 1", " 37 - Chat Channel 2",
                                  " 38 - Chat Channel 3", " 39 - Chat Channel 4", " 40 - Chat Channel 5",
                                  " Type /usercolor page5 for numbers 41 - 50"};
constexpr const char* kPage5[] = {" 41 - Chat Channel 6", " 42 - Chat Channel 7", " 43 - Chat Channel 8",
                                  " 44 - Chat Channel 9", " 45 - Chat Channel 10", " 46 - Melee Crits",
                                  " 47 - Spell Crits", " 48 - Too far away (melee)", " 49 - NPC Rampage",
                                  " 50 - NPC Flurry", " Type /usercolor page6 for numbers 51 and up"};
constexpr const char* kPage6[] = {" 51 - NPC Enrage", " 52 - Say echo", " 53 - Tell echo", " 54 - Group echo",
                                  " 55 - Guild echo", " 56 - OOC echo", " 57 - Auction echo", " 58 - Shout echo",
                                  " 59 - Emote echo", " 60 - Chat Channel 1 echo",
                                  " Type /usercolor page7 for numbers 61 and up"};
constexpr const char* kPage7[] = {" 61 - Chat Channel 2 echo", " 62 - Chat Channel 3 echo",
                                  " 63 - Chat Channel 4 echo", " 64 - Chat Channel 5 echo",
                                  " 65 - Chat Channel 6 echo", " 66 - Chat Channel 7 echo",
                                  " 67 - Chat Channel 8 echo", " 68 - Chat Channel 9 echo",
                                  " 69 - Chat Channel 10 echo"};

} // namespace

std::span<const char* const> usercolor_help_lines(int page) {
    switch (page) {
    case 1: return kPage1;
    case 2: return kPage2;
    case 3: return kPage3;
    case 4: return kPage4;
    case 5: return kPage5;
    case 6: return kPage6;
    case 7: return kPage7;
    default: return kUsage;
    }
}

UserColor usercolor_command(std::string_view args) {
    UserColor c;
    if (args.empty()) {
        c.page = kUserColorUsage;
        return c;
    }
    std::string words[4];
    std::string_view rest = args;
    for (auto& w : words) w = next_cmd_arg(rest).substr(0, 0x3f);
    if (is_numeric_string(words[0])) c.index = eq_atoi(words[0]);
    if (same_no_case(words[1], "reset")) {
        c.reset = true;
    } else if (same_no_case(words[0], "help") || same_no_case(words[0], "page1")) {
        c.page = 1;
        return c;
    } else {
        static constexpr const char* kPages[] = {"page2", "page3", "page4", "page5", "page6", "page7"};
        for (int p = 0; p < 6; ++p)
            if (same_no_case(words[0], kPages[p])) {
                c.page = p + 2;
                return c;
            }
        if (is_numeric_string(words[1])) {
            c.red = eq_atoi(words[1]);
            if (is_numeric_string(words[2])) c.green = eq_atoi(words[2]);
            if (is_numeric_string(words[3])) c.blue = eq_atoi(words[3]);
        }
    }
    // A reset takes the default colour later (the channels are not known here): only the index is checked then.
    if (c.reset) {
        if (static_cast<std::uint32_t>(c.index) > 0x63) c.page = kUserColorUsage;
        return c;
    }
    if (static_cast<std::uint32_t>(c.red) > 0xff || static_cast<std::uint32_t>(c.green) > 0xff ||
        static_cast<std::uint32_t>(c.blue) > 0xff || static_cast<std::uint32_t>(c.index) > 0x63)
        c.page = kUserColorUsage;
    return c;
}

// ---------------------------------------------------------------- /hotbutton

HotButtonCommand hotbutton_command(std::string_view args) {
    HotButtonCommand h;
    std::string_view after_name = args;
    h.name = next_cmd_arg(after_name).substr(0, 0x3f);
    std::string_view after_page = after_name;
    std::string page = next_cmd_arg(after_page).substr(0, 0x3f);
    if (h.name.empty()) {
        h.usage = true;
        return h;
    }
    bool digits = true;
    for (char c : page)
        if (!std::isdigit(static_cast<unsigned char>(c))) digits = false;
    // The rest after the word, as GetNextCmdArg leaves it: the spaces after a word are skipped by the next read only.
    auto trim = [](std::string_view s) {
        while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
        return std::string(s.substr(0, 0xff));
    };
    if (digits) {
        h.line = trim(after_page);
        h.color = eq_atoi(page);
    } else {
        h.line = trim(after_name);
    }
    return h;
}

// ---------------------------------------------------------------- /viewport

std::optional<Viewport> viewport_command(std::string_view args, std::int32_t screen_w, std::int32_t screen_h) {
    if (args.empty()) return std::nullopt;
    Viewport r;
    if (args.find("reset") != std::string_view::npos) {
        r.v = {0, 0, screen_w, screen_h, 0, 0, 100, 100};
        return r;
    }
    std::string words[5];
    std::string_view rest = args;
    for (auto& w : words) w = next_cmd_arg(rest).substr(0, 0x3f);
    std::int32_t x = -1, y = -1, width = -1, height = -1;
    bool scale = false;
    if (is_numeric_string(words[0])) x = eq_atoi(words[0]);
    if (is_numeric_string(words[1])) y = eq_atoi(words[1]);
    if (is_numeric_string(words[2])) width = eq_atoi(words[2]);
    if (is_numeric_string(words[3])) height = eq_atoi(words[3]);
    if (is_numeric_string(words[4])) scale = eq_atoi(words[4]) != 0;
    if (!(x > -1 && y > -1 && width > 0 && height > 0)) return std::nullopt;
    if (width > screen_w) width = screen_w;
    if (wrap_add(width, x) > screen_w) x = wrap_sub(screen_w, width);
    if (height > screen_h) height = screen_h;
    if (wrap_add(y, height) > screen_h) y = wrap_sub(screen_h, height);
    std::int32_t right = wrap_add(width, x), bottom = wrap_add(height, y);
    r.v = {x, y, right, bottom, 0, 0, 0, 0};
    if (scale) {
        r.v[4] = percent(x, screen_w);
        r.v[5] = percent(y, screen_h);
        r.v[6] = percent(wrap_add(wrap_sub(right, x), 1), screen_w);
        r.v[7] = percent(wrap_add(wrap_sub(bottom, y), 1), screen_h);
    }
    return r;
}

std::string viewport_line(const Viewport& v) {
    return std::format("x={} y={} w={} h={} scale={}", v.v[0], v.v[1], wrap_sub(v.v[2], v.v[0]),
                       wrap_sub(v.v[3], v.v[1]), v.scaled() ? 1 : 0);
}

std::array<std::pair<const char*, std::int32_t>, 8> viewport_ini(const Viewport& v) {
    return {{{"X", v.v[0]},
             {"Y", v.v[1]},
             {"W", wrap_sub(v.v[2], v.v[0])},
             {"H", wrap_sub(v.v[3], v.v[1])},
             {"XScale", v.v[4]},
             {"YScale", v.v[5]},
             {"WScale", v.v[6]},
             {"HScale", v.v[7]}}};
}

ViewRect viewport_rect(const Viewport& v, std::int32_t screen_w, std::int32_t screen_h) {
    ViewRect r;
    // The division is signed: a scale from a hand-edited display.viewport wraps the product (core/wrap_math.h).
    r.x = v.v[4] ? core::wrapped_mul_div(screen_w, v.v[4], 100) : v.v[0];
    r.y = v.v[5] ? core::wrapped_mul_div(screen_h, v.v[5], 100) : v.v[1];
    r.w = v.v[6] ? core::wrapped_mul_div(screen_w, v.v[6], 100) : wrap_sub(v.v[2], v.v[0]);
    r.h = v.v[7] ? core::wrapped_mul_div(screen_h, v.v[7], 100) : wrap_sub(v.v[3], v.v[1]);
    if (r.w < 1 || r.w > screen_w) r.w = screen_w;
    if (r.x < 0) r.x = 0;
    if (wrap_add(r.w, r.x) > screen_w) r.x = wrap_sub(screen_w, r.w);
    if (r.h < 1 || r.h > screen_h) r.h = screen_h;
    if (r.y < 0) r.y = 0;
    if (wrap_add(r.h, r.y) > screen_h) r.y = wrap_sub(screen_h, r.h);
    return r;
}

std::string viewport_text(const Viewport& v) {
    std::string text;
    for (std::int32_t x : v.v) text += (text.empty() ? "" : " ") + std::to_string(x);
    return text;
}

std::optional<Viewport> parse_viewport(std::string_view text) {
    Viewport v;
    std::string_view rest = text;
    for (auto& x : v.v) {
        std::string word = next_cmd_arg(rest);
        if (word.empty() || !is_numeric_string(word)) return std::nullopt;
        x = eq_atoi(word);
    }
    return v;
}

// ---------------------------------------------------------------- /particledensity

std::optional<int> particle_density(std::string_view args) {
    std::int32_t level = eq_atoi(args);
    if (is_numeric_string(args) && static_cast<std::uint32_t>(level) <= 3) return level;
    static constexpr const char* kNames[] = {"OFF", "LOW", "MED", "HIG"};
    for (int k = 0; k < 4; ++k)
        if (starts_no_case(args, kNames[k], 3)) return k;
    return std::nullopt;
}

std::string particle_density_line(std::string_view args) {
    return std::format("Particle Cloud Density is currently: {}", args.substr(0, 0xd0));
}

float particle_density_value(int level) {
    static constexpr float kValues[] = {0.0f, 0.25f, 0.5f, 1.0f};
    return level >= 0 && level <= 3 ? kValues[level] : 1.0f;
}

int particle_density_choice(float density) {
    if (density <= 0.0f) return 0;
    if (density <= 0.25f) return 1;
    if (density <= 0.5f) return 2;
    return 3;
}

// ---------------------------------------------------------------- tracking

std::optional<TrackFilterChoice> trackfilter_choice(std::string_view args, const EqStrFn& eqstr) {
    // Each filter bit with the letters that pick it (lower case, from the start of the argument) and its name's
    // eqstr; tried in this order.
    struct Filter {
        std::uint32_t bit;
        std::string_view prefix;
        std::uint32_t eqstr_id;
    };
    static constexpr Filter kFilters[] = {{0x01, "gn", 13156}, {0x40, "gy", 11005}, {0x02, "b", 13157},
                                          {0x04, "w", 13158},  {0x08, "y", 13160},  {0x10, "r", 13159},
                                          {0x20, "lb", 13161}};
    auto starts_with = [&](std::string_view prefix) {
        for (std::size_t i = 0; i < prefix.size(); ++i)
            if (lower(at(args, i)) != prefix[i]) return false;
        return true;
    };
    for (const Filter& f : kFilters)
        if (starts_with(f.prefix) || same_no_case(args, eqstr(f.eqstr_id))) return TrackFilterChoice{f.bit, f.eqstr_id};
    return std::nullopt;
}

std::optional<TrackSortChoice> tracksort_choice(std::string_view args, const EqStrFn& eqstr) {
    struct Option {
        char first, second;
        std::uint32_t name, message;
        const char* ini;
    };
    static constexpr Option kOptions[] = {{'N', 0, 13120, 13149, "NORMAL"},     {'A', 0, 6353, 6355, "ALPHA"},
                                          {'D', 0, 13144, 13150, "DISTANCE"},   {'C', 0, 13145, 13151, "CONSIDER"},
                                          {'R', 'A', 6354, 6356, "RALPHA"},     {'R', 'D', 13146, 13152, "RDISTANCE"},
                                          {'R', 'C', 13147, 13153, "RCONSIDER"}};
    std::uint32_t type = 0;
    for (const Option& o : kOptions) {
        bool letters = either_case(at(args, 0), o.first) && (!o.second || either_case(at(args, 1), o.second));
        if (letters || same_no_case(args, eqstr(o.name))) return TrackSortChoice{type, o.message, o.ini};
        ++type;
    }
    return std::nullopt;
}

std::int32_t track_players_value(const std::optional<std::string>& text) {
    if (!text || (!text->empty() && (*text)[0] == '*')) return 1; // missing ("*NULL*"): TRUE written
    char c = text->empty() ? '\0' : (*text)[0];
    if (c == 'T' || c == 't' || c == '1') return 1;
    if (c == 'G' || c == 'g' || c == '2') return 2;
    return 0;
}

bool track_toggle_value(const std::optional<std::string>& text) {
    if (!text || (!text->empty() && (*text)[0] == '*')) return true;
    char c = text->empty() ? '\0' : (*text)[0];
    return c == 'T' || c == 't' || c == '1';
}

std::uint32_t track_sort_value(const std::optional<std::string>& text) {
    if (!text || text->empty()) return 0;
    char c = (*text)[0];
    switch (c) {
    case 'N': case 'n': case '0': return 0;
    case 'A': case 'a': case '1': return 1;
    case 'D': case 'd': case '2': return 2;
    case 'C': case 'c': case '3': return 3;
    case '4': return 4;
    case '5': return 5;
    case '6': return 6;
    case 'R': case 'r': {
        char d = at(*text, 1);
        if (d == 'A' || d == 'a') return 4;
        if (d == 'D' || d == 'd') return 5;
        if (d == 'C' || d == 'c') return 6;
        return 0; // the constructor keeps the register it had (docs/unknowns.md); the normal sort here
    }
    default: return 0;
    }
}

// ---------------------------------------------------------------- sound

std::optional<std::int32_t> mixahead_value(std::string_view args) {
    if (args.empty()) return std::nullopt;
    std::int32_t v = eq_atoi(args);
    if (static_cast<std::uint32_t>(v) - 1u > 0x63) return std::nullopt;
    return v;
}

// ---------------------------------------------------------------- /netstats

std::vector<ChatOut> netstats_lines(const NetStats& s) {
    // The original's text goes to the chat window as STML: "&NBSP;" is a space there, and "\n" breaks the line.
    std::vector<ChatOut> out;
    auto add = [&](std::string text, std::uint32_t color) {
        for (std::size_t at = 0; (at = text.find("&NBSP;", at)) != std::string::npos;) text.replace(at, 6, " ");
        std::size_t start = 0;
        for (;;) {
            std::size_t nl = text.find('\n', start);
            out.push_back({text.substr(start, nl == std::string::npos ? std::string::npos : nl - start), color});
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    };
    add("", 0xa);
    add("\nPing Times", 0xf);
    add(std::format("&NBSP;&NBSP;&NBSP;master={}", s.master), 0xa);
    add(std::format("&NBSP;&NBSP;&NBSP;average={}", s.average), 0xa);
    add(std::format("&NBSP;&NBSP;&NBSP;low={}", s.low), 0xa);
    add(std::format("&NBSP;&NBSP;&NBSP;high={}", s.high), 0xa);
    add(std::format("&NBSP;&NBSP;&NBSP;last={}", s.last), 0xa);
    add(std::format("&NBSP;&NBSP;&NBSP;reliable avg={}", s.reliable_average), 0xa);
    add("", 0xa);
    add("\nPacket Loss", 0xf);
    add(std::format("&NBSP;&NBSP;&NBSP;sent={:.2f}%\n", (1.0 - static_cast<double>(s.sent_success)) * 100.0), 0xa);
    add(std::format("&NBSP;&NBSP;&NBSP;rcvd={:.2f}%\n", (1.0 - static_cast<double>(s.received_success)) * 100.0), 0xa);
    add("", 0xa);
    return out;
}

} // namespace hailstone::game
