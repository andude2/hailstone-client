#pragma once

#include "net/item_packets.h"

namespace hailstone::net {

// EQEmu's ItemVerifyRequest uses the carrier's own click when present, then
// looks for a clickable augment in socket order. The request still names the
// carrier's slot, even when the spell and reuse data come from an augment.
inline const Item& item_click_source(const Item& item) {
    if (item.item_class != 0 || item.effects[kEffectClick].present()) return item;
    constexpr std::uint32_t sockets = 6;
    for (std::uint32_t socket = 0; socket < sockets; ++socket) {
        const Item* augment = item.content(socket);
        if (!augment) continue;
        const auto& click = augment->effects[kEffectClick];
        // Click, Expendable, EquipClick and Click2 (EQEmu common/item_data.h).
        if (click.type == 1 || (click.type >= 3 && click.type <= 5)) return *augment;
    }
    return item;
}

inline Item& item_click_source(Item& item) {
    return const_cast<Item&>(item_click_source(static_cast<const Item&>(item)));
}

} // namespace hailstone::net
