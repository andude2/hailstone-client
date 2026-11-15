// mq/event_pattern.h: mq.event's patterns, matched as MacroQuest's Lua events match them (Blech, '#' scan and '|'
// print delimiters; plugins/lua/LuaEvent.cpp hands on #n# by number).
#include <doctest/doctest.h>

#include <random>
#include <string>
#include <vector>

#include "mq/event_pattern.h"

using namespace hailstone;

namespace {

using Args = std::vector<std::string>;

} // namespace

TEST_CASE("mq.event patterns: Blech's wildcards take as little as the line allows, the last one the rest") {
    mq::EventPattern slain("#*#You have slain #1#!#*#");
    CHECK(slain.match("You have slain a rat!") == Args{"a rat"});
    CHECK(slain.match("[x] You have slain a rat! Well done!") == Args{"a rat"});
    CHECK_FALSE(slain.match("You have slain a rat"));

    // #n# hands its text on by number: fn(line, arg1, arg2, arg3) whatever their order in the pattern.
    mq::EventPattern hits("#2# hits #1# for #3# points.");
    CHECK(hits.match("A gnoll hits you for 12 points.") == Args{"you", "A gnoll", "12"});
    CHECK(hits.match("A hits B hits C for 1 points.") == Args{"B hits C", "A", "1"});
    CHECK_FALSE(hits.match("A gnoll hits you for 12 points"));

    // #*# is no greedier than #n#: the first ' ' ends it.
    CHECK(mq::EventPattern("#*# #1#").match("a b c") == Args{"b c"});
    CHECK(mq::EventPattern("#1# #2#").match("a b c") == Args{"a", "b c"});
    CHECK(mq::EventPattern("#*# says #1#").match("A says B says C") == Args{"B says C"});

    // The last literal must be found first at the very end of the line.
    CHECK(mq::EventPattern("#1#!").match("hi!") == Args{"hi"});
    CHECK_FALSE(mq::EventPattern("#1#!").match("hi! there!"));
    CHECK_FALSE(mq::EventPattern("#*#hello").match("hello hello"));
    CHECK(mq::EventPattern("#*#hello").match("well, hello") == Args{});
}

TEST_CASE("mq.event patterns: the start, the whole line, case and line breaks") {
    // A literal before the first #...# starts the line; without any #...# the line is the pattern.
    CHECK_FALSE(mq::EventPattern("You hit #1#").match("[x] You hit a rat"));
    CHECK(mq::EventPattern("You hit #1#").match("You hit a rat") == Args{"a rat"});
    CHECK(mq::EventPattern("Hello there").match("hello THERE") == Args{});
    CHECK_FALSE(mq::EventPattern("Hello").match("Hello there"));
    CHECK_FALSE(mq::EventPattern("Hello").match("Hello Hello"));
    // A-Z ignore their case; the text handed on keeps the line's.
    CHECK(mq::EventPattern("#1# TELLS YOU, '#2#'").match("Bob tells you, 'Hi'") == Args{"Bob", "Hi"});
    // A literal may end at the line's end with the #...# after it empty.
    CHECK(mq::EventPattern("abc#1#").match("abc") == Args{""});
    CHECK(mq::EventPattern("#1#").match("whole line") == Args{"whole line"});
    // A line break is a character like another.
    CHECK(mq::EventPattern("#1#").match("a\nb") == Args{"a\nb"});
    CHECK(mq::EventPattern("#*#").match("a\rb") == Args{});
    // An empty line matches nothing, an empty pattern nothing (MQ's Blech would not even take one).
    CHECK_FALSE(mq::EventPattern("#*#").match(""));
    CHECK_FALSE(mq::EventPattern("").match(""));
    CHECK_FALSE(mq::EventPattern("").match("x"));
    // Both are C strings to Blech: they end at a NUL.
    CHECK(mq::EventPattern(std::string_view("hi\0there", 8)).match("hi") == Args{});
    CHECK(mq::EventPattern("hi").match(std::string_view("hi\0there", 8)) == Args{});
}

