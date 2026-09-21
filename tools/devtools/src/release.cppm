// Release-time work: checking a staged directory against packaging/release.manifest.json, and
// splitting CI's payload tarballs into the xlings-res archives a release publishes. Ported from
// packaging/scripts/release_manifest.py and packaging/scripts/xlings_artifacts.py; the manifest
// data model lives in mcppls.pack.release (modules/pack), because that is packaging DATA rather
// than devtools orchestration -- see that module's own comment.
//
//   mcppls-devtools release check --version V --dir D [--render]
//   mcppls-devtools release xlings --payloads DIR --out DIR [--version V] [--kit-version K] [--clangd-version C]
export module mcppls.devtools.release;

import std;
import mcpplibs.cmdline;
import mcppls.base.error;
import mcppls.pack.release;

export namespace mcppls::devtools::release {

// packaging/release.manifest.json, checked (and, when `render` is set, rendered as MANIFEST.md
// into `directory`) against a staged release directory.
base::Result<mcppls::pack::release::CheckReport> check_release(const std::string& root, const std::string& version,
                                                                const std::string& directory, bool render);

struct XlingsResult {
    std::vector<std::string> written;   // every file this wrote, in the order it wrote them
};

// Splits `payloadsDir`'s payload-<platform>.tar.gz files (each with a payload/ inside, as CI's
// payload artifacts carry it) into xlings-res server and kit archives under `outDir`, and renders
// packaging/xlings/*.lua.in with their sha256. `version`, `kitVersion` and `clangdVersion` default
// to mcpp.toml and packaging/payload.lock.json, the same as the Python original.
base::Result<XlingsResult> make_xlings_artifacts(const std::string& root, const std::string& payloadsDir,
                                                  const std::string& outDir, std::optional<std::string> version,
                                                  std::optional<std::string> kitVersion,
                                                  std::optional<std::string> clangdVersion);

} // namespace mcppls::devtools::release

export namespace mcppls::devtools {

mcpplibs::cmdline::App release_command(bool& handled, int& status);

} // namespace mcppls::devtools
