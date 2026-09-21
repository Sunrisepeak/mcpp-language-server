# os

Three packages, `linux`, `macos` and `windows`, each exporting the same module,
`mcppls.os`, with the same shape: `Family`, `FAMILY`, `FAMILY_NAME`,
`EXECUTABLE_SUFFIX`, `PATH_LIST_SEPARATOR`, `VSCODE_TARGET`,
`CASE_INSENSITIVE_PATHS`. Only one of the three is ever in a build's dependency
graph — `mcpp.toml`'s `[target.'cfg(os = "...")'.dependencies]` picks it by the
build target, so it is a compile-time choice, not a runtime one.

The one thing to get right: because exactly one package is present, code
consumes these as compile-time constants with `if constexpr (mcppls::os::FAMILY
== ...)`, never with `#ifdef` — there is no preprocessor macro to branch on, and
no need for one, since the platform never varies within a single build. The
value differences live entirely in `<platform>/src/os.cppm`; everything else
about each package (`mcpp.toml`) is identical.
