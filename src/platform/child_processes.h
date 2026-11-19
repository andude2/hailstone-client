#pragma once

// Other programs this one starts and leaves running (the box set's other Hailstone windows): started detached
// from this one's console, never stopped by it, and collected once they exit so none is left a zombie. A
// tracked child (start_tracked) also reports its name and exit code through take_exited(), for a supervisor
// that relaunches its own programs (the headless crew, docs/headless.md); stop_all() ends them all, gently
// first (the program's own stop), forcibly when asked or when they linger past it.

#include <string>
#include <vector>

struct SDL_Process;

namespace hailstone::platform {

class ChildProcesses {
public:
    ChildProcesses() = default;
    ChildProcesses(const ChildProcesses&) = delete;
    ChildProcesses& operator=(const ChildProcesses&) = delete;
    ~ChildProcesses(); // forgets them: they go on running

    // args[0] is the program. False (with error set) when it could not be started.
    bool start(const std::vector<std::string>& args, std::string& error);
    // A child the caller keeps by name: its exit (with the exit code) is
    // queued for take_exited() by the next poll().
    bool start_tracked(std::string name, const std::vector<std::string>& args, std::string& error);
    // Ends every child: force false asks each to stop (on Windows a child
    // process is ended outright either way); force true kills them.
    void stop_all(bool force = false);
    // Collects the ones that have exited.
    void poll();
    struct Exited {
        std::string name;
        int exit_code = -1;
    };
    std::vector<Exited> take_exited();
    std::size_t running() const { return children_.size(); }

private:
    struct Child {
        SDL_Process* process = nullptr;
        std::string name; // empty: untracked
    };
    std::vector<Child> children_;
    std::vector<Exited> exited_;
};

} // namespace hailstone::platform
