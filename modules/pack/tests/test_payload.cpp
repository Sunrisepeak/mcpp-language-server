// mcppls.pack.payload ports assemble_payload.py: assembling the <payload>/ layout from a server, a
// trimmed clangd and a semantic kit, verifying one, and copying an already-assembled one (--from).
//
// The fixtures here stand in for what mcppls.pack.clangd::trim and the kit recipe (ported
// separately) would actually produce -- just enough of each part's shape for every one of
// verify()'s checks to have something to check, so a real assemble+verify round trip runs clean.
import std;
import mcppls.testing;
import mcppls.base.path;
import mcppls.pack.lock;
import mcppls.pack.payload;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.os;

namespace payload = mcppls::pack::payload;
namespace lock = mcppls::pack::lock;
namespace base = mcppls::base;
namespace fs = mcppls::platform::fs;

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
// failed to start (openkal error 14). The same bytes serve all three hosts.
std::string runnable_clangd() {
    const auto self = mcppls::platform::env::arguments();
    std::string path { base::normalize_path(self.empty() ? std::string {} : self.front()) };
    if (!base::is_absolute_path(path)) path = base::join_path(fs::current_directory(), path);
    return fs::read_file(path).value_or(std::string {});
}

// A clangd directory shaped like mcppls.pack.clangd::trim's output: the executable, the license,
// and one lib/clang/<major>/include. Each call gets its own directory -- these fixtures are built
// fresh per test case, and several test cases share one scratch root.
std::string make_clangd_directory(const std::string& root, std::string_view platform) {
    const std::string dir { base::join_path(root, std::format("clangd-{}", std::random_device {}())) };
    const std::string exe { suffix_for(platform) };
    // On the host this platform matches, assemble()'s own clangd-version check (assemble_payload.py's
    // last step) runs this and compares what it prints -- "23.1.0" -- against the lock.
    put(base::join_path(dir, "bin/clangd" + exe), runnable_clangd());
    put(base::join_path(dir, "LICENSE.TXT"), "Apache-2.0 WITH LLVM-exception");
    put(base::join_path(dir, "lib/clang/23/include/stddef.h"), "typedef long ptrdiff_t;");
    return dir;
}

// A kit directory shaped like the recipe's output (build_kit.py's linux-x64 shape): kit.json, one
// module-metadata file naming the `std` module, and the include directory and license it points at.
std::string make_kit_directory(const std::string& root, std::string_view name = "mcppls-kit-libcxx-23.1.0-x86_64-unknown-linux-gnu") {
    const std::string dir { base::join_path(root, std::format("kit-{}", std::random_device {}())) };
    put(base::join_path(dir, "include/c++/v1/vector"), "// fixture header");
    put(base::join_path(dir, "modules/std.cppm"), "export module std;");
    put(base::join_path(dir, "licenses/LLVM-LICENSE.TXT"), "Apache-2.0 WITH LLVM-exception");
    put(base::join_path(dir, "libcxx.modules.json"), std::format(R"JSON({{
  "modules": [
    {{ "logical-name": "std", "source-path": "modules/std.cppm", "is-std-library": true,
       "local-arguments": {{ "system-include-directories": ["include/c++/v1"] }} }}
  ]
}})JSON"));
    put(base::join_path(dir, "kit.json"), std::format(R"JSON({{
  "kit-version": 1,
  "name": "{}",
  "stdlib": {{ "name": "libc++", "version": "23.1.0", "module-metadata": "libcxx.modules.json" }},
  "system-include-directories": ["include/c++/v1"],
  "sysroot": null,
  "licenses": ["licenses/LLVM-LICENSE.TXT"]
}})JSON", name));
    return dir;
}

lock::Lock lock_with_clangd_version(std::string version = "23.1.0") {
    lock::Lock lockData {};
    lockData.clangdVersion = std::move(version);
    return lockData;
}