TEST_CASE("mq.event patterns: numbers, other names, adjacent variables and the delimiters as text") {
    // Gaps are "", and as many arguments as the highest number.
    CHECK(mq::EventPattern("#3# and #1#").match("x and y") == Args{"y", "", "x"});
    // A name that is no number above 0 matches and is not handed on; "2nd" reads as 2 (GetIntFromString).
    CHECK(mq::EventPattern("#who# hits #1#").match("A rat hits you") == Args{"you"});
    CHECK(mq::EventPattern("#0# #-1# #1#").match("a b c") == Args{"c"});
    CHECK(mq::EventPattern("# 2 # #2nd#").match("a b") == Args{"", "b"});
    // A number twice: the last.
    CHECK(mq::EventPattern("#1# then #1#").match("a then b") == Args{"b"});
    // Two #...# together: the first gets "".
    CHECK(mq::EventPattern("#1##2#").match("abc") == Args{"", "abc"});
    // ## and || are the character; a lone '#' opens a name to the pattern's end.
    CHECK(mq::EventPattern("##1 #1#").match("#1 x") == Args{"x"});
    CHECK(mq::EventPattern("a||b #1#").match("a|b c") == Args{"c"});
    CHECK(mq::EventPattern("Cost: 5# of it").match("Cost: 5 whatever") == Args{});
    CHECK_FALSE(mq::EventPattern("Cost: 5# of it").match("Cost: 6"));
    // A '#' alone at the end is dropped (an empty run).
    CHECK(mq::EventPattern("abc#").match("abc") == Args{});
    // Past kMaxArg a number is not handed on (MQ would make a table that long).
    CHECK(mq::EventPattern("#2000000000# #1#").match("a b") == Args{"b"});
    CHECK(mq::EventPattern("#255#").match("a")->size() == 255);
    CHECK(mq::EventPattern("#256#").match("a") == Args{});
}

TEST_CASE("mq.event patterns: |text| is what the script's ${...} gives when the line comes") {
    mq::EventPattern p("#1# tells you, '|${Me.Name}|'");
    CHECK(p.has_print_vars());
    CHECK_FALSE(mq::EventPattern("#1#").has_print_vars());
    auto resolve = [](std::string_view text) { return text == "${Me.Name}" ? std::string("Soandso") : std::string(text); };
    CHECK(p.match("Bob tells you, 'Soandso'", resolve) == Args{"Bob"});
    CHECK(p.match("Bob tells you, 'soandso'", resolve) == Args{"Bob"}); // as literal text: case ignored
    CHECK_FALSE(p.match("Bob tells you, 'Other'", resolve));
    // Without a resolver (the script may not read): the text as written.
    CHECK(p.match("Bob tells you, '${Me.Name}'") == Args{"Bob"});
    // An empty value takes no room.
    auto empty = [](std::string_view) { return std::string(); };
    CHECK(mq::EventPattern("#1#|x|#2#").match("abc", empty) == Args{"", "abc"});
    CHECK(mq::EventPattern("a|x|b").match("ab", empty) == Args{});
    // Inside |...| a '#' ends it and starts a #...#.
    CHECK(mq::EventPattern("|a#1#").match("ax") == Args{"x"});
}

