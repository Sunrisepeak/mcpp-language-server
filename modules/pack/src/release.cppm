// What a release of mcpp-language-server carries, and whether a staged directory matches it.
// Ported from packaging/scripts/release_manifest.py; packaging/release.manifest.json is the
// declaration this reads, unchanged.
//
// This is packaging DATA logic -- what the manifest says a release is, and what "matches" means --
// not devtools orchestration, so it lives here rather than in tools/devtools/src: a future producer
// of a release directory besides `mcppls-devtools release check` (M6's `mcpp pack --format`, per
// the tooling architecture §5.6) would want the same answer without depending on devtools.
export module mcppls.pack.release;

import std;
import mcppls.base.error;

export namespace mcppls::pack::release {

struct Asset {
    std::string id;
    // "{platform}", "{version}" and "{kit}" (the platform's kit, packaging/payload.lock.json's `kit.name`)
    // placeholders, `std::format`-free; a `*` stands for a version the release does not name (a kit's).
    std::string pattern;
    bool perPlatform { false };
    bool required { false };
    std::string what;
    std::string install;
    // server, vscode, clion, zed, claude-code; `release` for what every release carries (SHA256SUMS, MANIFEST.md).
    std::string component { "release" };
};

struct Manifest {
    std::string releaseName;   // "{version}" placeholder
    std::vector<std::string> platforms;
    std::map<std::string, std::string> kits;   // platform -> its kit's name, for "{kit}"; the caller fills it from the lock
    std::vector<Asset> assets;
    std::vector<std::string> notInARelease;
};

base::Result<Manifest> load_manifest(std::string_view path);

// One file name this release declares, and the asset it comes from -- one row per platform for a
// per-platform asset, one row otherwise. A name with a `*` matches any file of that shape.
struct Expected {
    std::string name;
    Asset asset;
};
// THE VERSIONS OF A RELEASE (docs/92-release.md "Versions"): each component's own, which its assets' `{version}`
// stands for, and the components the release carries -- every one for a series release, one for a component's patch.
struct Versions {
    std::string release;                                // the release's own: the series' X.Y.0, or the component's
    std::map<std::string, std::string> byComponent;
    std::set<std::string> components;                   // empty: every component
    bool carries(std::string_view component) const {
        return component == "release" || components.empty() || components.contains(std::string { component });
    }
    std::string of(std::string_view component) const {
        const auto it = byComponent.find(std::string { component });
        return it == byComponent.end() ? release : it->second;
    }
};
// Every component at one version: the series release of that version.
Versions one_version(std::string_view version);

std::vector<Expected> expected_files(const Manifest& manifest, const Versions& versions);

// The MANIFEST.md text release_manifest.py's `--render` writes into the staged directory: every
// declared file, what it is, how to install it, and which ones are missing from THIS directory.
std::string render(const Manifest& manifest, const Versions& versions, const std::vector<Expected>& files,
                   const std::set<std::string>& onDisk);

struct CheckReport {
    bool ok { true };
    std::vector<std::string> problems;    // exit non-zero when non-empty
    std::vector<std::string> notes;       // "ok: NAME (N MB)" / "absent (optional): NAME" -- informational
};

// Checks `directory`'s files against what `manifest` declares for `versions`: every required asset of a
// component the release carries is present and not empty, and nothing else is there. When `renderAlso`
// is set, writes MANIFEST.md into the directory first (as release_manifest.py's `--render` does), so it
// is judged present for the rest of the check the same way SHA256SUMS is.
base::Result<CheckReport> check(const Manifest& manifest, const Versions& versions, std::string_view directory,
                                bool renderAlso);

} // namespace mcppls::pack::release
