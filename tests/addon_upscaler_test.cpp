// Upscaler add-ons (docs/addons.md "Upscalers"): an upscaler provider (sdk/hailstone_upscaler.h) with the runtime it
// loads, in upscalers/<name>/, that the client loads at the start when display.upscaler names it. The manifest's kind
// and caps, what its package may hold, its install and removal, the hand-installed list, and the manager installing
// one from GitHub in memory.

#include <doctest/doctest.h>

#include "addon_test_util.h"
#include "app/addon_install.h"
#include "app/addon_manager.h"
#include "core/addon_manifest.h"
#include "render/upscaler_provider.h"
#include "scratch_dir.h"

using namespace hailstone;
using namespace hailstone::app;
using namespace hailstone::addon_test;
namespace fs = std::filesystem;

namespace {

std::string upscaler_manifest(const std::string& name, const std::string& platform, const std::string& caps = "none") {
    return manifest_for(name, "upscaler", "1.0.0", caps, platform, "x", "u.zip");
}

AddonManifest upscaler(const std::string& name) {
    auto m = parse_addon_manifest(upscaler_manifest(name, this_platform()));
    REQUIRE(m);
    return *m;
}

std::optional<std::vector<AddonFile>> unpack(const fs::path& dir, const AddonManifest& m,
                                             const std::map<std::string, std::string>& files, std::string* error) {
    const fs::path zip = dir / "pkg.zip";
    put_file(zip, zip_of(files));
    return unpack_addon(dir, m, zip, this_platform(), error);
}

// A vendor runtime's name for this platform: versioned on Linux, as NVIDIA's is.
std::string runtime_name() {
    return this_platform().starts_with("linux") ? "libvendor-runtime.so.3.0.2" : "vendor_runtime" + lib_ext();
}

} // namespace

TEST_CASE("addon upscaler: the manifest's kind, caps and packages") {
    std::string why;
    auto m = parse_addon_manifest(upscaler_manifest("xess", "windows-x64"), &why);
    REQUIRE_MESSAGE(m, why);
    CHECK(m->kind == AddonKind::Upscaler);
    CHECK(m->caps.empty());
    // An upscaler asks for nothing: it sees only the frames it upscales.
    CHECK_FALSE(parse_addon_manifest(upscaler_manifest("xess", "windows-x64", "read"), &why));
    CHECK(why.find("caps none") != std::string::npos);
    // Each package is for one platform.
    CHECK_FALSE(parse_addon_manifest(upscaler_manifest("xess", "any"), &why));
    CHECK(why.find("one platform") != std::string::npos);
    CHECK(addon_kind_word(AddonKind::Upscaler) == "upscaler");
}

TEST_CASE("addon upscaler: its package: the provider, the runtime beside it, data") {
    if (this_platform().empty()) return;
    test::ScratchDir dir("addon_upscaler");
    const AddonManifest m = upscaler("xess");
    const std::string lib = library_for(this_platform());
    const std::string provider = "xess" + lib_ext();
    std::string why;
    auto files = unpack(dir.path(), m,
                        {{provider, lib}, {runtime_name(), lib}, {"LICENSE.txt", "terms"}, {"notices/third.md", "n"}},
                        &why);
    REQUIRE_MESSAGE(files, why);
    CHECK(files->size() == 4);
    CHECK(read_file(addons_dir(dir.path()) / "new" / "xess" / "LICENSE.txt") == "terms");

    struct Case {
        std::map<std::string, std::string> files;
        const char* why;
    };
    const std::string other = this_platform().starts_with("linux") ? "windows-x64" : "linux-x64";
    for (const Case& c : std::vector<Case>{
             {{{"LICENSE.txt", "x"}}, "no provider"},
             {{{runtime_name(), lib}}, "no provider"},
             {{{"bin/" + provider, lib}}, "top"},
             {{{provider, lib}, {"lib/" + runtime_name(), lib}}, "top"},
             {{{provider, "not a library"}}, "not a library"},
             {{{provider, library_for(other)}}, "not a library"},
             {{{provider, lib}, {runtime_name(), "not a library"}}, "not a library"},
             {{{provider, lib}, {other.starts_with("linux") ? "libx.so" : "x.dll", "x"}}, "another platform"},
             {{{provider, lib}, {"helper.exe", "MZ"}}, "may hold"},
             {{{provider, lib}, {"setup.sh", "x"}}, "may hold"},
             {{{provider, lib}, {".hidden", "x"}}, "not a name"},
             {{{provider, lib}, {"A.txt", "x"}, {"a.txt", "y"}}, "twice"},
         }) {
        std::string names;
        for (const auto& [n, _] : c.files) names += n + " ";
        CAPTURE(names);
        CHECK_FALSE(unpack(dir.path(), m, c.files, &why));
        CHECK_MESSAGE(why.find(c.why) != std::string::npos, why);
    }
    // The provider's name in another case is not the provider (on Windows and macOS it is one file).
    if (this_platform().starts_with("linux")) {
        CHECK_FALSE(unpack(dir.path(), m, {{"XeSS" + lib_ext(), lib}}, &why));
        CHECK_MESSAGE(why.find("named xess") != std::string::npos, why);
    }
}

