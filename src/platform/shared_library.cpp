#include "platform/shared_library.h"

#include <utility>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_loadso.h>

#include "core/path_utf8.h"

namespace hailstone::platform {

SharedLibrary::SharedLibrary(const std::filesystem::path& path) {
    handle_ = SDL_LoadObject(path_utf8(path).c_str());
    if (!handle_) error_ = SDL_GetError();
}

SharedLibrary::~SharedLibrary() { unload(); }

SharedLibrary::SharedLibrary(SharedLibrary&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)), error_(std::move(other.error_)) {}

SharedLibrary& SharedLibrary::operator=(SharedLibrary&& other) noexcept {
    if (this != &other) {
        unload();
        handle_ = std::exchange(other.handle_, nullptr);
        error_ = std::move(other.error_);
    }
    return *this;
}

void* SharedLibrary::symbol(const char* name) const {
    return handle_ ? reinterpret_cast<void*>(SDL_LoadFunction(static_cast<SDL_SharedObject*>(handle_), name)) : nullptr;
}

void SharedLibrary::unload() {
    if (handle_) SDL_UnloadObject(static_cast<SDL_SharedObject*>(handle_));
    handle_ = nullptr;
}

std::string_view shared_library_suffix() {
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

} // namespace hailstone::platform
