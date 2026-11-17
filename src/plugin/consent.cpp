#include "plugin/consent.h"

#include <algorithm>
#include <atomic>
#include <sstream>

#include "core/ascii_case.h"
#include "core/atomic_file.h"

namespace hailstone::plugin {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kHeader = "hailstone-plugin-approvals 1";

fs::path approvals_file(const fs::path& profile_dir) { return profile_dir / kApprovalsName; }

bool same_name(std::string_view a, std::string_view b) { return iequals(a, b); }

std::atomic<std::uint32_t> g_approvals_generation{0};

} // namespace

std::uint32_t plugin_approvals_generation() { return g_approvals_generation.load(std::memory_order_acquire); }

std::vector<PluginApproval> read_plugin_approvals(const fs::path& profile_dir) {
    std::vector<PluginApproval> out;
    if (profile_dir.empty()) return out;
    auto text = read_text_file(approvals_file(profile_dir));
    if (!text) return out;
    std::istringstream in(*text);
    bool header = false;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (!header) {
            if (line != kHeader) return out; // not one this version reads: nothing is taken as allowed
            header = true;
            continue;
        }
        // "allow|deny <sha256> <name>" (the name last: it may hold spaces)
        const std::size_t a = line.find(' ');
        if (a == std::string::npos) continue;
        const std::size_t b = line.find(' ', a + 1);
        if (b == std::string::npos) continue;
        const std::string_view word = std::string_view(line).substr(0, a);
        if (word != "allow" && word != "deny") continue;
        auto hash = parse_sha256(std::string_view(line).substr(a + 1, b - a - 1));
        std::string name = line.substr(b + 1);
        if (!hash || name.empty()) continue;
        out.push_back({std::move(name), *hash, word == "allow"});
    }
    return out;
}

bool write_plugin_approvals(const fs::path& profile_dir, const std::vector<PluginApproval>& approvals) {
    if (profile_dir.empty()) return false;
    std::string text = "# Your answers to this server's compiled plugins (docs/plugins.md). Written by the client.\n";
    text += kHeader;
    text += "\n";
    for (const auto& p : approvals)
        text += std::string(p.allowed ? "allow " : "deny ") + to_hex(p.sha256) + " " + p.name + "\n";
    const bool ok = write_file_atomic(approvals_file(profile_dir), text);
    g_approvals_generation.fetch_add(1, std::memory_order_acq_rel); // even a failed write may have changed the file
    return ok;
}

Consent consent_in(const std::vector<PluginApproval>& approvals, const Sha256Digest& hash) {
    Consent c = Consent::Unasked;
    for (const auto& p : approvals)
        if (p.sha256 == hash) c = p.allowed ? Consent::Allowed : Consent::Denied; // the last line wins
    return c;
}

Consent plugin_consent(const fs::path& profile_dir, const Sha256Digest& hash) {
    return consent_in(read_plugin_approvals(profile_dir), hash);
}

bool set_plugin_consent(const fs::path& profile_dir, std::string_view name, const Sha256Digest& hash, bool allowed) {
    auto all = read_plugin_approvals(profile_dir);
    std::erase_if(all, [&](const PluginApproval& p) { return same_name(p.name, name) || p.sha256 == hash; });
    all.push_back({std::string(name), hash, allowed});
    return write_plugin_approvals(profile_dir, all);
}

bool revoke_plugin_consent(const fs::path& profile_dir, std::string_view name, const std::vector<Sha256Digest>& hashes) {
    auto all = read_plugin_approvals(profile_dir);
    auto listed = [&](const Sha256Digest& h) { return std::find(hashes.begin(), hashes.end(), h) != hashes.end(); };
    for (auto& p : all)
        if (same_name(p.name, name) || listed(p.sha256)) p.allowed = false;
    for (const auto& h : hashes)
        if (std::none_of(all.begin(), all.end(), [&](const PluginApproval& p) { return p.sha256 == h; }))
            all.push_back({std::string(name), h, false});
    return write_plugin_approvals(profile_dir, all);
}

bool forget_plugin_consent(const fs::path& profile_dir, std::string_view name, const std::vector<Sha256Digest>& hashes) {
    auto all = read_plugin_approvals(profile_dir);
    std::erase_if(all, [&](const PluginApproval& p) {
        return same_name(p.name, name) || std::find(hashes.begin(), hashes.end(), p.sha256) != hashes.end();
    });
    return write_plugin_approvals(profile_dir, all);
}

} // namespace hailstone::plugin
