#pragma once

// A shared library loaded at run time (an upscaler provider, render/upscaler_provider.h): SDL's loader, which takes
// a UTF-8 path on every platform.

#include <filesystem>
#include <string>
#include <string_view>

namespace hailstone::platform {

class SharedLibrary {
public:
    SharedLibrary() = default;
    // Loads `path`; loaded() says whether it did, error() why not.
    explicit SharedLibrary(const std::filesystem::path& path);
    ~SharedLibrary();
    SharedLibrary(SharedLibrary&& other) noexcept;
    SharedLibrary& operator=(SharedLibrary&& other) noexcept;
    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;

    bool loaded() const { return handle_ != nullptr; }
    const std::string& error() const { return error_; }
    // The exported function `name`, or nullptr.
    void* symbol(const char* name) const;
    void unload();

private:
    void* handle_ = nullptr;
    std::string error_;
};

// This platform's suffix for a shared library: ".dll", ".dylib" or ".so".
std::string_view shared_library_suffix();

} // namespace hailstone::platform
