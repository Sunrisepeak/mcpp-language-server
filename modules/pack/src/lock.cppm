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

// packaging/payload.lock.json's `platforms.<name>`.
struct Platform {
    std::string clangd;   // a key into `entries`: the clangd release for this platform
    KitSource kit;
};

// packaging/payload.lock.json, parsed. `entries` and `platforms` keep the file's own key order out
// of it -- both are looked up by name, never iterated for order.
struct Lock {
    std::string clangdVersion;
    std::string libcxxVersion;
    std::map<std::string, fetch::Entry> entries;
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

} // namespace mcppls::pack::lock
