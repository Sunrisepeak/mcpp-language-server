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
    std::string pattern;      // "{platform}" and "{version}" placeholders, `std::format`-free
    bool perPlatform { false };
    bool required { false };
    std::string what;
    std::string install;
};

struct Manifest {
    std::string releaseName;   // "{version}" placeholder
    std::vector<std::string> platforms;
    std::vector<Asset> assets;
    std::vector<std::string> notInARelease;
};

base::Result<Manifest> load_manifest(std::string_view path);

// One file name this release declares, and the asset it comes from -- one row per platform for a
// per-platform asset, one row otherwise.
struct Expected {
    std::string name;
    Asset asset;
};
std::vector<Expected> expected_files(const Manifest& manifest, std::string_view version);

// The MANIFEST.md text release_manifest.py's `--render` writes into the staged directory: every
// declared file, what it is, how to install it, and which ones are missing from THIS directory.
std::string render(const Manifest& manifest, std::string_view version, const std::vector<Expected>& files,
                   const std::set<std::string>& onDisk);

struct CheckReport {
    bool ok { true };
    std::vector<std::string> problems;    // exit non-zero when non-empty
    std::vector<std::string> notes;       // "ok: NAME (N MB)" / "absent (optional): NAME" -- informational
};

// Checks `directory`'s files against what `manifest` declares for `version`. When `renderAlso` is
// set, writes MANIFEST.md into the directory first (as release_manifest.py's `--render` does), so
// it is judged present for the rest of the check the same way SHA256SUMS is.
base::Result<CheckReport> check(const Manifest& manifest, std::string_view version, std::string_view directory,
                                bool renderAlso);

} // namespace mcppls::pack::release
