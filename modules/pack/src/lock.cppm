// Reading packaging/payload.lock.json: what to fetch for each platform's clangd and semantic kit.
//
// This ports fetch.py's `load_lock` and the platform lookups trim_clangd.py and assemble_payload.py
// both did through it (`lock["platforms"][platform]["clangd"]`, `lock["clangd-version"]`). The file
// itself does not move -- it stays data under packaging/, read here instead of by every script that
// used to import fetch.py for it.
export module mcppls.pack.lock;

import std;
import mcppls.base.error;
import mcppls.pack.fetch;

export namespace mcppls::pack::lock {

// packaging/payload.lock.json's `platforms.<name>.kit`: which entry to fetch for the kit's source
// and which recipe and target triple build it. mcppls.pack.kit (ported separately) is the recipe;
// this only carries what the lock records.
struct KitSource {
    std::string recipe;
    std::string source;   // a key into `entries`: the archive the recipe is built from
    std::string target;   // the triple passed to the recipe
};

// packaging/payload.lock.json's `platforms.<name>`: the one table of platforms. A name is
// `<os>-<arch>` (mcppls.pack.targets reads it); everything else that lists platforms -- the VS Code
// extension, the release manifest, CI's matrices -- is checked against these rows by
// `mcppls-devtools check platforms`.
struct Platform {
    std::string clangd;         // a key into `entries`: the clangd release for this platform
    KitSource kit;
    std::string serverTarget;   // the mcpp `--target` the payload's server is built for
};

// packaging/payload.lock.json's `entries.<name>.license-from`: for an archive that carries no
// LICENSE.TXT of its own (LLVM's Linux arm64 release), where the same license text is -- one
// member of another entry's archive, named relative to that archive's top-level directory.
struct LicenseFrom {
    std::string entry;    // a key into `entries`
    std::string member;   // e.g. "llvm/LICENSE.TXT"
};

// packaging/payload.lock.json, parsed. `entries` and `platforms` keep the file's own key order out
// of it -- both are looked up by name, never iterated for order.
struct Lock {
    std::string clangdVersion;
    std::string libcxxVersion;
    std::map<std::string, fetch::Entry> entries;
    std::map<std::string, LicenseFrom> licenseFrom;   // keyed by the entry it is for
    std::map<std::string, Platform> platforms;
};

// Reads and parses the lock file at `path` (packaging/payload.lock.json under the repository root).
base::Result<Lock> load(std::string_view path);

// The entry named `name`, or an error naming the ones the lock does have (fetch.py's
// "unknown lock entry").
base::Result<fetch::Entry> entry(const Lock& lockData, std::string_view name);

// The platform block named `platformName`, or an error naming the ones the lock does have
// (fetch.py's "unknown platform").
base::Result<Platform> platform(const Lock& lockData, std::string_view platformName);

// Every platform name the lock has, in name order.
std::vector<std::string> platform_names(const Lock& lockData);

} // namespace mcppls::pack::lock