payload::AssembleOptions options_for(const std::string& work, std::string_view platform,
                                     const std::string& server, const std::string& clangdDir,
                                     const std::string& kitDir, const std::string& out,
                                     const std::string& repoRoot) {
    return payload::AssembleOptions {
        .platform = std::string { platform },
        .serverPath = server,
        .clangdDirectory = clangdDir,
        .kitDirectory = kitDir,
        .outDirectory = out,
        .repositoryRoot = repoRoot,
    };
}

} // namespace

int main(int argc, char* argv[]) {
    // Started as the fixture's clangd (runnable_clangd): answer the one question asked of it.
    if (argc > 1 && std::string_view { argv[1] } == "--version") {
        std::println("clangd version 23.1.0 (fixture)");
        return 0;
    }
    using namespace mcppls::testing;

    const std::string work { scratch_dir() };
    const std::string repoRoot { base::join_path(work, "repo") };
    put(base::join_path(repoRoot, "LICENSE"), "Apache-2.0");
    const std::string server { base::join_path(work, "mcppls-fixture") };
    put(server, "#!/bin/sh\necho mcppls\n");
    const auto lockData = lock_with_clangd_version();

    "a well-formed assemble produces a payload that verifies clean"_test = [&] {
        const std::string clangdDir { make_clangd_directory(work, "linux-x64") };
        const std::string kitDir { make_kit_directory(work) };
        const std::string out { base::join_path(work, "payload-ok") };

        auto assembled = payload::assemble(options_for(work, "linux-x64", server, clangdDir, kitDir, out, repoRoot), lockData);
        expect(fatal(assembled.has_value())) << (assembled ? std::string {} : assembled.error().message);
        if (!assembled) return;

        expect(fs::is_regular_file(base::join_path(*assembled, "bin/mcppls")));
        expect(fs::is_regular_file(base::join_path(*assembled, "clangd/bin/clangd")));
        expect(fs::is_regular_file(base::join_path(*assembled, "kit/kit.json")));
        expect(fs::is_regular_file(base::join_path(*assembled, "licenses/mcppls-LICENSE.txt")));
        expect(fs::is_regular_file(base::join_path(*assembled, "licenses/LLVM-LICENSE.TXT")));
        expect(fs::is_regular_file(base::join_path(*assembled, "payload.json")));

        if constexpr (mcppls::os::FAMILY != mcppls::os::Family::windows) {
            const auto serverMode = std::filesystem::status(base::join_path(*assembled, "bin/mcppls")).permissions();
            expect((serverMode & std::filesystem::perms::owner_exec) != std::filesystem::perms::none);
            const auto clangdMode = std::filesystem::status(base::join_path(*assembled, "clangd/bin/clangd")).permissions();
            expect((clangdMode & std::filesystem::perms::owner_exec) != std::filesystem::perms::none);
        }

        auto problems = payload::verify(*assembled);
        for (const auto& problem : problems) std::println(std::cerr, "  verify problem: {}", problem);
        expect(problems.empty());
    };

    "a win32-x64 assemble uses the .exe paths throughout"_test = [&] {
        const std::string clangdDir { make_clangd_directory(work, "win32-x64") };
        const std::string kitDir { make_kit_directory(work, "mcppls-kit-libcxx-23.1.0-x86_64-w64-mingw32") };
        const std::string out { base::join_path(work, "payload-win32") };
        auto assembled = payload::assemble(options_for(work, "win32-x64", server, clangdDir, kitDir, out, repoRoot), lockData);
        expect(fatal(assembled.has_value())) << (assembled ? std::string {} : assembled.error().message);
        if (!assembled) return;
        expect(fs::is_regular_file(base::join_path(*assembled, "bin/mcppls.exe")));
        expect(fs::is_regular_file(base::join_path(*assembled, "clangd/bin/clangd.exe")));
        auto problems = payload::verify(*assembled);
        for (const auto& problem : problems) std::println(std::cerr, "  verify problem: {}", problem);
        expect(problems.empty());
    };

    "assembling for an unknown platform fails before writing anything"_test = [&] {
        const std::string clangdDir { make_clangd_directory(work, "linux-x64") };
        const std::string kitDir { make_kit_directory(work) };
        const std::string out { base::join_path(work, "payload-bad-platform") };
        auto assembled = payload::assemble(options_for(work, "sparc-solaris", server, clangdDir, kitDir, out, repoRoot), lockData);
        expect(!assembled.has_value());
        expect(!fs::exists(out));
    };

    "assembling with a missing clangd binary fails"_test = [&] {
        const std::string kitDir { make_kit_directory(work) };
        const std::string emptyClangd { base::join_path(work, "empty-clangd") };
        std::filesystem::create_directories(emptyClangd);
        auto assembled = payload::assemble(
            options_for(work, "linux-x64", server, emptyClangd, kitDir, base::join_path(work, "payload-missing-clangd"), repoRoot), lockData);
        expect(!assembled.has_value());
    };

    "verify reports a missing payload.json"_test = [&] {
        const std::string empty { base::join_path(work, "not-a-payload") };
        std::filesystem::create_directories(empty);
        auto problems = payload::verify(empty);
        expect(problems.size() == 1);
        expect(!problems.empty() && problems.front().contains("payload.json"));
    };

    "verify catches a tampered integrity file"_test = [&] {
        const std::string clangdDir { make_clangd_directory(work, "linux-x64") };
        const std::string kitDir { make_kit_directory(work) };
        const std::string out { base::join_path(work, "payload-tampered") };
        auto assembled = payload::assemble(options_for(work, "linux-x64", server, clangdDir, kitDir, out, repoRoot), lockData);
        expect(fatal(assembled.has_value()));
        if (!assembled) return;
        // Appending to the clangd binary after assembly changes its size and sha256 without
        // touching what payload.json recorded -- exactly the "payload-corrupt" case W9.4 exists for.
        {
            std::ofstream tamper { base::join_path(*assembled, "clangd/bin/clangd"), std::ios::binary | std::ios::app };
            tamper << "tampered";
        }
        auto problems = payload::verify(*assembled);
        expect(!problems.empty());
        const bool sawMismatch = std::ranges::any_of(problems, [](const std::string& p) { return p.contains("sha256") || p.contains("bytes"); });
        expect(sawMismatch);
    };

    "verify catches a kit that does not provide std"_test = [&] {
        const std::string clangdDir { make_clangd_directory(work, "linux-x64") };
        const std::string kitDir { base::join_path(work, "kit-no-std") };
        put(base::join_path(kitDir, "include/c++/v1/vector"), "// fixture header");
        put(base::join_path(kitDir, "modules/other.cppm"), "export module other;");
        put(base::join_path(kitDir, "licenses/LLVM-LICENSE.TXT"), "Apache-2.0 WITH LLVM-exception");
        put(base::join_path(kitDir, "libcxx.modules.json"), R"JSON({
  "modules": [
    { "logical-name": "other", "source-path": "modules/other.cppm", "is-std-library": true,
      "local-arguments": { "system-include-directories": ["include/c++/v1"] } }
  ]
})JSON");
        put(base::join_path(kitDir, "kit.json"), R"JSON({
  "kit-version": 1,
  "name": "mcppls-kit-no-std",
  "stdlib": { "name": "libc++", "version": "23.1.0", "module-metadata": "libcxx.modules.json" },
  "system-include-directories": ["include/c++/v1"],
  "sysroot": null,
  "licenses": ["licenses/LLVM-LICENSE.TXT"]
})JSON");
        const std::string out { base::join_path(work, "payload-no-std") };
        auto assembled = payload::assemble(options_for(work, "linux-x64", server, clangdDir, kitDir, out, repoRoot), lockData);
        expect(fatal(assembled.has_value())) << (assembled ? std::string {} : assembled.error().message);
        if (!assembled) return;
        auto problems = payload::verify(*assembled);
        expect(std::ranges::any_of(problems, [](const std::string& p) { return p.contains("does not provide std"); }));
    };

    "verify catches a kit carrying a program (S4-4-2)"_test = [&] {
        const std::string clangdDir { make_clangd_directory(work, "linux-x64") };
        const std::string kitDir { make_kit_directory(work, "mcppls-kit-with-a-script") };
        put(base::join_path(kitDir, "configure.sh"), "#!/bin/sh\necho no\n");
        const std::string out { base::join_path(work, "payload-kit-script") };
        auto assembled = payload::assemble(options_for(work, "linux-x64", server, clangdDir, kitDir, out, repoRoot), lockData);
        expect(fatal(assembled.has_value())) << (assembled ? std::string {} : assembled.error().message);
        if (!assembled) return;
        auto problems = payload::verify(*assembled);
        expect(std::ranges::any_of(problems, [](const std::string& p) { return p.contains("S4-4-2"); }));
    };

    "a darwin kit must declare requires macos-sdk (S4-4-6)"_test = [&] {
        const std::string clangdDir { make_clangd_directory(work, "darwin-arm64") };
        const std::string kitDir { make_kit_directory(work, "mcppls-kit-libcxx-23.1.0-arm64-apple-darwin") };
        const std::string out { base::join_path(work, "payload-darwin-no-sdk") };
        auto assembled = payload::assemble(options_for(work, "darwin-arm64", server, clangdDir, kitDir, out, repoRoot), lockData);
        expect(fatal(assembled.has_value())) << (assembled ? std::string {} : assembled.error().message);
        if (!assembled) return;
        auto problems = payload::verify(*assembled);
        expect(std::ranges::any_of(problems, [](const std::string& p) { return p.contains("S4-4-6"); }));
    };

    "copy_from copies a payload to a new directory, keeps exec bits, and verifies it"_test = [&] {
        const std::string clangdDir { make_clangd_directory(work, "linux-x64") };
        const std::string kitDir { make_kit_directory(work) };
        const std::string source { base::join_path(work, "payload-source") };
        auto assembled = payload::assemble(options_for(work, "linux-x64", server, clangdDir, kitDir, source, repoRoot), lockData);
        expect(fatal(assembled.has_value()));
        if (!assembled) return;

        const std::string out { base::join_path(work, "payload-copied") };
        auto copied = payload::copy_from(*assembled, out);
        expect(fatal(copied.has_value())) << (copied ? std::string {} : copied.error().message);
        if (!copied) return;
        expect(fs::is_regular_file(base::join_path(*copied, "bin/mcppls")));
        if constexpr (mcppls::os::FAMILY != mcppls::os::Family::windows) {
            const auto mode = std::filesystem::status(base::join_path(*copied, "bin/mcppls")).permissions();
            expect((mode & std::filesystem::perms::owner_exec) != std::filesystem::perms::none);
        }
    };

    "copy_from onto itself is a no-op copy that still verifies"_test = [&] {
        const std::string clangdDir { make_clangd_directory(work, "linux-x64") };
        const std::string kitDir { make_kit_directory(work) };
        const std::string source { base::join_path(work, "payload-self") };
        auto assembled = payload::assemble(options_for(work, "linux-x64", server, clangdDir, kitDir, source, repoRoot), lockData);
        expect(fatal(assembled.has_value()));
        if (!assembled) return;
        auto copied = payload::copy_from(*assembled, *assembled);
        expect(copied.has_value());
    };

    "copy_from a directory without payload.json fails"_test = [&] {
        const std::string notAPayload { base::join_path(work, "not-a-payload-2") };
        std::filesystem::create_directories(notAPayload);
        auto copied = payload::copy_from(notAPayload, base::join_path(work, "payload-copy-fail"));
        expect(!copied.has_value());
    };

    std::filesystem::remove_all(work);
    return report();
}
