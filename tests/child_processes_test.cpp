// platform::ChildProcesses' tracked children: a supervisor (the headless
// crew) gets each child's name and exit code, by name.
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include "platform/child_processes.h"
#include "platform/paths.h"

using namespace hailstone;

TEST_CASE("child processes: a tracked child reports its name and exit code") {
    std::filesystem::path exe = platform::install_dir() / "hailstone_tests";
#if defined(_WIN32)
    exe += ".exe";
#endif
    if (!std::filesystem::is_regular_file(exe)) {
        MESSAGE("no hailstone_tests beside this binary; skipped");
        return;
    }
    platform::ChildProcesses children;
    std::string error;
    // The test binary answering --help runs nothing and exits 0.
    REQUIRE(children.start_tracked("self-help", {exe.string(), "--help"}, error));
    REQUIRE(children.start_tracked("self-bad-option", {exe.string(), "--no-such-doctest-option"}, error));
    CHECK(children.running() == 2);
    std::vector<platform::ChildProcesses::Exited> exited;
    for (int i = 0; i < 200 && exited.size() < 2; ++i) {
        children.poll();
        auto got = children.take_exited();
        exited.insert(exited.end(), got.begin(), got.end());
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    REQUIRE(exited.size() == 2);
    CHECK(children.running() == 0);
    for (const auto& e : exited) {
        if (e.name == "self-help") CHECK(e.exit_code == 0);
        if (e.name == "self-bad-option") CHECK(e.exit_code != 0);
    }
}
