#include "platform/child_processes.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_process.h>

#include <algorithm>

namespace hailstone::platform {

ChildProcesses::~ChildProcesses() {
    for (const auto& c : children_) SDL_DestroyProcess(c.process); // the SDL object only, not the program
}

bool ChildProcesses::start(const std::vector<std::string>& args, std::string& error) {
    return start_tracked("", args, error);
}

bool ChildProcesses::start_tracked(std::string name, const std::vector<std::string>& args, std::string& error) {
    std::vector<const char*> argv;
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    SDL_Process* p = SDL_CreateProcess(argv.data(), false);
    if (!p) {
        error = SDL_GetError();
        return false;
    }
    children_.push_back({p, std::move(name)});
    return true;
}

void ChildProcesses::stop_all(bool force) {
    for (const auto& c : children_) SDL_KillProcess(c.process, force);
}

void ChildProcesses::poll() {
    std::erase_if(children_, [this](Child& c) {
        int code = 0;
        if (!SDL_WaitProcess(c.process, false, &code)) return false;
        SDL_DestroyProcess(c.process);
        if (!c.name.empty()) exited_.push_back({c.name, code});
        return true;
    });
}

std::vector<ChildProcesses::Exited> ChildProcesses::take_exited() {
    auto out = std::move(exited_);
    exited_.clear();
    return out;
}

} // namespace hailstone::platform
