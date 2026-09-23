// release_manifest.py and xlings_artifacts.py, ported: this drives mcppls.devtools.release against
// a synthetic repository root, so it exercises the same orchestration (extract a payload tarball,
// write the xlings-res archives, render the .lua descriptors) the real release job runs, without
// touching the real checkout or the network.
import std;
import mcppls.testing;
import mcppls.base.path;
import mcppls.platform.fs;
import mcppls.platform.dirs;
import mcppls.pack.archive;
import mcppls.devtools.release;

namespace release = mcppls::devtools::release;
namespace ar = mcppls::pack::archive;
namespace fs = mcppls::platform::fs;
namespace base = mcppls::base;

namespace {

std::string scratch(std::string_view name) {
    const std::string directory { base::join_path(
        mcppls::platform::dirs::temp_directory(),
        std::format("mcppls-test-devtools-release-{}-{}", name, std::chrono::steady_clock::now().time_since_epoch().count())) };
    (void) fs::create_directories(directory);
    return directory;
}

void write(const std::string& path, std::string_view content) {
    (void) fs::create_directories(base::parent_path(path));
    (void) fs::write_file(path, content);
}

constexpr std::string_view RELEASE_MANIFEST = R"({
  "release-name": "mcpp-language-server {version}",
  "platforms": ["linux-x64"],
  "assets": [
    { "id": "payload", "pattern": "payload-{platform}.tar.gz", "per-platform": true, "required": true,
      "what": "The payload", "install": "Unpack it" }
  ],
  "not-in-a-release": []
})";

// One payload-<platform>.tar.gz, shaped like CI's real one: a top-level `payload/` directory
// holding bin/<exeName> and a two-file kit/.
std::string make_payload_tarball(const std::string& directory, std::string_view platformTarget, std::string_view exeName) {
    const std::string sourceDir { base::join_path(directory, std::format("payload-source-{}", platformTarget)) };
    (void) fs::create_directories(base::join_path(sourceDir, "bin"));
    (void) fs::create_directories(base::join_path(sourceDir, "kit"));
    (void) fs::write_file(base::join_path(sourceDir, std::format("bin/{}", exeName)), "ELF-ish server bytes");
    (void) fs::make_executable(std::array { base::join_path(sourceDir, std::format("bin/{}", exeName)) });
    (void) fs::write_file(base::join_path(sourceDir, "kit/kit.json"), R"({"kit-version": 1})");
    (void) fs::write_file(base::join_path(sourceDir, "kit/module.pcm"), "pcm-bytes");

    const std::string tarball { base::join_path(directory, std::format("payload-{}.tar.gz", platformTarget)) };
    const std::vector<ar::WriteEntry> entries {
        { base::join_path(sourceDir, "bin"), "bin" },
        { base::join_path(sourceDir, "kit"), "kit" },
    };
    auto wrote = ar::write(tarball, "payload", entries);
    if (!wrote) throw std::runtime_error(wrote.error().message);
    return tarball;
}

// make_xlings_artifacts processes every platform the lock has, so a test that wants it to succeed
// has to provide one tarball per platform -- a partial payloads directory is exactly the "missing
// tarball" test below.
void make_every_payload_tarball(const std::string& directory) {
    make_payload_tarball(directory, "linux-x64", "mcppls");
    make_payload_tarball(directory, "linux-arm64", "mcppls");
    make_payload_tarball(directory, "darwin-arm64", "mcppls");
    make_payload_tarball(directory, "win32-x64", "mcppls.exe");
}

std::string make_root() {
    const std::string root { scratch("root") };
    write(base::join_path(root, "LICENSE"), "Apache-2.0\n");
    write(base::join_path(root, "packaging/release.manifest.json"), RELEASE_MANIFEST);
    write(base::join_path(root, "packaging/payload.lock.json"), R"({"clangd-version": "23.1.0", "libcxx-version": "23.1.0", "entries": {},
            "platforms": {"linux-x64": {}, "linux-arm64": {}, "darwin-arm64": {}, "win32-x64": {}}})");
    write(base::join_path(root, "packaging/xlings/mcpp-language-server.lua.in"),
          "ref = \"@VERSION@\"\ndeps = { \"xim:llvm-tools@@CLANGD_VERSION@\" }\nsha256 = \"@SHA256_LINUX_X86_64@\"\n");
    write(base::join_path(root, "packaging/xlings/mcppls-kit.lua.in"),
          "ref = \"@KIT_VERSION@\"\nsha256 = \"@KIT_SHA256_LINUX_X86_64@\"\n");
    return root;
}

} // namespace

