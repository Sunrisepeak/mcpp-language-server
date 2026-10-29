// mcppls.pack.payload: assembling the one payload (payload version 5) from the server's okb and
// tree, the engine of each platform and the openkal kit, verifying one, and copying an
// already-assembled one (--from).
//
// The fixtures here stand in for what `mcpp pack`, mcppls.pack.clangd::trim and the kit builders
// would actually produce -- just enough of each part's shape for every one of verify()'s checks to
// have something to check, so a real assemble+verify round trip runs clean.
import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.base.path;
import mcppls.pack.lock;
import mcppls.pack.fetch;
import mcppls.pack.payload;
import mcpplibs.os;
import mcppls.base.payload;

namespace os = mcpplibs::os;
namespace payload = mcppls::pack::payload;
namespace lock = mcppls::pack::lock;
namespace base = mcppls::base;
namespace {

std::string scratch_dir() {
    const auto root = std::filesystem::current_path() / ".test-scratch"
                      / std::format("mcppls-payload-{}", std::random_device {}());
    std::filesystem::create_directories(root);
    return root.string();
}

void put(const std::string& path, std::string_view content) {
    std::filesystem::create_directories(std::filesystem::path { path }.parent_path());
    std::ofstream out { path, std::ios::binary };
    out << content;
}

std::string suffix_for(std::string_view platform) { return platform == "win32-x64" ? ".exe" : ""; }

// A clangd this host can actually start: this test program, which answers `--version` the way
// clangd does (see main). The code under test runs `clangd --version` when the platform being
// packaged is this host's, as the Python it replaced did. A shell script stood in for clangd before,
// and it runs on Linux and macOS but not on Windows, where every fixture that matched the host
// failed to start (openkal error 14). The same bytes serve all three hosts. Read once: a dev build of
// this program on Windows is large enough that reading and hashing it for every fixture outlasted the
// test's time limit.
const std::string& runnable_clangd() {
    static const std::string bytes = [] {
        const auto self = os::env::arguments();
        std::string path { base::normalize_path(self.empty() ? std::string {} : self.front()) };
        if (!base::is_absolute_path(path)) path = base::join_path(base::current_directory(), path);
        return os::fs::read(path).value_or(std::string {});
    }();
    return bytes;
}

// A clangd directory shaped like a maintained engine part: the executable, the license, one
// lib/clang/<major>/include and engine.json, whose identity binds the platform and the bytes. Each
// call gets its own directory -- these fixtures are built fresh per test case, and several test
// cases share one scratch root.
std::string make_clangd_directory(const std::string& root, std::string_view platform, std::string_view version = "23.1.0-mcppls.5") {
    const std::string dir { base::join_path(root, std::format("clangd-{}", std::random_device {}())) };
    const std::string exe { suffix_for(platform) };
    // On the host this platform matches, assemble()'s own clangd-version check runs this and
    // compares what it prints -- "23.1.0-mcppls.5" -- against the identity. Another platform's
    // engine is never started here, so a few bytes stand for it.
    put(base::join_path(dir, "bin/clangd" + exe), platform == base::payload_platform() ? std::string_view { runnable_clangd() } : std::string_view { "a clangd" });
    put(base::join_path(dir, "LICENSE.TXT"), "Apache-2.0 WITH LLVM-exception");
    put(base::join_path(dir, "lib/clang/23/include/stddef.h"), "typedef long ptrdiff_t;");
    const auto digest = mcppls::pack::fetch::digest_of(base::join_path(dir, "bin/clangd" + exe));
    const nlohmann::json identity { { "engine-version", std::string { version } }, { "llvm-base-version", "23.1.0" },
                                    { "llvm-commit", std::string(40, 'a') }, { "fork-commit", std::string(40, 'b') },
                                    { "patch-series-sha256", std::string(64, 'c') }, { "platform", std::string { platform } },
                                    { "sha256", digest.value_or(std::string(64, 'd')) }, { "features", nlohmann::json::array() } };
    put(base::join_path(dir, "engine.json"), identity.dump());
    return dir;
}

// The tree `mcpp pack --target openkal --mode self-contained --systems all` writes: the runner of
// every system, and mcpp-package.json saying where it runs.
std::string make_server_tree(const std::string& root, std::vector<std::string> runsOn = { "linux-aarch64", "linux-x86_64", "macos-aarch64", "windows-x86_64" }) {
    const std::string dir { base::join_path(root, std::format("tree-{}", std::random_device {}())) };
    for (const auto& system : runsOn) {
        put(base::join_path(dir, std::format("libexec/okrun/{}/okrun{}", system, system.starts_with("windows") ? ".exe" : "")), "a runner");
    }
    put(base::join_path(dir, "mcpp-package.json"), nlohmann::json { { "schema", 1 }, { "runs_on", { { "self", runsOn } } } }.dump());
    return dir;
}

// A kit directory shaped like the openkal kit: kit.json of two targets, one module-metadata file
// naming the `std` module, the include directories and license it points at, and a `__config` that
// states the libc++ version its headers are (S4-4-5).
std::string make_kit_directory(const std::string& root, std::string_view name = "mcppls-kit-openkal-0.17.3", std::string_view headers = "220108") {
    const std::string dir { base::join_path(root, std::format("kit-{}", std::random_device {}())) };
    put(base::join_path(dir, "include/c++/v1/vector"), "// fixture header");
    put(base::join_path(dir, "include/c++/v1/__config"), std::format("#ifndef _LIBCPP___CONFIG\n#  define _LIBCPP_VERSION {}\n#endif\n", headers));
    put(base::join_path(dir, "include/x86_64-openkal/bits/alltypes.h"), "// fixture header");
    put(base::join_path(dir, "include/aarch64-openkal/bits/alltypes.h"), "// fixture header");
    put(base::join_path(dir, "modules/std.cppm"), "export module std;");
    put(base::join_path(dir, "licenses/LLVM-LICENSE.TXT"), "Apache-2.0 WITH LLVM-exception");
    put(base::join_path(dir, "libcxx.modules.json"), std::format(R"JSON({{
  "modules": [
    {{ "logical-name": "std", "source-path": "modules/std.cppm", "is-std-library": true,
       "local-arguments": {{ "system-include-directories": ["include/c++/v1"] }} }}
  ]
}})JSON"));
    put(base::join_path(dir, "kit.json"), std::format(R"JSON({{
  "kit-version": 2,
  "name": "{}",
  "target": "x86_64-unknown-none-elf",
  "stdlib": {{ "name": "libc++", "version": "22.1.8", "module-metadata": "libcxx.modules.json" }},
  "system-include-directories": ["include/c++/v1", "include/x86_64-openkal"],
  "sysroot": null,
  "targets": {{
    "x86_64-openkal": {{ "target": "x86_64-unknown-none-elf", "system-include-directories": ["include/c++/v1", "include/x86_64-openkal"] }},
    "aarch64-openkal": {{ "target": "aarch64-unknown-none-elf", "system-include-directories": ["include/c++/v1", "include/aarch64-openkal"] }}
  }},
  "licenses": ["licenses/LLVM-LICENSE.TXT"]
}})JSON", name));
    return dir;
}

