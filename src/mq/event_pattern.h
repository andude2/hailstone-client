#pragma once

// An mq.event pattern, matched as MacroQuest matches it: MQ's Lua events feed each chat line to Blech (Lax/Blech
// 1.7.4, contrib/Blech/Blech.h, with '#' the scan and '|' the print delimiter; plugins/lua/LuaEvent.cpp), read as
// the spec, not copied.
//
//  - #name# takes text from the line: #*#, #1#, #2#, ... or any other name (#target#). Only a whole number above 0
//    is handed to the script; the others match and are dropped. A '#' with no closing one makes the rest of the
//    pattern a name, so it takes the rest of the line.
//  - |text| is the text's ${...} parsed when a line comes (MQ's LuaVarProcess); outside it, `##` is a literal '#'
//    and `||` a literal '|'.
//  - The rest is literal, matched ignoring the case of A-Z.
//  - A literal before the first #...# must start the line; with no #...# at all the line must be the pattern.
//    Each #...# takes text up to the first place the literal after it is found (none: up to where the next #...#
//    starts, so the first of two adjacent ones gets ""). The last one takes the rest of the line; the literal after
//    it must be found first at the very end. So #*# takes as little as the line allows too: "#*# says #1#" on
//    "A says B says C" gives #1# "B says C".
//  - A line break is a character like any other, and an empty line matches nothing.
//
// One deliberate difference: Blech keeps all of a script's events in one tree of shared prefixes and walks it once
// per line; where two patterns share a literal's beginning, the walk can turn down a line that pattern alone would
// match. Here each pattern is matched on its own, as its text says.
//
// A #n# above kMaxArg is not handed on (a deliberate difference: MQ makes a table of n arguments, and #2000000000#
// runs it out of memory).
//
// A match is one pass over the pattern: a string search for each literal in what is left of the line, never trying
// another place for an earlier one (no backtracking, so no regex blowup on a line built to be slow).

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone::mq {

class EventPattern {
public:
    static constexpr int kMaxArg = 255;

    explicit EventPattern(std::string_view pattern);

    // The text a |name| stands for now (the script's ${...} parsed); null: the name as written.
    using Resolve = std::function<std::string(std::string_view name)>;
    bool has_print_vars() const { return print_vars_; }

    // When the line matches: what the callback gets after the line, MQ's way: #n#'s text at [n - 1], "" for a number
    // the pattern lacks, up to the highest #n# (a number used twice: its last). Empty without any #n#.
    std::optional<std::vector<std::string>> match(std::string_view line, const Resolve& resolve = {}) const;
    // The same, given fold(line) too: a caller matching one line against many patterns folds it once.
    std::optional<std::vector<std::string>> match(std::string_view line, std::string_view low,
                                                  const Resolve& resolve = {}) const;
    // The line as patterns compare it (A-Z to a-z, other bytes as they are).
    static std::string fold(std::string_view line);

    // The pattern's bytes as written (what a script's events are held to, mq/lua_script.h).
    std::size_t size() const { return size_; }

private:
    struct Token {
        enum class Kind { Literal, Print, Scan } kind = Kind::Literal;
        std::string text; // a literal's text (A-Z folded to a-z), a print variable's name
        int number = 0;   // a scan variable's #n# (0: not handed on)
    };
    std::vector<Token> tokens_;
    int max_number_ = 0;
    bool print_vars_ = false;
    std::size_t size_ = 0;
};

} // namespace hailstone::mq
