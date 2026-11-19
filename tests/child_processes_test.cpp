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
    // The test binary answering --help runs nothing and exits 0. (A child
    // asked to run tests would inherit this one's output and hold the suite
    // open, so both tracked children are --help runs.)
    REQUIRE(children.start_tracked("self-help", {exe.string(), "--help"}, error));
    REQUIRE(children.start_tracked("self-help-2", {exe.string(), "-h"}, error));
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
    for (const auto& e : exited) CHECK(e.exit_code == 0);
    CHECK(exited[0].name != exited[1].name); // each exit is reported under its own name
}
