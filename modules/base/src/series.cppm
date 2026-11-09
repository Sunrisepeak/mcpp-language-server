// The series rule (docs/92-release.md "Versions"): a version is X.Y.Z, X.Y the series the server and
// every editor plugin share, Z each component's own patch. A plugin works with any server of its series.
export module mcppls.base.series;

import std;

export namespace mcppls::base {

// The X.Y of a three-part version (a pre-release or build suffix allowed); nothing for any other text.
std::optional<std::string> series_of(std::string_view version);

// Why a plugin that stated its version cannot use this server (`server` is the server's version), or
// nothing when it can: the same series, or a version of another shape, about which nothing is known.
std::optional<std::string> plugin_series_problem(std::string_view plugin, std::string_view pluginVersion,
                                                 std::string_view server);

} // namespace mcppls::base
