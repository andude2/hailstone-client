#include "app/addon_upscaler_policy.h"
#include "app/client_app_internal.h"

namespace hailstone::app::client_internal {

std::string ClientApp::upscaler_addon_problem(const plugin::Rules& rules) const {
    if (!rules.addons.enabled) return {};
    const auto* provider = vk_.upscaler_provider();
    return addon_upscaler_problem(rules.addons, read_addon_registry(platform::data_dir()),
                                 provider ? std::string_view(provider->name()) : std::string_view{},
                                 provider ? provider->loaded_folder() : std::filesystem::path{});
}

} // namespace hailstone::app::client_internal
