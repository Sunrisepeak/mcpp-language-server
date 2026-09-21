// The release manifest data model (packaging/scripts/release_manifest.py, ported): what a release
// declares, what "matches a staged directory" means, and MANIFEST.md's rendering.
import std;
import mcppls.testing;
import mcppls.base.path;
import mcppls.pack.release;
import mcppls.platform.fs;
import mcppls.platform.dirs;

namespace release = mcppls::pack::release;
namespace fs = mcppls::platform::fs;
namespace base = mcppls::base;

namespace {

std::string scratch(std::string_view name) {
    const std::string directory { base::join_path(
        mcppls::platform::dirs::temp_directory(),
        std::format("mcppls-test-release-{}-{}", name, std::chrono::steady_clock::now().time_since_epoch().count())) };
    (void) fs::create_directories(directory);
    return directory;
}

// A small manifest, in the shape packaging/release.manifest.json actually has: one per-platform
// required asset, one non-per-platform optional asset, one required singleton.
constexpr std::string_view MANIFEST = R"({
  "release-name": "mcpp-language-server {version}",
  "platforms": ["linux-x64", "darwin-arm64"],
  "assets": [
    { "id": "payload", "pattern": "payload-{platform}.tar.gz", "per-platform": true, "required": true,
      "what": "The payload", "install": "Unpack it" },
    { "id": "clion", "pattern": "mcppls-clion-{version}.zip", "per-platform": false, "required": false,
      "what": "The CLion plugin", "install": "Install from disk" },
    { "id": "checksums", "pattern": "SHA256SUMS", "per-platform": false, "required": true,
      "what": "Checksums", "install": "sha256sum -c" },
    { "id": "manifest", "pattern": "MANIFEST.md", "per-platform": false, "required": true,
      "what": "This declaration, rendered", "install": "This page" }
  ],
  "not-in-a-release": ["The source code: it is the repository itself."]
})";

} // namespace

int main() {
    using namespace mcppls::testing;

    const std::string work { scratch("work") };
    const std::string manifestPath { base::join_path(work, "release.manifest.json") };
    expect(fs::write_file(manifestPath, MANIFEST).has_value());

    auto manifest = release::load_manifest(manifestPath);
    expect(manifest.has_value());
    if (!manifest) return 1;

    "expected_files expands per-platform assets across every platform"_test = [&] {
        const auto files = release::expected_files(*manifest, "2026.9.16.1");
        expect(files.size() == 5);   // 2 platforms x payload, + clion, + checksums, + manifest
        const bool sawLinux = std::ranges::any_of(files, [](const release::Expected& f) { return f.name == "payload-linux-x64.tar.gz"; });
        const bool sawMac = std::ranges::any_of(files, [](const release::Expected& f) { return f.name == "payload-darwin-arm64.tar.gz"; });
        expect(sawLinux);
        expect(sawMac);
    };

    "a directory with every required file and nothing undeclared matches"_test = [&] {
        const std::string directory { scratch("match") };
        expect(fs::write_file(base::join_path(directory, "payload-linux-x64.tar.gz"), "x").has_value());
        expect(fs::write_file(base::join_path(directory, "payload-darwin-arm64.tar.gz"), "x").has_value());
        expect(fs::write_file(base::join_path(directory, "SHA256SUMS"), "x").has_value());
        auto report = release::check(*manifest, "2026.9.16.1", directory, false);
        expect(report.has_value());
        if (!report) return;
        expect(report->ok);
        expect(report->problems.empty());
        // the optional clion asset is absent, which is a note, not a problem
        const bool notedAbsent = std::ranges::any_of(report->notes, [](const std::string& n) { return n.contains("absent (optional)"); });
        expect(notedAbsent);
    };

    "a missing required file fails, naming it"_test = [&] {
        const std::string directory { scratch("missing") };
        expect(fs::write_file(base::join_path(directory, "payload-linux-x64.tar.gz"), "x").has_value());
        // darwin payload and SHA256SUMS both missing
        auto report = release::check(*manifest, "2026.9.16.1", directory, false);
        expect(report.has_value());
        if (!report) return;
        expect(!report->ok);
        const bool namesDarwin = std::ranges::any_of(
            report->problems, [](const std::string& p) { return p.contains("payload-darwin-arm64.tar.gz"); });
        expect(namesDarwin);
    };

    "an undeclared file fails, naming it"_test = [&] {
        const std::string directory { scratch("undeclared") };
        expect(fs::write_file(base::join_path(directory, "payload-linux-x64.tar.gz"), "x").has_value());
        expect(fs::write_file(base::join_path(directory, "payload-darwin-arm64.tar.gz"), "x").has_value());
        expect(fs::write_file(base::join_path(directory, "SHA256SUMS"), "x").has_value());
        expect(fs::write_file(base::join_path(directory, "nobody-declared-this.bin"), "x").has_value());
        auto report = release::check(*manifest, "2026.9.16.1", directory, false);
        expect(report.has_value());
        if (!report) return;
        expect(!report->ok);
        const bool namesIt = std::ranges::any_of(
            report->problems, [](const std::string& p) { return p.contains("nobody-declared-this.bin"); });
        expect(namesIt);
    };

    "an empty declared file fails"_test = [&] {
        const std::string directory { scratch("empty") };
        expect(fs::write_file(base::join_path(directory, "payload-linux-x64.tar.gz"), "").has_value());
        expect(fs::write_file(base::join_path(directory, "payload-darwin-arm64.tar.gz"), "x").has_value());
        expect(fs::write_file(base::join_path(directory, "SHA256SUMS"), "x").has_value());
        auto report = release::check(*manifest, "2026.9.16.1", directory, false);
        expect(report.has_value());
        if (!report) return;
        expect(!report->ok);
        const bool namesIt = std::ranges::any_of(report->problems, [](const std::string& p) { return p.contains("empty: payload-linux-x64.tar.gz"); });
        expect(namesIt);
    };

    "--render writes MANIFEST.md, and it counts as present for the rest of the check"_test = [&] {
        const std::string directory { scratch("render") };
        expect(fs::write_file(base::join_path(directory, "payload-linux-x64.tar.gz"), "x").has_value());
        expect(fs::write_file(base::join_path(directory, "payload-darwin-arm64.tar.gz"), "x").has_value());
        expect(fs::write_file(base::join_path(directory, "SHA256SUMS"), "x").has_value());
        auto report = release::check(*manifest, "2026.9.16.1", directory, true);
        expect(report.has_value());
        if (!report) return;
        expect(report->ok);
        expect(fs::is_regular_file(base::join_path(directory, "MANIFEST.md")));
        auto text = fs::read_file(base::join_path(directory, "MANIFEST.md"));
        expect(text.has_value());
        if (!text) return;
        expect(text->contains("mcpp-language-server 2026.9.16.1"));
        expect(text->contains("The source code: it is the repository itself."));
    };

    std::filesystem::remove_all(work);
    return report();
}
