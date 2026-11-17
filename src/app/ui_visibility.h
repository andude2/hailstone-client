#pragma once
// Whether the interface is drawn this frame. F10 (Toggle UI Display) hides it in the world only; the pre-game and
// loading screens, and a zone still settling, always show it. A question only the player may answer (the server's
// compiled plugins' consent) shows it too while it waits: hidden, the player could never see it, and the plugin would
// wait for an answer for good. Hiding comes back once it is answered.
namespace hailstone::app {

inline bool ui_shown(bool hidden, bool in_world, bool zone_settling, bool question_waiting) {
    return !hidden || !in_world || zone_settling || question_waiting;
}

} // namespace hailstone::app