TEST_CASE("mq.event patterns: random patterns and lines agree with a plain model of Blech's matching") {
    // The model: each #...# found in turn by trying every end for it left to right, and taking the first end at
    // which the literal after it starts (the last literal: found first at the line's end).
    struct Part {
        bool var;
        std::string text;
        int n;
    };
    auto lower = [](std::string s) {
        for (char& c : s) c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
        return s;
    };
    auto model = [&](const std::vector<Part>& parts, const std::string& line) -> std::optional<Args> {
        if (line.empty()) return std::nullopt;
        const std::string low = lower(line);
        int max_n = 0;
        for (const auto& p : parts)
            if (p.var && p.n > max_n) max_n = p.n;
        Args args(static_cast<std::size_t>(max_n));
        std::size_t pos = 0;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            const Part& p = parts[i];
            if (!p.var) {
                if (low.compare(pos, p.text.size(), lower(p.text)) != 0) return std::nullopt;
                pos += p.text.size();
                continue;
            }
            const bool last = i + 1 == parts.size();
            std::size_t end = line.size();
            if (!last) {
                if (parts[i + 1].var) {
                    end = pos;
                } else {
                    const std::string lit = lower(parts[i + 1].text);
                    end = std::string::npos;
                    for (std::size_t e = pos; e + lit.size() <= line.size(); ++e)
                        if (low.compare(e, lit.size(), lit) == 0) {
                            end = e;
                            break;
                        }
                    if (end == std::string::npos) return std::nullopt;
                    if (i + 2 == parts.size() && end + lit.size() != line.size()) return std::nullopt;
                }
            }
            if (p.n > 0) args[static_cast<std::size_t>(p.n - 1)] = line.substr(pos, end - pos);
            pos = end;
        }
        if (pos != line.size()) return std::nullopt;
        return args;
    };
    std::mt19937 rng(20261111);
    const std::vector<Part> pool{{true, "*", 0}, {true, "1", 1}, {true, "2", 2}, {true, "x", 0},  {false, "a", 0},
                                 {false, "B", 0}, {false, " ", 0}, {false, "ab", 0}, {false, "\n", 0}, {false, "!", 0}};
    const std::string alphabet = "abAB !\n";
    int matched = 0;
    for (int round = 0; round < 6000; ++round) {
        std::vector<Part> parts;
        std::string pattern;
        for (int k = 1 + static_cast<int>(rng() % 5); k > 0; --k) {
            const Part& p = pool[rng() % pool.size()];
            // Adjacent literals are one literal to both; keep them apart so the model's parts are the pattern's.
            if (!p.var && !parts.empty() && !parts.back().var) continue;
            parts.push_back(p);
            pattern += p.var ? "#" + p.text + "#" : p.text;
        }
        std::string line;
        for (int k = static_cast<int>(rng() % 9); k > 0; --k) line += alphabet[rng() % alphabet.size()];
        INFO("pattern '" << pattern << "' line '" << line << "'");
        const auto want = model(parts, line);
        CHECK(mq::EventPattern(pattern).match(line) == want);
        matched += want.has_value();
    }
    CHECK(matched > 300);
}

TEST_CASE("mq.event patterns: many arguments on a long line that does not match take no time") {
    mq::EventPattern p("#1# #2# #3# #4# #5#!");
    const std::string spaces(4000, ' ');
    CHECK_FALSE(p.match(spaces));
    CHECK(p.match(spaces + "!"));
}

TEST_CASE("mq.event patterns: a line folded once by the caller matches as the line alone; huge |...| values fail fast") {
    mq::EventPattern hits("#2# hits #1# for #3# points.");
    const std::string line = "A Gnoll HITS you for 12 points.";
    const std::string low = mq::EventPattern::fold(line);
    CHECK(low == "a gnoll hits you for 12 points.");
    CHECK(hits.match(line, low) == hits.match(line));
    CHECK(hits.match(line, low) == Args{"you", "A Gnoll", "12"});
    // A NUL ends the line for both.
    const std::string nul = std::string("A hits B for 1 points.") + '\0' + "junk";
    CHECK(hits.match(nul, mq::EventPattern::fold(nul)) == Args{"B", "A", "1"});
    // A |...| value longer than the line: no match, and the walk stops there.
    mq::EventPattern big("#*#|x|#*#");
    int resolved = 0;
    auto resolve = [&](std::string_view) {
        ++resolved;
        return std::string(5000, 'a');
    };
    CHECK_FALSE(big.match(std::string(100, 'a'), resolve));
    CHECK(resolved == 1);
    mq::EventPattern fits("#*#|x|#*#");
    CHECK(fits.match("xx aaa yy", [](std::string_view) { return std::string("AAA"); }) == Args{});
}
