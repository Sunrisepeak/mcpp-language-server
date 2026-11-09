module mcppls.base.series;

import std;

namespace mcppls::base {

std::optional<std::string> series_of(std::string_view version) {
    auto digits = [](std::string_view s) { return !s.empty() && std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; }); };
    const auto end = version.find_first_of("-+");
    const auto core = version.substr(0, end);
    const auto first = core.find('.');
    if (first == std::string_view::npos) return std::nullopt;
    const auto second = core.find('.', first + 1);
    if (second == std::string_view::npos) return std::nullopt;
    const auto major = core.substr(0, first), minor = core.substr(first + 1, second - first - 1), patch = core.substr(second + 1);
    if (!digits(major) || !digits(minor) || !digits(patch)) return std::nullopt;
    return std::format("{}.{}", major, minor);
}

std::optional<std::string> plugin_series_problem(std::string_view plugin, std::string_view pluginVersion, std::string_view server) {
    const auto ours = series_of(server);
    const auto theirs = series_of(pluginVersion);
    if (!ours || !theirs || *ours == *theirs) return std::nullopt;
    return std::format("this server is mcppls {} (series {}); the {} plugin {} works with series {}. Install a server of series {} "
                       "(xlings install mcppls@{}), or the plugin of series {}",
                       server, *ours, plugin.empty() ? std::string_view { "editor" } : plugin, pluginVersion, *theirs, *theirs, *theirs, *ours);
}

} // namespace mcppls::base
