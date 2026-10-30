// release_manifest.py and xlings_artifacts.py, ported: this drives mcppls.devtools.release against
// a synthetic repository root, so it exercises the same orchestration (extract a payload tarball,
// write the xlings-res archives, render the .lua descriptors) the real release job runs, without
// touching the real checkout or the network.
import std;
import mcppls.testing;
import mcppls.base.path;
import mcppls.pack.archive;
import mcppls.devtools.release;
import mcpplibs.os;

namespace os = mcpplibs::os;

namespace release = mcppls::devtools::release;
namespace ar = mcppls::pack::archive;
namespace base = mcppls::base;

namespace {

std::string scratch(std::string_view name) {
    std::error_code ignored;
    const std::string directory { base::join_path(
        base::spelled(os::dirs::temp()),
        std::format("mcppls-test-devtools-release-{}-{}", name, std::chrono::steady_clock::now().time_since_epoch().count())) };
    (void)std::filesystem::create_directories(directory, ignored);
    return directory;
}

void write(const std::string& path, std::string_view content) {
    std::error_code ignored;
    (void)std::filesystem::create_directories(base::parent_path(path), ignored);
    (void) os::fs::write(path, content);
}

constexpr std::string_view RELEASE_MANIFEST = R"({
  "release-name": "mcpp-language-server {version}",
  "platforms": ["linux-x64", "win32-x64"],
  "assets": [
    { "id": "payload", "pattern": "mcppls-payload-{version}.tar.gz", "per-platform": false, "required": true,
      "what": "The payload", "install": "Unpack it" },
    { "id": "kit-openkal", "pattern": "mcppls-kit-openkal-*.tar.gz", "per-platform": false, "required": true,
      "what": "The openkal kit", "install": "xlings" },
    { "id": "kit", "pattern": "mcppls-kit-{kit}-*.tar.gz", "per-platform": true, "required": true,
      "what": "A target's kit", "install": "xlings" }
  ],
  "not-in-a-release": []
})";

constexpr std::string_view LOCK = R"({"clangd-version": "23.1.0-mcppls.5", "libcxx-version": "23.1.0", "entries": {},
    "platforms": {"linux-x64": {"kit": {"name": "x86_64-linux-gnu"}}, "win32-x64": {"kit": {"name": "x86_64-w64-mingw32"}}}})";

// A file of the release that only has to be there: its bytes are not read.
void asset(const std::string& directory, std::string_view name) { write(base::join_path(directory, std::string { name }), "x"); }

// The openkal tree, as `mcpp pack --target openkal --mode self-contained --systems all --pkg-format zip` writes it:
// one top directory and mcpp-package.json saying where it runs.
void make_tree(const std::string& directory, std::string_view version) {
    const std::string source { base::join_path(directory, "tree-source") };
    write(base::join_path(source, "mcpp-package.json"),
          R"({"schema": 1, "runs_on": {"self": ["linux-aarch64", "linux-x86_64", "macos-aarch64", "windows-x86_64"], "with": {"okrun": ["macos-x86_64"]}}})");
    write(base::join_path(source, "bin/mcppls"), "#!/bin/sh\n");
    const std::string name { std::format("mcppls-{}-openkal", version) };
    const std::vector<ar::WriteEntry> entries { { base::join_path(source, "mcpp-package.json"), "mcpp-package.json" }, { base::join_path(source, "bin"), "bin" } };
    auto wrote = ar::write(base::join_path(directory, name + ".zip.tar.gz"), name, entries);
    if (!wrote) throw std::runtime_error(wrote.error().message);
    // The writer writes a gzip-compressed tar; the reader reads by content, so the name the release gives it is kept.
    std::filesystem::rename(base::join_path(directory, name + ".zip.tar.gz"), base::join_path(directory, name + ".zip"));
    std::filesystem::remove_all(source);
}

std::string make_root() {
    const std::string root { scratch("root") };
    write(base::join_path(root, "LICENSE"), "Apache-2.0\n");
    write(base::join_path(root, "packaging/release.manifest.json"), RELEASE_MANIFEST);
    write(base::join_path(root, "packaging/payload.lock.json"), LOCK);
    write(base::join_path(root, "packaging/xlings/mcpp-language-server.lua.in"),
          "archs = {@ARCHS@}\ndeps = { \"xim:llvm-tools@@CLANGD_VERSION@\", \"xim:mcppls-kit-openkal@@OPENKAL_KIT_VERSION@\" }\nxpm = {\n@XPM_SYSTEMS@}\n");
    write(base::join_path(root, "packaging/xlings/mcppls-kit.lua.in"),
          "name = \"mcppls-kit-@KIT_NAME@\"\nwhat = \"@KIT_WHAT@\"\nlicenses = {@KIT_LICENSES@}\nref = \"@KIT_VERSION@\"\nurl = \"@KIT_URL@\"\nsha256 = \"@KIT_SHA256@\"\nfor = \"@KIT_TARGET@\"\n");
    return root;
}

} // namespace

