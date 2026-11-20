// The local authenticated pipe the drive relay uses (docs/research/drive-headless.md): a child process connects,
// both sides exchange bounded messages, and the child echoes them back. Windows only for now; the test skips
// elsewhere, as the platform component itself does.

#include <doctest/doctest.h>
#include <SDL3/SDL.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "platform/child_processes.h"
#include "platform/executable_path.h"
#include "platform/local_pipe.h"

using namespace hailstone;
using namespace hailstone::platform;

namespace {

void set_env(const char* name, const std::string& value) {
    SDL_SetEnvironmentVariable(SDL_GetEnvironment(), name, value.c_str(), true);
}
void clear_env(const char* name) { SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), name); }
const char* get_env(const char* name) { return SDL_GetEnvironmentVariable(SDL_GetEnvironment(), name); }

std::vector<std::byte> bytes_of(const std::string& text) {
    std::vector<std::byte> out(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) out[i] = static_cast<std::byte>(text[i]);
    return out;
}

std::string text_of(const std::vector<std::byte>& bytes) {
    std::string out(bytes.size(), '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) out[i] = static_cast<char>(bytes[i]);
    return out;
}

struct EnvClear {
    ~EnvClear() {
        clear_env("HAILSTONE_LOCAL_PIPE_ENDPOINT");
        clear_env("HAILSTONE_LOCAL_PIPE_TOKEN");
        clear_env("HAILSTONE_LOCAL_PIPE_CHILD");
    }
};

} // namespace

TEST_CASE("local pipe: same-process round trip") {
    LocalPipeServer server(make_local_pipe_session());
    const auto session = server.session();
    std::jthread client([session] {
        auto conn = LocalPipeConnection::connect(session, 5000);
        auto message = conn->receive_message(5000);
        if (message) conn->send_message(*message);
    });
    auto conn = server.accept(5000);
    REQUIRE_MESSAGE(conn != nullptr, server.error());
    REQUIRE(conn->send_message(bytes_of("hello thread")));
    auto echo = conn->receive_message(5000);
    REQUIRE(echo);
    CHECK(text_of(*echo) == "hello thread");
    conn->close();
}

// Run only in the child the test starts, and only with the endpoint and token the parent set.
TEST_CASE("local_pipe_child") {
    const char* endpoint = get_env("HAILSTONE_LOCAL_PIPE_ENDPOINT");
    const char* token = get_env("HAILSTONE_LOCAL_PIPE_TOKEN");
    const char* child = get_env("HAILSTONE_LOCAL_PIPE_CHILD");
    if (!endpoint || !token || !child || !*endpoint || !*token) return; // the ordinary suite run, not a child

    auto conn = LocalPipeConnection::connect({endpoint, token}, 15000);
    while (auto message = conn->receive_message(5000)) {
        if (!conn->send_message(*message)) break;
    }
}

TEST_CASE("local pipe: a child echoes authenticated messages") {
    auto exe = process_executable_path();
    REQUIRE(exe);
    EnvClear clear_on_exit;
    clear_env("HAILSTONE_LOCAL_PIPE_ENDPOINT");
    clear_env("HAILSTONE_LOCAL_PIPE_TOKEN");
    clear_env("HAILSTONE_LOCAL_PIPE_CHILD");

    LocalPipeServer server(make_local_pipe_session());
    const auto session = server.session();

    set_env("HAILSTONE_LOCAL_PIPE_ENDPOINT", session.endpoint);
    set_env("HAILSTONE_LOCAL_PIPE_TOKEN", session.token);
    set_env("HAILSTONE_LOCAL_PIPE_CHILD", "1");
    ChildProcesses child;
    std::string start_error;
    const bool started = child.start_tracked("pipe-echo", {exe->string(), "-tc=local_pipe_child"}, start_error);
    REQUIRE_MESSAGE(started, start_error);

    auto conn = server.accept(15000);
    REQUIRE_MESSAGE(conn != nullptr, server.error());
    if (!conn) {
        child.stop_all(true);
        return;
    }

    REQUIRE(conn->send_message(bytes_of("hello pipe")));
    auto first = conn->receive_message(5000);
    REQUIRE(first);
    CHECK(text_of(*first) == "hello pipe");

    // A message larger than one socket write: the framing, not a single send, must carry it.
    std::vector<std::byte> large(256u << 10);
    for (std::size_t i = 0; i < large.size(); ++i) large[i] = static_cast<std::byte>(i * 31u);
    REQUIRE(conn->send_message(large));
    auto echoed = conn->receive_message(5000);
    REQUIRE(echoed);
    CHECK(*echoed == large);

    conn->close();

    // Let the child see the close and exit; do not leave it holding the test's environment.
    bool exited = false;
    for (int i = 0; i < 500 && !exited; ++i) {
        child.poll();
        for (const auto& e : child.take_exited()) exited = e.exit_code == 0;
        if (!exited) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!exited) child.stop_all(true);
    CHECK(exited);
}