TEST_CASE("addon upscaler: install into upscalers/, found there, name taken, removal") {
    if (this_platform().empty()) return;
    test::ScratchDir dir("addon_upscaler_apply");
    const AddonManifest m = upscaler("xess");
    const std::string lib = library_for(this_platform());
    std::string why;
    auto files = unpack(dir.path(), m, {{"xess" + lib_ext(), lib}, {runtime_name(), lib}}, &why);
    REQUIRE_MESSAGE(files, why);
    InstalledAddon a;
    a.name = m.name;
    a.kind = m.kind;
    a.source = "owner/xess";
    a.version = m.version_text;
    a.files = *files;
    REQUIRE_MESSAGE(apply_addon(dir.path(), a, &why), why);
    const fs::path folder = dir.path() / "upscalers" / "xess";
    CHECK(addon_folder(dir.path(), AddonKind::Upscaler, "xess") == folder);
    CHECK(fs::exists(folder / ("xess" + lib_ext())));
    CHECK(fs::exists(folder / runtime_name()));
    // The client finds it where display.upscaler looks.
    CHECK(render::installed_upscaler_providers(dir.path() / "upscalers") == std::vector<std::string>{"xess"});
    auto all = read_installed_addons(dir.path());
    REQUIRE(all.size() == 1);
    CHECK(all[0].kind == AddonKind::Upscaler);
    CHECK(addon_owns_folder(dir.path(), "upscalers/xess"));
    // No ceilings: it is not a plugin or a script.
    CHECK(addon_ceilings(all).empty());
    // Its name is taken, and so is any folder of that name in upscalers/ (the example a build puts there).
    CHECK_FALSE(addon_name_taken(dir.path(), all, AddonKind::Upscaler, "xess").empty());
    put_file(dir.path() / "upscalers" / "example" / ("example" + lib_ext()), lib);
    CHECK(addon_name_taken(dir.path(), {}, AddonKind::Upscaler, "example").find("upscalers/example") != std::string::npos);
    CHECK(addon_name_taken(dir.path(), {}, AddonKind::Upscaler, "fsr4").empty());
    // The hand-put one is listed as such; the installed one is not.
    const auto manual = manual_addons({dir.path()}, all);
    REQUIRE(manual.size() == 1);
    CHECK(manual[0].name == "example");
    CHECK(manual[0].kind == AddonKind::Upscaler);

    put_file(folder / "settings.ini", "mine");
    REQUIRE_MESSAGE(remove_addon(dir.path(), "xess", &why), why);
    CHECK_FALSE(fs::exists(folder / ("xess" + lib_ext())));
    CHECK(read_file(folder / "settings.ini") == "mine"); // its settings stay
    CHECK(render::installed_upscaler_providers(dir.path() / "upscalers") == std::vector<std::string>{"example"});
}

TEST_CASE("addon upscaler: the manager installs one from GitHub with no capabilities") {
    if (this_platform().empty()) return;
    test::ScratchDir dir("addon_upscaler_manager");
    FakeGitHub github;
    AddonsEnv env;
    env.data_dir = dir.path();
    env.client_version = "0.7.0-beta";
    env.platform = this_platform();
    env.fetch = github.fetch();
    env.now = [] { return std::int64_t{1'000'000}; };
    AddonManager manager(std::move(env));
    manager.wait_idle();

    const std::string lib = library_for(this_platform());
    const std::string zip = zip_of({{"fsr4" + lib_ext(), lib}, {runtime_name(), lib}, {"LICENSE.txt", "t"}});
    const Keys key;
    github.release("owner/fsr4", key.sign(manifest_for("fsr4", "upscaler", "1.0.0", "none", this_platform(), zip, "u.zip")),
                   "u.zip", zip);
    manager.inspect("https://github.com/owner/fsr4");
    manager.wait_idle();
    AddonsStatus st = manager.status();
    REQUIRE_MESSAGE(st.offer, st.message);
    CHECK_MESSAGE(st.offer->blocked.empty(), st.offer->blocked);
    CHECK(st.offer->caps == 0);
    manager.install_offer();
    manager.wait_idle();
    REQUIRE_MESSAGE(manager.has_ready(), manager.status().message);
    REQUIRE(manager.install_ready().size() == 1);
    CHECK(fs::exists(dir.path() / "upscalers" / "fsr4" / ("fsr4" + lib_ext())));
    CHECK(fs::exists(dir.path() / "upscalers" / "fsr4" / runtime_name()));
}

TEST_CASE("upscaler provider copy: loaded from a copy, reused, older ones deleted") {
    test::ScratchDir dir("upscaler_copy");
    const fs::path ups = dir.path() / "upscalers", copies = dir.path() / "cache";
    const std::string lib = "xess" + lib_ext();
    put_file(ups / "xess" / lib, "v1");
    put_file(ups / "xess" / "runtime" / "data.bin", "d");
    put_file(ups / "xess-2" / ("xess-2" + lib_ext()), "other");
    const fs::path a = render::upscaler_provider_copy(ups, "xess", copies);
    REQUIRE_FALSE(a.empty());
    CHECK(a.parent_path() == copies);
    CHECK(read_file(a / lib) == "v1");
    CHECK(read_file(a / "runtime" / "data.bin") == "d");
    // The same files: the same copy.
    CHECK(render::upscaler_provider_copy(ups, "xess", copies) == a);
    // Another provider whose name goes on from this one's keeps its copy when this one's are cleaned up.
    const fs::path other = render::upscaler_provider_copy(ups, "xess-2", copies);
    REQUIRE_FALSE(other.empty());
    // New files (an update): a new copy, and the old one goes.
    put_file(ups / "xess" / lib, "version two");
    const fs::path b = render::upscaler_provider_copy(ups, "xess", copies);
    REQUIRE_FALSE(b.empty());
    CHECK(b != a);
    CHECK(read_file(b / lib) == "version two");
    CHECK_FALSE(fs::exists(a));
    CHECK(fs::exists(other));
    // Nothing installed: no copy.
    CHECK(render::upscaler_provider_copy(ups, "missing", copies).empty());
}
