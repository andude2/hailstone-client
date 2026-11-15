#pragma once

// --input-script: a test driver that plays made-up input at set times after zoning in, as the OS and a pad would send
// it (platform::Window's virtual pad and pushed events), so the gamepad modes, the windows' focus and typing can be
// checked headlessly. Steps are ';'-separated "<seconds> <action> [args]", the seconds after the latest zone-in; a
// time written "c<seconds>" counts from when the character select screen last appeared (after a camp, say), one written
// "d<seconds>" from when the disconnected screen (a link death) appeared. Steps run in the order written where the
// clock changes, and by time within a run of steps on the same clock:
//   pad <button> [hold]        a virtual Xbox pad's button (a b x y back guide start ls rs lb rb up down left right),
//                              held `hold` seconds (0.15)
//   axis <lx|ly|rx|ry|lt|rt> <value> [hold]   a stick or trigger at value (-1..1) for `hold` seconds (0.5)
//   key <name> [hold]          a key by its SDL scancode name ("Return", "A", "Escape", "Slash"; "Left_Alt" for "Left Alt")
//   text <text...>             typed text (the text input event)
//   click <x> <y> [right] [hold]   a mouse click at that fraction of the window (0..1), held `hold` seconds (0.08;
//                              "click x y 0.6" a left one)
//   wheel <x> <y> <steps>      the mouse wheel turned there (steps > 0: away from the user)
//   move <x> <y>               the mouse moved there (a hover; a click right after a move lands as a user's would)
//   press <x> <y> / release <x> <y>   the left button down / up there (with moves between: a drag)
//   shot <file>                a screenshot
//   state                      logs the pad's mode and ImGui's focus (ImGuiLayer::debug_state)
//   close                      the window manager's close request (the title bar's X, Alt+F4)
//   droplink                   the world and zone links go silent (net::Session::simulate_link_loss): a link death
// The virtual pad is attached at the start, so it is the pad in use.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone::platform { class Window; }

namespace hailstone::app {

// A number in a script: `fallback` for a missing one, and for "nan", "inf" or one too large to be finite, which would
// otherwise reach a float-to-integer cast (Window::set_virtual_pad_axis) as undefined behaviour.
inline double script_number(const std::string& text, double fallback = 0.0) {
    const double v = std::strtod(text.c_str(), nullptr);
    return std::isfinite(v) ? v : fallback;
}

class InputScript {
public:
    struct Hooks {
        std::function<void(const std::string& path)> screenshot;
        std::function<std::string()> state;
        std::function<void()> drop_link;
    };

    // Parses `script`; bad steps are logged and skipped.
    InputScript(std::string_view script, platform::Window& window, Hooks hooks);

    // Once a frame after the UI drew, with the seconds since the player zoned in, since the character select screen
    // appeared and since the disconnected screen appeared (each negative while not there): runs the steps due and lets
    // go of what was held long enough.
    void frame(double seconds, double select_seconds = -1.0, double disconnect_seconds = -1.0);
    bool done() const { return next_ >= steps_.size() && releases_.empty(); }

private:
    enum class Clock : std::uint8_t { Zone, Select, Disconnect }; // "<seconds>", "c<seconds>", "d<seconds>"
    struct Step {
        double at = 0.0;
        Clock clock = Clock::Zone;
        std::string action;
        std::vector<std::string> args;
        std::string rest; // the arguments as written (text)
    };
    // A press lets go at its time, but not before the client has run kMinHeldFrames frames with it held: at a low
    // frame rate (a busy GPU, a window in the background) a short tap would otherwise fall between two frames.
    static constexpr int kMinHeldFrames = 3;
    struct Release {
        double at = 0.0;
        Clock clock = Clock::Zone;
        long frame = 0; // the earliest frame
        std::function<void()> fn;
    };
    void run(const Step& s, double now);

    platform::Window& window_;
    Hooks hooks_;
    std::vector<Step> steps_;
    std::size_t next_ = 0;
    long frame_ = 0;
    std::vector<Release> releases_;
};

} // namespace hailstone::app
