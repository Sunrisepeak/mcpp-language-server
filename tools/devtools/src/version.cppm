// The project's version lives in mcpp.toml; everywhere else it is derived or checked. Ported from
// .github/scripts/version.py --- see that file's docstring for why a site nothing checked (the
// version the running binary reports) is the one a release without this could ship silently wrong.
//
//   mcppls-devtools version --print                 the product version
//   mcppls-devtools version --print --extension     the version the editor plugins carry (the same)
//   mcppls-devtools version --check                 every derived site agrees, or say which does not
//   mcppls-devtools version --set 0.0.2              write it everywhere
export module mcppls.devtools.version;

import std;
import mcpplibs.cmdline;
import mcppls.base.error;

export namespace mcppls::devtools::version {

// The version the editor plugins carry: the product version itself, which must be a three-part
// semantic version MAJOR.MINOR.PATCH -- the one shape the VS Code Marketplace, Open VSX, Zed and
// JetBrains all accept, so no plugin carries a version mapped from another. Anything else, a
// four-part date version included, is refused rather than published under a second name.
base::Result<std::string> extension_version(std::string_view product);

// Each site below reads its current value, and -- when `next` is given -- writes it and returns
// what was written. `root` is the repository root (mcppls-devtools locates it the same way every
// other command does, through mcppls::devtools::repository_root).
base::Result<std::string> manifest_version(const std::string& root, std::optional<std::string_view> next = std::nullopt);
base::Result<std::string> module_version(const std::string& root, std::optional<std::string_view> next = std::nullopt);
base::Result<std::string> extension_manifest_version(const std::string& root, std::optional<std::string_view> next = std::nullopt);
base::Result<std::string> zed_manifest_version(const std::string& root, std::optional<std::string_view> next = std::nullopt);
base::Result<std::string> clion_plugin_version(const std::string& root, std::optional<std::string_view> next = std::nullopt);
base::Result<std::string> claude_plugin_version(const std::string& root, std::optional<std::string_view> next = std::nullopt);
base::Result<std::string> claude_marketplace_version(const std::string& root, std::optional<std::string_view> next = std::nullopt);

// The oldest mcpp the server tells users is enough (MINIMUM_MCPP_VERSION), and the one CI actually
// builds and tests with (.github/versions.env). Neither is written by --set: both are read-only
// facts --check compares against each other.
struct McppVersions {
    std::optional<std::string> minimum;
    std::optional<std::string> tested;
};
base::Result<McppVersions> mcpp_versions(const std::string& root);

// The clangd version the server prints, and the one the payload locks.
struct ClangdVersions {
    std::optional<std::string> printed;
    std::optional<std::string> locked;
};
base::Result<ClangdVersions> clangd_versions(const std::string& root);

// The kit manifest version the server accepts, and the one build_kit.py writes.
struct KitVersions {
    std::optional<std::string> accepted;
    std::optional<std::string> written;
};
base::Result<KitVersions> kit_versions(const std::string& root);

// Every problem `--check` would report against the current mcpp.toml version; empty means every
// site agrees.
base::Result<std::vector<std::string>> check(const std::string& root);

// Writes `product` to every site --set writes: mcpp.toml, modules/base/src/version.cppm,
// editors/vscode/package.json, editors/zed/extension.toml, editors/clion/gradle.properties, and the
// Claude Code plugin's plugin.json and its marketplace entry.
base::Result<std::string> set_everywhere(const std::string& root, std::string_view product);

} // namespace mcppls::devtools::version

export namespace mcppls::devtools {

mcpplibs::cmdline::App version_command(bool& handled, int& status);

} // namespace mcppls::devtools
