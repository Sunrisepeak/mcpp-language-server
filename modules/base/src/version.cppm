export module mcppls.base.version;

import std;

export namespace mcppls::base {

// The version this binary reports --- `mcppls version`, the LSP and MCP serverInfo, the daemon
// handshake, a review report's metadata. It is what a user quotes in a bug report, so it is
// checked against mcpp.toml (the one source) by `mcppls-devtools version --check`, not kept in step by
// hand. Three constants that lived here and nothing read were removed rather than left to drift:
// the S1 profile version is spec::PROFILE_VERSION, the kit manifest version is spec::KIT_VERSION.
inline constexpr std::string_view VERSION { "0.0.3" };
// The clangd the payload ships. Checked against packaging/payload.lock.json by the same command.
inline constexpr std::string_view CLANGD_VERSION { "23.1.0" };
// The oldest mcpp that answers `mcpp emit build-database` — the `mcpp.build-database` kind, which
// mcpp's own docs date to "mcpp 2026.9.15.1+" (docs/50-machine-output.md §8). An older mcpp, or one
// that does not advertise the kind, is not an error: the server degrades to whatever
// compile_commands.json exists. This constant exists so the log can say what would remove the
// degradation, rather than only that it happened.
inline constexpr std::string_view MINIMUM_MCPP_VERSION { "2026.9.15.1" };

} // namespace mcppls::base