lock::Lock lock_with_clangd_version(std::string version = "23.1.0-mcppls.5") {
    lock::Lock lockData {};
    lockData.clangdVersion = std::move(version);
    for (const std::string_view platform : { "linux-x64", "linux-arm64", "darwin-arm64", "win32-x64" }) {
        lockData.platforms.emplace(std::string { platform }, lock::Platform {});
    }
    return lockData;
}

struct Fixture {
    std::string work;
    std::string repoRoot;
    std::string image;
};

payload::AssembleOptions options_for(const Fixture& fixture, std::vector<std::string> platforms, const std::string& out) {
    payload::AssembleOptions options {
        .serverImage = fixture.image,
        .serverTree = make_server_tree(fixture.work),
        .kitDirectory = make_kit_directory(fixture.work),
        .outDirectory = out,
        .repositoryRoot = fixture.repoRoot,
    };
    for (const auto& platform : platforms) options.engines.push_back({ platform, make_clangd_directory(fixture.work, platform) });
    return options;
}

} // namespace

int main(int argc, char* argv[]) {
    std::error_code ignored;
    // Started as the fixture's clangd (runnable_clangd): answer the one question asked of it.
    if (argc > 1 && std::string_view { argv[1] } == "--version") {
        std::println("clangd version 23.1.0-mcppls.5 (fixture)");
        return 0;
    }
    using namespace mcppls::testing;

    const std::string work { scratch_dir() };
    const Fixture fixture { work, base::join_path(work, "repo"), base::join_path(work, "mcppls.okb") };
    put(base::join_path(fixture.repoRoot, "LICENSE"), "Apache-2.0");
    put(fixture.image, "PK\x03\x04 an okb");
    const auto lockData = lock_with_clangd_version();
    const std::vector<std::string> every { "linux-x64", "linux-arm64", "darwin-arm64", "win32-x64" };

    "one payload carries the okb, every system's runner and every platform's engine, and verifies clean"_test = [&] {
        std::error_code ignored;
        const std::string out { base::join_path(work, "payload-one") };
        auto assembled = payload::assemble(options_for(fixture, every, out), lockData);
        expect(fatal(assembled.has_value())) << (assembled ? std::string {} : assembled.error().message);
        if (!assembled) return;
        for (const std::string_view relative : { "bin/mcppls.okb", "libexec/okrun/linux-x86_64/okrun", "libexec/okrun/linux-aarch64/okrun",
                                                 "libexec/okrun/macos-aarch64/okrun", "libexec/okrun/windows-x86_64/okrun.exe",
                                                 "clangd/linux-x64/bin/clangd", "clangd/win32-x64/bin/clangd.exe", "clangd/darwin-arm64/engine.json",
                                                 "clangd/linux-arm64/lib/clang/23/include/stddef.h", "kit/kit.json", "licenses/mcppls-LICENSE.txt",
                                                 "licenses/LLVM-LICENSE.TXT", "payload.json" }) {
            expect(std::filesystem::is_regular_file(base::join_path(*assembled, relative), ignored)) << relative;
        }
        if (os::host() != os::platform::windows) {
            for (const std::string_view program : { "libexec/okrun/linux-x86_64/okrun", "clangd/darwin-arm64/bin/clangd" }) {
                const auto mode = std::filesystem::status(base::join_path(*assembled, program)).permissions();
                expect((mode & std::filesystem::perms::owner_exec) != std::filesystem::perms::none) << program;
            }
        }
        const auto manifest = nlohmann::json::parse(os::fs::read(base::join_path(*assembled, "payload.json")).value_or("{}"));
        expect(manifest["payload-version"] == 5);
        expect(manifest["server"].value("path", std::string {}) == "bin/mcppls.okb");
        expect(manifest["platforms"].size() == 4u);
        expect(manifest["platforms"]["win32-x64"].value("runner", std::string {}) == "libexec/okrun/windows-x86_64/okrun.exe");
        expect(manifest["platforms"]["darwin-arm64"]["engines"]["clangd"].value("path", std::string {}) == "clangd/darwin-arm64/bin/clangd");
        expect(manifest["platforms"]["linux-arm64"]["engines"]["clangd"]["identity"].value("platform", std::string {}) == "linux-arm64");
        expect(manifest["platforms"]["linux-x64"]["files"].contains("clangd/linux-x64/bin/clangd"));
        expect(manifest["files"].contains("kit/kit.json"));
        auto problems = payload::verify(*assembled);
        for (const auto& problem : problems) std::println(std::cerr, "  verify problem: {}", problem);
        expect(problems.empty());
    };

    "a payload of some platforms serves those only"_test = [&] {
        const std::string out { base::join_path(work, "payload-two") };
        auto assembled = payload::assemble(options_for(fixture, { "linux-x64", "win32-x64" }, out), lockData);
        expect(fatal(assembled.has_value())) << (assembled ? std::string {} : assembled.error().message);
        if (!assembled) return;
        const auto manifest = nlohmann::json::parse(os::fs::read(base::join_path(*assembled, "payload.json")).value_or("{}"));
        expect(manifest["platforms"].size() == 2u && !manifest["platforms"].contains("darwin-arm64"));
        expect(payload::verify(*assembled).empty());
    };

    "a tree that does not run on a platform's system is refused before anything is written"_test = [&] {
        std::error_code ignored;
        auto options = options_for(fixture, { "darwin-arm64" }, base::join_path(work, "payload-no-runner"));
        options.serverTree = make_server_tree(work, { "linux-x86_64" });
        auto assembled = payload::assemble(options, lockData);
        expect(!assembled.has_value());
        if (!assembled) expect(assembled.error().message.contains("macos-aarch64")) << assembled.error().message;
        expect(!std::filesystem::exists(options.outDirectory, ignored));
        // A runner the tree carries while its runs_on leaves the system out: the tree says it does not run there.
        options.serverTree = make_server_tree(work);
        put(base::join_path(options.serverTree, "mcpp-package.json"), nlohmann::json { { "runs_on", { { "self", { "linux-x86_64" } } } } }.dump());
        expect(!payload::assemble(options, lockData));
    };

    "every engine's immutable identity binds its platform and bytes before the output is replaced"_test = [&] {
        std::error_code ignored;
        const std::string out { base::join_path(work, "payload-identified") };
        put(base::join_path(out, "sentinel"), "keep");
        auto options = options_for(fixture, { "win32-x64" }, out);
        const std::string engine { options.engines.front().clangdDirectory };
        auto identity = nlohmann::json::parse(os::fs::read(base::join_path(engine, "engine.json")).value_or("{}"));
        identity["sha256"] = std::string(64, 'e');
        put(base::join_path(engine, "engine.json"), identity.dump());
        expect(!payload::assemble(options, lockData)) << "a forged binary SHA is refused";
        expect(std::filesystem::is_regular_file(base::join_path(out, "sentinel"), ignored));
        options = options_for(fixture, { "win32-x64" }, out);
        options.engines.front().platform = "linux-x64";
        expect(!payload::assemble(options, lockData)) << "an engine of another platform is refused";
        options = options_for(fixture, { "win32-x64" }, out);
        expect(!payload::assemble(options, lock_with_clangd_version("24.1.0"))) << "an engine cannot silently change the locked LLVM base";
        expect(!payload::assemble(options, lock_with_clangd_version("23.1.0-mcppls.6"))) << "nor the locked fork version";
        options.candidateEngine = true;
        expect(payload::assemble(options, lock_with_clangd_version("23.1.0-mcppls.6")).has_value()) << "a candidate engine is another fork of the base";
        std::filesystem::remove(base::join_path(options.engines.front().clangdDirectory, "engine.json"), ignored);
        expect(!payload::assemble(options, lockData)) << "an engine without engine.json is refused";
    };

    "assembling for an unknown platform fails before writing anything"_test = [&] {
        const std::string out { base::join_path(work, "payload-bad-platform") };
        auto options = options_for(fixture, { "linux-x64" }, out);
        options.engines.front().platform = "sparc-solaris";
        expect(!payload::assemble(options, lockData).has_value());
        expect(!std::filesystem::exists(out, ignored));
    };

    "assembling with a missing clangd binary fails"_test = [&] {
        auto options = options_for(fixture, { "linux-x64" }, base::join_path(work, "payload-missing-clangd"));
        const std::string emptyClangd { base::join_path(work, "empty-clangd") };
        std::filesystem::create_directories(emptyClangd);
        options.engines.front().clangdDirectory = emptyClangd;
        expect(!payload::assemble(options, lockData).has_value());
    };

    "verify reports a missing payload.json"_test = [&] {
        const std::string empty { base::join_path(work, "not-a-payload") };
        std::filesystem::create_directories(empty);
        auto problems = payload::verify(empty);
        expect(problems.size() == 1);
        expect(!problems.empty() && problems.front().contains("payload.json"));
    };

    "verify catches a tampered engine and a payload of an earlier version"_test = [&] {
        auto assembled = payload::assemble(options_for(fixture, { "linux-x64" }, base::join_path(work, "payload-tampered")), lockData);
        expect(fatal(assembled.has_value()));
        if (!assembled) return;
        {
            std::ofstream tamper { base::join_path(*assembled, "clangd/linux-x64/bin/clangd"), std::ios::binary | std::ios::app };
            tamper << "tampered";
        }
        auto problems = payload::verify(*assembled);
        expect(std::ranges::any_of(problems, [](const std::string& p) { return p.contains("sha256") || p.contains("bytes"); }));
        auto manifest = nlohmann::json::parse(os::fs::read(base::join_path(*assembled, "payload.json")).value_or("{}"));
        manifest["payload-version"] = 4;
        put(base::join_path(*assembled, "payload.json"), manifest.dump());
        problems = payload::verify(*assembled);
        expect(std::ranges::any_of(problems, [](const std::string& p) { return p.contains("payload-version is 4"); }));
    };

    "verify_kit: std provided, data only, the libc++ its headers are, a macOS kit's SDK"_test = [&] {
        expect(payload::verify_kit(make_kit_directory(work)).empty());
        const std::string noStd { make_kit_directory(work) };
        put(base::join_path(noStd, "libcxx.modules.json"), R"JSON({ "modules": [ { "logical-name": "other", "source-path": "modules/std.cppm" } ] })JSON");
        expect(std::ranges::any_of(payload::verify_kit(noStd), [](const std::string& p) { return p.contains("does not provide std"); }));
        const std::string script { make_kit_directory(work) };
        put(base::join_path(script, "configure.sh"), "#!/bin/sh\necho no\n");
        expect(std::ranges::any_of(payload::verify_kit(script), [](const std::string& p) { return p.contains("S4-4-2"); }));
        const std::string otherHeaders { make_kit_directory(work, "mcppls-kit-openkal-0.17.3", "230103") };
        expect(std::ranges::any_of(payload::verify_kit(otherHeaders), [](const std::string& p) { return p.contains("S4-4-5") && p.contains("23.1.3"); }));
        const std::string darwin { make_kit_directory(work) };
        auto kit = nlohmann::json::parse(os::fs::read(base::join_path(darwin, "kit.json")).value_or("{}"));
        kit["target"] = "arm64-apple-darwin";
        kit.erase("targets");
        put(base::join_path(darwin, "kit.json"), kit.dump());
        expect(std::ranges::any_of(payload::verify_kit(darwin), [](const std::string& p) { return p.contains("S4-4-6"); }));
        const std::string missingTarget { make_kit_directory(work) };
        std::filesystem::remove_all(base::join_path(missingTarget, "include/aarch64-openkal"));
        expect(std::ranges::any_of(payload::verify_kit(missingTarget), [](const std::string& p) { return p.contains("include/aarch64-openkal"); }))
            << "every target's include directories are checked";
    };

    "copy_from copies a payload to a new directory, keeps exec bits, and verifies it"_test = [&] {
        std::error_code ignored;
        auto assembled = payload::assemble(options_for(fixture, { "linux-x64" }, base::join_path(work, "payload-source")), lockData);
        expect(fatal(assembled.has_value()));
        if (!assembled) return;
        const std::string out { base::join_path(work, "payload-copied") };
        auto copied = payload::copy_from(*assembled, out);
        expect(fatal(copied.has_value())) << (copied ? std::string {} : copied.error().message);
        if (!copied) return;
        expect(std::filesystem::is_regular_file(base::join_path(*copied, "libexec/okrun/linux-x86_64/okrun"), ignored));
        if (os::host() != os::platform::windows) {
            const auto mode = std::filesystem::status(base::join_path(*copied, "libexec/okrun/linux-x86_64/okrun")).permissions();
            expect((mode & std::filesystem::perms::owner_exec) != std::filesystem::perms::none);
        }
    };

    "copy_from onto itself is a no-op copy that still verifies"_test = [&] {
        auto assembled = payload::assemble(options_for(fixture, { "linux-x64" }, base::join_path(work, "payload-self")), lockData);
        expect(fatal(assembled.has_value()));
        if (!assembled) return;
        expect(payload::copy_from(*assembled, *assembled).has_value());
    };

    "copy_from a directory without payload.json fails"_test = [&] {
        const std::string notAPayload { base::join_path(work, "not-a-payload-2") };
        std::filesystem::create_directories(notAPayload);
        expect(!payload::copy_from(notAPayload, base::join_path(work, "payload-copy-fail")).has_value());
    };

    std::filesystem::remove_all(work);
    return report();
}
