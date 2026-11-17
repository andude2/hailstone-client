#pragma once

// One of the Ability Book's entries as the plugin bulk read's rows (get_ability_data_bulk): what app::ability_bulk_rows
// gives plugin::NativeEnv::ability_bulk, and native_plugins.cpp writes as an HS_Bulk_AbilityRecord. The row needs
// neither the SDK nor the app's ability model, so both sides can include this.

#include <cstdint>
#include <string>

#include "mq/tlo.h"

namespace hailstone::plugin {

struct AbilityBulkRow {
    mq::AbilityKind kind = mq::AbilityKind::Skill;
    std::int32_t id = 0;           // a skill's id, a discipline's spell id, an AA's ability (group) id
    bool ready = false;            // its reuse is over (not passive: the rows are usable entries only)
    std::int64_t remaining_ms = 0; // until ready
    std::int64_t reuse_ms = 0;     // its reuse time
    std::string name;
};

} // namespace hailstone::plugin
