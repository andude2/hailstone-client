#include "platform/child_processes.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_properties.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>

namespace hailstone::platform {
namespace {

#if defined(_WIN32)
// A Win32 GUI process launched outside a terminal (e.g. from Explorer or a shortcut) has NULL/invalid
// standard handles, causing SDL_CreateProcess(..., false)'s default SDL_PROCESS_STDIO_INHERITED to fail in
// DuplicateHandle(). Distinguish real console/pipe/file handles from missing or NUL-only handles so GUI
// launches and background supervisors spawn headless children with CREATE_NO_WINDOW and NUL stdio.
bool has_inheritable_std_handle(DWORD which) {
    const HANDLE h = ::GetStdHandle(which);
    if (!h || h == INVALID_HANDLE_VALUE) return false;
    const DWORD type = ::GetFileType(h);
    if (type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK) return true;
    if (type == FILE_TYPE_CHAR) return ::GetConsoleWindow() != nullptr;
    return false;
}
#endif

} // namespace

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
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
#if defined(_WIN32)
    const bool has_out = has_inheritable_std_handle(STD_OUTPUT_HANDLE);
    const bool has_err = has_inheritable_std_handle(STD_ERROR_HANDLE);
    if (!has_out && !has_err) {
        SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    } else {
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                              has_out ? SDL_PROCESS_STDIO_INHERITED : SDL_PROCESS_STDIO_NULL);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER,
                              has_err ? SDL_PROCESS_STDIO_INHERITED : SDL_PROCESS_STDIO_NULL);
    }
#endif
    SDL_Process* p = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
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
