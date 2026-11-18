#include "render/gpu_timer.h"

namespace hailstone::render {

void gpu_sections(std::span<const char* const> names, std::span<const std::uint64_t> ticks, float period_ns,
                  std::uint32_t valid_bits, std::vector<GpuSection>& out) {
    out.clear();
    if (!(period_ns > 0.0f) || valid_bits == 0 || ticks.size() != names.size() + 1) return;
    const std::uint64_t mask = valid_bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << valid_bits) - 1u;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::uint64_t delta = (ticks[i + 1] - ticks[i]) & mask;
        const float ms = static_cast<float>(static_cast<double>(delta) * static_cast<double>(period_ns) / 1.0e6);
        bool merged = false;
        for (auto& s : out)
            if (s.name == names[i]) {
                s.ms += ms;
                merged = true;
                break;
            }
        if (!merged) out.push_back({names[i], ms});
    }
}

} // namespace hailstone::render
