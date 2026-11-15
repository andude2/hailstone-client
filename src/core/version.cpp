#include "core/version.h"

#include <charconv>

namespace hailstone {

namespace {

bool all_digits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// A numeric field: digits, no leading zero (SemVer 2), within 32 bits.
std::optional<std::uint32_t> number(std::string_view s) {
    if (!all_digits(s) || (s.size() > 1 && s[0] == '0')) return std::nullopt;
    std::uint32_t n = 0;
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (ec != std::errc{} || end != s.data() + s.size()) return std::nullopt;
    return n;
}

bool identifier(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-')) return false;
    return true;
}

} // namespace

std::optional<Version> parse_version(std::string_view text) {
    if (!text.empty() && (text[0] == 'v' || text[0] == 'V')) text.remove_prefix(1);
    if (auto plus = text.find('+'); plus != std::string_view::npos) {
        // Ignored for precedence, but checked as SemVer writes it (dot-separated [0-9A-Za-z-] identifiers, leading
        // zeros allowed): an add-on's unsigned manifest must not carry control characters or other bytes into
        // installed.txt, the log and the UI through it.
        std::string_view build = text.substr(plus + 1);
        while (true) {
            const auto dot = build.find('.');
            if (!identifier(build.substr(0, dot))) return std::nullopt;
            if (dot == std::string_view::npos) break;
            build = build.substr(dot + 1);
        }
        text = text.substr(0, plus);
    }
    std::string_view core = text, pre;
    if (auto dash = text.find('-'); dash != std::string_view::npos) {
        core = text.substr(0, dash);
        pre = text.substr(dash + 1);
        if (pre.empty()) return std::nullopt;
    }
    Version v;
    std::uint32_t* parts[] = {&v.major, &v.minor, &v.patch};
    for (int i = 0; i < 3; ++i) {
        auto dot = core.find('.');
        if ((i < 2) == (dot == std::string_view::npos)) return std::nullopt;
        auto n = number(core.substr(0, dot));
        if (!n) return std::nullopt;
        *parts[i] = *n;
        core = i < 2 ? core.substr(dot + 1) : std::string_view{};
    }
    while (!pre.empty()) {
        auto dot = pre.find('.');
        std::string_view field = pre.substr(0, dot);
        if (!identifier(field) || (all_digits(field) && !number(field))) return std::nullopt;
        v.pre.emplace_back(field);
        if (dot == std::string_view::npos) break;
        pre = pre.substr(dot + 1);
        if (pre.empty()) return std::nullopt;
    }
    return v;
}

int compare_versions(const Version& a, const Version& b) {
    auto cmp = [](std::uint32_t x, std::uint32_t y) { return x < y ? -1 : (x > y ? 1 : 0); };
    if (int c = cmp(a.major, b.major)) return c;
    if (int c = cmp(a.minor, b.minor)) return c;
    if (int c = cmp(a.patch, b.patch)) return c;
    if (a.pre.empty() || b.pre.empty()) return a.pre.empty() == b.pre.empty() ? 0 : (a.pre.empty() ? 1 : -1);
    for (std::size_t i = 0; i < a.pre.size() && i < b.pre.size(); ++i) {
        const std::string& x = a.pre[i];
        const std::string& y = b.pre[i];
        const bool xn = all_digits(x), yn = all_digits(y);
        if (xn && yn) {
            // Equal-length digit strings without leading zeros compare as text; a longer one is larger.
            if (x.size() != y.size()) return x.size() < y.size() ? -1 : 1;
            if (int c = x.compare(y)) return c < 0 ? -1 : 1;
        } else if (xn != yn) {
            return xn ? -1 : 1;
        } else if (int c = x.compare(y)) {
            return c < 0 ? -1 : 1;
        }
    }
    return cmp(static_cast<std::uint32_t>(a.pre.size()), static_cast<std::uint32_t>(b.pre.size()));
}

std::string Version::to_string() const {
    std::string s = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    for (std::size_t i = 0; i < pre.size(); ++i) s += (i == 0 ? "-" : ".") + pre[i];
    return s;
}

} // namespace hailstone