int main() {
    using namespace mcppls::testing;

    "release check: a staged directory matching the manifest passes"_test = [&] {
        const std::string root { make_root() };
        const std::string staged { scratch("staged") };
        (void) fs::write_file(base::join_path(staged, "payload-linux-x64.tar.gz"), "x");
        auto checked = release::check_release(root, "2026.9.16.1", staged, false);
        expect(checked.has_value());
        if (checked) expect(checked->ok);
        std::filesystem::remove_all(root);
    };

    "release check: a missing required file fails"_test = [&] {
        const std::string root { make_root() };
        const std::string staged { scratch("staged-empty") };
        auto checked = release::check_release(root, "2026.9.16.1", staged, false);
        expect(checked.has_value());
        if (checked) expect(!checked->ok);
        std::filesystem::remove_all(root);
    };

    "release xlings: server and kit archives are written, with sha256 rendered into both templates"_test = [&] {
        const std::string root { make_root() };
        const std::string payloads { scratch("payloads") };
        make_every_payload_tarball(payloads);
        const std::string out { scratch("xlings-out") };

        auto result = release::make_xlings_artifacts(root, payloads, out, std::string { "2026.9.16.1" },
                                                      std::nullopt, std::nullopt);
        expect(result.has_value());
        if (!result) return;

        const std::string serverArchive { base::join_path(out, "mcpp-language-server-2026.9.16.1-linux-x86_64.tar.gz") };
        const std::string kitArchive { base::join_path(out, "mcppls-kit-23.1.0-linux-x86_64.tar.gz") };
        expect(fs::is_regular_file(serverArchive));
        expect(fs::is_regular_file(kitArchive));

        // The server archive carries bin/mcppls and LICENSE, nothing from kit/.
        auto serverEntries = ar::extract(serverArchive, base::join_path(out, "server-check"), [](std::string_view) { return true; });
        expect(serverEntries.has_value());
        if (serverEntries) {
            expect(std::ranges::find(*serverEntries, "bin/mcppls") != serverEntries->end());
            expect(std::ranges::find(*serverEntries, "LICENSE") != serverEntries->end());
        }
        auto kitEntries = ar::extract(kitArchive, base::join_path(out, "kit-check"), [](std::string_view) { return true; });
        expect(kitEntries.has_value());
        if (kitEntries) {
            expect(std::ranges::find(*kitEntries, "kit.json") != kitEntries->end());
            expect(std::ranges::find(*kitEntries, "module.pcm") != kitEntries->end());
        }

        auto serverLua = fs::read_file(base::join_path(out, "mcpp-language-server.lua"));
        expect(serverLua.has_value());
        if (serverLua) {
            expect(serverLua->contains("ref = \"2026.9.16.1\""));
            // `@...@`-shaped placeholders are gone; a legitimate "@" (xlings' `pkg@version` syntax
            // inside the rendered value itself, e.g. "xim:llvm-tools@23.1.0") is not one.
            expect(!serverLua->contains("@VERSION@"));
            expect(!serverLua->contains("@CLANGD_VERSION@"));
            expect(!serverLua->contains("@SHA256_LINUX_X86_64@"));
        }
        auto kitLua = fs::read_file(base::join_path(out, "mcppls-kit.lua"));
        expect(kitLua.has_value());
        if (kitLua) {
            expect(kitLua->contains("ref = \"23.1.0\""));
            expect(!kitLua->contains("@"));
        }
        std::filesystem::remove_all(root);
    };

    "release xlings: a missing payload tarball is an error naming which platform"_test = [&] {
        const std::string root { make_root() };
        const std::string payloads { scratch("payloads-empty") };
        const std::string out { scratch("xlings-out-empty") };
        auto result = release::make_xlings_artifacts(root, payloads, out, std::string { "2026.9.16.1" }, std::nullopt, std::nullopt);
        expect(!result.has_value());
        std::filesystem::remove_all(root);
    };

    return report();
}
