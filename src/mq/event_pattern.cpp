#include "mq/event_pattern.h"

#include <array>
#include <charconv>

namespace hailstone::mq {

namespace {

// Blech compares ignoring the case of A-Z only (its own Equalness, _strnicmp in the C locale); other bytes as they are.
char fold(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string folded(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = fold(c);
    return out;
}

// The number LuaEvent hands a #name# on as (GetIntFromString: the name trimmed of " \t\r\n", then as many digits as
// std::from_chars reads, "2nd" being 2); 0 when it is none, not above 0 or above kMaxArg.
int scan_number(std::string_view name) {
    const std::size_t b = name.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return 0;
    name.remove_prefix(b);
    name = name.substr(0, name.find_last_not_of(" \t\r\n") + 1);
    int n = 0;
    std::from_chars(name.data(), name.data() + name.size(), n);
    return n > 0 && n <= EventPattern::kMaxArg ? n : 0;
}

} // namespace

EventPattern::EventPattern(std::string_view pattern) : size_(pattern.size()) {
    // Blech takes the pattern as a C string: it ends at a NUL.
    pattern = pattern.substr(0, pattern.find('\0'));
    using K = Token::Kind;
    K mode = K::Literal;
    std::string part;
    // A run is added only when it holds something (Blech's AddEvent): a lone '#' at the end is dropped.
    auto flush = [&] {
        if (part.empty()) return;
        if (mode == K::Literal) {
            if (!tokens_.empty() && tokens_.back().kind == K::Literal) tokens_.back().text += folded(part);
            else tokens_.push_back({K::Literal, folded(part), 0});
        } else if (mode == K::Print) {
            tokens_.push_back({K::Print, part, 0});
            print_vars_ = true;
        } else {
            const int n = scan_number(part);
            tokens_.push_back({K::Scan, part, n});
            if (n > max_number_) max_number_ = n;
        }
        part.clear();
    };
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        const char next = i + 1 < pattern.size() ? pattern[i + 1] : '\0';
        if (c == '#' || c == '|') {
            if (mode == K::Literal && next == c) { // ## and || are the character itself
                part += c;
                ++i;
                continue;
            }
            flush();
            const K var = c == '#' ? K::Scan : K::Print;
            mode = mode == var ? K::Literal : var; // (a '#' inside |...| ends it and starts a #...#, as in Blech)
            continue;
        }
        part += c;
    }
    flush();
}

std::string EventPattern::fold(std::string_view line) { return folded(line); }

std::optional<std::vector<std::string>> EventPattern::match(std::string_view line, const Resolve& resolve) const {
    const std::string low = folded(line);
    return match(line, low, resolve);
}

std::optional<std::vector<std::string>> EventPattern::match(std::string_view line, std::string_view low,
                                                            const Resolve& resolve) const {
    line = line.substr(0, line.find('\0')); // a C string to Blech too
    if (line.empty() || tokens_.empty() || low.size() < line.size()) return std::nullopt; // (Blech is not fed an empty line; MQ crashes on "")
    low = low.substr(0, line.size());
    // Where each #n# was found, made into the arguments only once the whole line matches.
    struct Span {
        std::size_t at = 0, size = 0;
    };
    std::array<Span, kMaxArg> spans{};
    const Token* pending = nullptr; // the #...# whose end is not known yet
    std::size_t pos = 0;            // where the line is matched from
    std::string fixed;              // the literal text since it (literals and |...| values together)
    auto take = [&](std::size_t end) {
        if (pending->number > 0) spans[static_cast<std::size_t>(pending->number - 1)] = {pos, end - pos};
    };
    auto result = [&]() {
        std::vector<std::string> args(static_cast<std::size_t>(max_number_));
        for (std::size_t i = 0; i < args.size(); ++i) args[i] = std::string(line.substr(spans[i].at, spans[i].size));
        return std::optional(std::move(args));
    };
    for (const Token& k : tokens_) {
        if (k.kind == Token::Kind::Literal) {
            fixed += k.text;
        } else if (k.kind == Token::Kind::Print) {
            fixed += folded(resolve ? resolve(k.text) : k.text);
        } else {
            if (pending) {
                if (fixed.empty()) {
                    take(pos); // two #...# with nothing between: the first gets ""
                } else {
                    const std::size_t at = low.find(fixed, pos); // the first place it is
                    if (at == std::string::npos) return std::nullopt;
                    take(at);
                    pos = at + fixed.size();
                }
            } else {
                if (low.compare(pos, fixed.size(), fixed) != 0) return std::nullopt; // the start of the line
                pos += fixed.size();
            }
            fixed.clear();
            pending = &k;
            continue;
        }
        // Literal text longer than what is left of the line can be nowhere in it (a |...| value grown large).
        if (fixed.size() > line.size() - pos) return std::nullopt;
    }
    if (!pending) return low == fixed ? result() : std::nullopt;
    if (fixed.empty()) {
        take(line.size());
        return result();
    }
    // Blech's walk takes the first place the last literal is, and that must end the line.
    const std::size_t at = low.find(fixed, pos);
    if (at == std::string::npos || at + fixed.size() != line.size()) return std::nullopt;
    take(at);
    return result();
}

} // namespace hailstone::mq
