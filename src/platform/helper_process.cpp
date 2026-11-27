#include "platform/helper_process.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace hailstone::platform {
namespace {

#if defined(_WIN32)
bool has_valid_std_error() {
    const HANDLE h = ::GetStdHandle(STD_ERROR_HANDLE);
    return h && h != INVALID_HANDLE_VALUE && ::GetFileType(h) != FILE_TYPE_UNKNOWN;
}
#endif

} // namespace

HelperProcess::~HelperProcess() {
    stop(true);
    if (process_) {
        SDL_WaitProcess(process_, true, nullptr);
        SDL_DestroyProcess(process_); // closes its streams
    }
}

bool HelperProcess::start(const std::vector<std::string>& args, std::string& error) {
    if (process_) {
        error = "already started";
        return false;
    }
    std::vector<const char*> argv;
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
#if defined(_WIN32)
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER,
                          has_valid_std_error() ? SDL_PROCESS_STDIO_INHERITED : SDL_PROCESS_STDIO_NULL);
#else
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_INHERITED);
#endif
    process_ = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!process_) {
        error = SDL_GetError();
        return false;
    }
    in_ = SDL_GetProcessInput(process_);
    exited_ = false;
    exit_code_ = 0;
    return true;
}

void HelperProcess::write(std::span<const std::byte> bytes) {
    if (!in_) return;
    flush();
    if (pending_write() + bytes.size() > max_pending_) { // not reading: ended, so the client sees it die and restarts it
        stop(true);
        return;
    }
    out_.insert(out_.end(), bytes.begin(), bytes.end());
    flush();
}

void HelperProcess::flush() {
    if (!in_ || pending_write() == 0) return;
    const std::size_t left = pending_write();
    std::size_t n = SDL_WriteIO(in_, out_.data() + head_, left);
    if (n < left && SDL_GetIOStatus(in_) == SDL_IO_STATUS_ERROR) { // the helper is gone
        out_.clear();
        head_ = 0;
        return;
    }
    written_ += n;
    head_ += n;
    // A pipe takes about 64 KB a write, so erasing what went from the front each time would move a large message
    // over and over (quadratic: seconds for one 60 MB message). The written bytes are dropped once they are at
    // least half the queue, so each byte is moved at most about once.
    if (head_ == out_.size()) {
        out_.clear();
        head_ = 0;
    } else if (head_ >= out_.size() / 2) {
        out_.erase(out_.begin(), out_.begin() + static_cast<std::ptrdiff_t>(head_));
        head_ = 0;
    }
}

std::size_t HelperProcess::read(std::span<std::byte> out) {
    if (!process_ || out.empty()) return 0;
    SDL_IOStream* s = SDL_GetProcessOutput(process_);
    if (!s) return 0;
    return SDL_ReadIO(s, out.data(), out.size());
}

bool HelperProcess::running() {
    if (!process_ || exited_) return false;
    int code = 0;
    if (SDL_WaitProcess(process_, false, &code)) {
        exited_ = true;
        exit_code_ = code;
        return false;
    }
    return true;
}

bool HelperProcess::wait_exit(int timeout_ms) {
    const Uint64 until = SDL_GetTicks() + static_cast<Uint64>(timeout_ms > 0 ? timeout_ms : 0);
    while (running()) {
        if (SDL_GetTicks() >= until) return false;
        SDL_Delay(10);
    }
    return true;
}

void HelperProcess::stop(bool kill) {
    if (!process_) return;
    if (in_) {
        flush();
        SDL_CloseIO(in_);
        in_ = nullptr;
        out_.clear();
        head_ = 0;
    }
    if (kill && running()) SDL_KillProcess(process_, true);
}

} // namespace hailstone::platform
