// Release-time work: checking a staged directory against packaging/release.manifest.json, and
// rendering the xim-pkgindex descriptors of a release's assets. The manifest data model lives in
// mcppls.pack.release (modules/pack), because that is packaging DATA rather than devtools
// orchestration -- see that module's own comment.
//
//   mcppls-devtools release check --version V --dir D [--render]
//   mcppls-devtools release xlings --release DIR --out DIR [--version V] [--url PREFIX] [--clangd-version C]
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

// Renders packaging/xlings/*.lua.in for the assets of `releaseDir`: the server's descriptor, one
// asset (mcppls-<version>-openkal.zip) whose system blocks name the architectures its
// mcpp-package.json `runs_on.self` covers, and one descriptor per kit (mcppls-kit-<name>-<version>.tar.gz,
// one asset for every system). `version` and `clangdVersion` default to mcpp.toml and
// packaging/payload.lock.json; `url` to the GitHub release of the version.
base::Result<XlingsResult> make_xlings_artifacts(const std::string& root, const std::string& releaseDir,
                                                  const std::string& outDir, std::optional<std::string> version,
                                                  std::optional<std::string> url,
                                                  std::optional<std::string> clangdVersion);

} // namespace mcppls::devtools::release

export namespace mcppls::devtools {

mcpplibs::cmdline::App release_command(bool& handled, int& status);

} // namespace mcppls::devtools
