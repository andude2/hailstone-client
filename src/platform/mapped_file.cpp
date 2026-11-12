#include "platform/mapped_file.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <cstdint>
#include <stdexcept>

namespace hailstone::platform {

MappedFile::~MappedFile() { reset(); }

MappedFile::MappedFile(MappedFile&& other) noexcept
    : view_bytes_(other.view_bytes_), view_(other.view_), size_(other.size_), file_(other.file_),
      mapping_(other.mapping_) {
    other.view_bytes_ = nullptr;
    other.view_ = nullptr;
    other.size_ = 0;
    other.file_ = nullptr;
    other.mapping_ = nullptr;
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        reset();
        view_bytes_ = other.view_bytes_;
        view_ = other.view_;
        size_ = other.size_;
        file_ = other.file_;
        mapping_ = other.mapping_;
        other.view_bytes_ = nullptr;
        other.view_ = nullptr;
        other.size_ = 0;
        other.file_ = nullptr;
        other.mapping_ = nullptr;
    }
    return *this;
}

void MappedFile::reset() noexcept {
#if defined(_WIN32)
    if (view_ != nullptr) {
        ::UnmapViewOfFile(view_);
        view_ = nullptr;
        view_bytes_ = nullptr;
    }
    if (mapping_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(mapping_));
        mapping_ = nullptr;
    }
    if (file_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(file_));
        file_ = nullptr;
    }
#else
    if (view_ != nullptr) {
        ::munmap(view_, size_);
        view_ = nullptr;
        view_bytes_ = nullptr;
    }
#endif
    size_ = 0;
}

MappedFile MappedFile::open(const std::filesystem::path& path) {
#if defined(_WIN32)
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot read " + path.string());
    LARGE_INTEGER sz{};
    if (::GetFileSizeEx(file, &sz) == 0 || sz.QuadPart < 0) {
        ::CloseHandle(file);
        throw std::runtime_error("cannot read " + path.string());
    }
    if (sz.QuadPart == 0) {
        ::CloseHandle(file);
        return MappedFile{};
    }
    if (static_cast<std::uint64_t>(sz.QuadPart) > static_cast<std::uint64_t>(SIZE_MAX)) {
        ::CloseHandle(file);
        throw std::runtime_error("cannot map " + path.string());
    }
    const std::size_t size = static_cast<std::size_t>(sz.QuadPart);
    HANDLE mapping = ::CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        ::CloseHandle(file);
        throw std::runtime_error("cannot map " + path.string());
    }
    void* view = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        ::CloseHandle(mapping);
        ::CloseHandle(file);
        throw std::runtime_error("cannot map " + path.string());
    }
    MappedFile out;
    out.view_bytes_ = static_cast<const std::byte*>(view);
    out.view_ = view;
    out.size_ = size;
    out.file_ = static_cast<void*>(file);
    out.mapping_ = static_cast<void*>(mapping);
    return out;
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("cannot read " + path.string());
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < 0) {
        ::close(fd);
        throw std::runtime_error("cannot read " + path.string());
    }
    if (st.st_size == 0) {
        ::close(fd);
        return MappedFile{};
    }
    if (static_cast<std::uint64_t>(st.st_size) > static_cast<std::uint64_t>(SIZE_MAX)) {
        ::close(fd);
        throw std::runtime_error("cannot map " + path.string());
    }
    const std::size_t size = static_cast<std::size_t>(st.st_size);
    void* view = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (view == MAP_FAILED) {
        ::close(fd);
        throw std::runtime_error("cannot map " + path.string());
    }
    // Archive reads jump between the directory at the end and scattered members: tell the kernel not to readahead.
#if defined(MADV_RANDOM)
    (void)::madvise(view, size, MADV_RANDOM);
#endif
    ::close(fd);
    MappedFile out;
    out.view_bytes_ = static_cast<const std::byte*>(view);
    out.view_ = view;
    out.size_ = size;
    return out;
#endif
}

} // namespace hailstone::platform
