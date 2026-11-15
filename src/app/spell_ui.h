#pragma once

// Glue between net::GameConnection's spell state and the spell windows
// (ui/spell_windows.h): builds the view models each frame, draws them and
// passes the player's clicks back to the connection.

#include <cstdint>
#include <memory>
#include <string>

#include "ui/spell_windows.h"

namespace hailstone { class GameDir; }
namespace hailstone::render { class VkContext; }
namespace hailstone::net { class GameConnection; }
namespace hailstone::game { struct SpellData; class EqStrings; }
namespace hailstone::game::mechanics { class Character; }

namespace hailstone::app {

class SpellUi {
public:
    SpellUi(render::VkContext& vk, const GameDir& game);
    ~SpellUi();

    // `character` (the stats port) gives the descriptions CalcAffectChange and SpellDuration.
    void draw(net::GameConnection& conn, const game::mechanics::Character* character = nullptr);
    // Asks the connection to open or close the book on the next draw
    // (GameConnection::open_spell_book / close_spell_book).
    void toggle_book() { toggle_ = !toggle_; }
    bool book_open() const { return book_.open; }

private:
    std::unique_ptr<ui::SpellIconCache> icons_;
    ui::SpellBookView book_;
    bool toggle_ = false;
};

// Who reads a description: the player's level and class and the stats port
// (game/spell_description.h).
struct DescriptionPlayer {
    const game::mechanics::Character* character = nullptr;
    int level = 1;
    std::uint8_t class_id = 0;
};
DescriptionPlayer description_player(const net::GameConnection& conn, const game::mechanics::Character* character);

// Which of SpellUi's C++ windows draw this frame: each is off while its Lua window stands in for it
// (ui::lua::replaces), and the book also needs to be open. SpellUi::draw builds a window's view (names, tooltips,
// buff texts) only when it draws; a UI pack that replaces them all builds none.
struct SpellWindowsDrawn {
    bool bar = true;    // CastSpellWnd
    bool cast = true;   // CastingWindow
    bool book = true;   // SpellBookWnd, open
    bool buffs = true;  // BuffWindow
    bool songs = true;  // ShortDurationBuffWindow
    bool target = true; // TargetWindow (the target's effects)
};
SpellWindowsDrawn spell_windows_drawn(bool book_open);

// The tooltip of a spell: name, level, mana, times, target and the dbstr
// description with its codes filled in as GetSpellDescriptionText does
// (game::spell_description_text).
std::string spell_tooltip(const game::SpellData& spell, const game::EqStrings& strings, int level_needed,
                          const DescriptionPlayer& player = {});
std::string spell_description(const game::SpellData& spell, const game::EqStrings& strings,
                              const DescriptionPlayer& player = {});

} // namespace hailstone::app