int main() {
    using namespace mcppls::testing;

    "release check: a staged directory matching the manifest passes, its kits named by the lock"_test = [&] {
        const std::string root { make_root() };
        const std::string staged { scratch("staged") };
        for (std::string_view name : { "mcppls-payload-0.0.20.tar.gz", "mcppls-kit-openkal-0.19.0.tar.gz", "mcppls-kit-x86_64-linux-gnu-23.1.0.tar.gz",
                                       "mcppls-kit-x86_64-w64-mingw32-23.1.0.tar.gz" }) {
            asset(staged, name);
        }
        auto checked = release::check_release(root, "0.0.20", staged, false);
        expect(checked.has_value());
        if (checked) expect(checked->ok) << (checked->problems.empty() ? std::string {} : checked->problems.front());
        asset(staged, "mcppls-kit-riscv64-linux-gnu-23.1.0.tar.gz");
        checked = release::check_release(root, "0.0.20", staged, false);
        expect(checked.has_value() && !checked->ok) << "a kit of no platform is undeclared";
        std::filesystem::remove_all(root);
    };

    "release check: a missing required file fails, a kit of a platform included"_test = [&] {
        const std::string root { make_root() };
        const std::string staged { scratch("staged-partial") };
        for (std::string_view name : { "mcppls-payload-0.0.20.tar.gz", "mcppls-kit-openkal-0.19.0.tar.gz", "mcppls-kit-x86_64-linux-gnu-23.1.0.tar.gz" }) {
            asset(staged, name);
        }
        auto checked = release::check_release(root, "0.0.20", staged, false);
        expect(checked.has_value());
        if (checked) {
            expect(!checked->ok);
            expect(std::ranges::any_of(checked->problems, [](const std::string& p) { return p.contains("mcppls-kit-x86_64-w64-mingw32-*"); }));
        }
        std::filesystem::remove_all(root);
    };

    "release xlings: one server asset for the systems it runs on, one descriptor per kit"_test = [&] {
        const std::string root { make_root() };
        const std::string releaseDir { scratch("release") };
        make_tree(releaseDir, "0.0.20");
        for (std::string_view name : { "mcppls-kit-openkal-0.19.0.tar.gz", "mcppls-kit-x86_64-linux-gnu-23.1.0.tar.gz", "mcppls-kit-x86_64-w64-mingw32-23.1.0.tar.gz" }) {
            asset(releaseDir, name);
        }
        const std::string out { scratch("xlings-out") };
        auto result = release::make_xlings_artifacts(root, releaseDir, out, std::string { "0.0.20" }, std::string { "https://example.org/r/" }, std::nullopt);
        expect(result.has_value()) << (result ? std::string {} : result.error().message);
        if (!result) return;
        expect(result->written.size() == 4u);
        const auto server = os::fs::read(base::join_path(out, "mcpp-language-server.lua")).value_or("");
        expect(server.contains("archs = {\"aarch64\", \"x86_64\"}"));
        expect(server.contains("xim:llvm-tools@23.1.0\"")) << "the engine's LLVM base";
        expect(server.contains("xim:mcppls-kit-openkal@0.19.0\""));
        expect(server.contains("url = \"https://example.org/r/mcppls-0.0.20-openkal.zip\""));
        expect(server.contains("macosx = {") && server.contains("windows = {") && server.contains("linux = {"));
        const auto mac = server.substr(server.find("macosx = {"));
        expect(mac.substr(0, mac.find("},\n        },")).find("x86_64") == std::string::npos) << "macOS runs on arm64 only by itself";
        const auto kit = os::fs::read(base::join_path(out, "mcppls-kit-x86_64-w64-mingw32.lua")).value_or("");
        expect(kit.contains("name = \"mcppls-kit-x86_64-w64-mingw32\"") && kit.contains("ref = \"23.1.0\"") && kit.contains("ZPL-2.1"));
        expect(kit.contains("url = \"https://example.org/r/mcppls-kit-x86_64-w64-mingw32-23.1.0.tar.gz\""));
        expect(!kit.contains("@KIT"));
        expect(os::fs::read(base::join_path(out, "mcppls-kit-openkal.lua")).value_or("").contains("ref = \"0.19.0\""));
        std::filesystem::remove_all(root);
    };

    "release xlings: a missing kit is an error naming the platform"_test = [&] {
        const std::string root { make_root() };
        const std::string releaseDir { scratch("release-partial") };
        make_tree(releaseDir, "0.0.20");
        asset(releaseDir, "mcppls-kit-openkal-0.19.0.tar.gz");
        asset(releaseDir, "mcppls-kit-x86_64-linux-gnu-23.1.0.tar.gz");
        auto result = release::make_xlings_artifacts(root, releaseDir, scratch("xlings-out-partial"), std::string { "0.0.20" }, std::nullopt, std::nullopt);
        expect(!result.has_value());
        if (!result) expect(result.error().message.contains("win32-x64")) << result.error().message;
        std::filesystem::remove_all(root);
    };

    return report();
}
