export module mcppls.base.version;

import std;

export namespace mcppls::base {

// The version this binary reports --- `mcppls version`, the LSP and MCP serverInfo, the daemon
// handshake, a review report's metadata. It is what a user quotes in a bug report, so it is
// checked against mcpp.toml (the one source) by `mcppls-devtools version --check`, not kept in step by
// hand. Three constants that lived here and nothing read were removed rather than left to drift:
// the S1 profile version is spec::PROFILE_VERSION, the kit manifest version is spec::KIT_VERSION.
inline constexpr std::string_view VERSION { "0.0.12" };
// The clangd the payload ships (mcppls-clangd). Checked against packaging/payload.lock.json by the same command.
inline constexpr std::string_view CLANGD_VERSION { "23.1.0-mcppls.0" };
// Maintained-engine identity stays intact in logs and caches. Its LLVM base
// decides libc++ kit compatibility; unknown vendor suffixes remain unchanged.
inline std::string_view llvm_base_version(std::string_view engineVersion) {
    const std::size_t suffix { engineVersion.find("-mcppls.") };
    if (suffix == std::string_view::npos) return engineVersion;
    const auto release = engineVersion.substr(suffix + 8);
    const auto digits = [](std::string_view value) {
        return !value.empty() && std::ranges::all_of(value, [](char c) { return c >= '0' && c <= '9'; });
    };
    if (!digits(release)) return engineVersion;
    const auto base = engineVersion.substr(0, suffix);
    const auto first = base.find('.');
    const auto second = first == std::string_view::npos ? first : base.find('.', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos ||
        !digits(base.substr(0, first)) || !digits(base.substr(first + 1, second - first - 1)) ||
        !digits(base.substr(second + 1))) return engineVersion;
    return base;
}

// The oldest mcpp that answers `mcpp emit build-database` — the `mcpp.build-database` kind, which
// mcpp's own docs date to "mcpp 2026.9.15.1+" (docs/50-machine-output.md §8). An older mcpp, or one
// that does not advertise the kind, is not an error: the server degrades to whatever
// compile_commands.json exists. This constant exists so the log can say what would remove the
// degradation, rather than only that it happened.
inline constexpr std::string_view MINIMUM_MCPP_VERSION { "2026.9.15.1" };

} // namespace mcppls::base
