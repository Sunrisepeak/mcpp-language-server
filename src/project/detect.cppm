// Which kind of project a workspace folder is, and where its build facts live
// (design section 14.1). B-1 (2026-09-27 plan §3.2, §3.6) moved the how of detecting each kind into
// that kind's own provider (mcppls.project.provider's `BuildSystemProvider`); this module keeps only
// what every kind shares: the vocabulary (`SourceKind`, `tier_of`), the configured-database
// exception (kept first: an explicit `mcppls.database` is never second-guessed by a provider), and
// asking the registry in order for everything else.
export module mcppls.project.detect;

import std;
import mcppls.project.provider;

export namespace mcppls::project {

// xmake and meson (B-5, B-6) join cmake and compile_commands as tier 3: like a bare
// compile_commands.json, they give exact compile arguments but no module roles of their own (mcpp
// and CMake's FILE_SET CXX_MODULES say which unit provides which module; xmake's and meson's
// compile databases do not), so mcppls recovers roles the same way it does for compile_commands --
// by scanning. They are placed ahead of a bare compile_commands.json in the registry (design plan
// §3.2 table) because a project that has a build system described this precisely by *something*,
// even when that something is not mcpp or CMake.
enum class SourceKind { build_database, mcpp, cmake, xmake, meson, compile_commands, inferred };

std::string_view to_string(SourceKind kind);

// The README's and design §2.1's L1..L4: which kind of source described the model, independent of
// S1's own 1..4 conformance level (how completely that source's *document* is structured). The two
// numbers happened to share a range and a user reading "level 2" could not tell which one it was;
// `project.tier` (S3) is this one, `project.level` stays S1's. build-database and mcpp are both the
// best case (a real build tool's or a hand-written database's own word); cmake without a database
// file falls back one tier below it, xmake/meson/a bare compile_commands.json one more, and inferred
// (scanning, or an untrusted workspace, which is L4 by definition) is the last.
int tier_of(SourceKind kind);

// The `SourceKind` a provider's own `id()` names ("mcpp", "cmake", "xmake", "meson",
// "compile-commands"); `inferred` for anything else, since inference is not a provider (design plan
// §3.2: "inferred 不是提供者，是所有提供者都不可用时的兜底").
SourceKind kind_of(std::string_view providerId);

struct Detection {
    SourceKind kind { SourceKind::inferred };
    std::string root;
    std::string manifest;          // mcpp.toml, CMakeLists.txt, xmake.lua or meson.build
    std::string buildDirectory;    // an existing build directory (CMake, or xmake's/meson's private one once described)
    std::string compileCommands;   // an existing compile_commands.json
    std::string buildDatabase;     // an existing P2977 / S1 document
};

// Asks `providers` in order (registry order: mcpp, cmake, xmake, meson, compile-commands --
// `registered_providers()` in mcppls.project.providers) and takes the first claim, keeping the
// precedence conformance fixtures depend on. `configuredDatabase` (`mcppls.database`) is checked
// first and unconditionally, ahead of every provider (B-1 task 1). `buildDiscovery = false` (the
// setting of the same name) skips the provider loop entirely -- nothing is detected, only an
// explicit database is read, and the caller falls back to inference otherwise (the setting's own
// notice, `build-discovery-off`, is added by `load_project`, not here: detection alone is cheap and
// side-effect free either way, so what kind of project this looks like is still worth knowing).
Detection detect_project(std::string_view root, std::string_view configuredDatabase = {},
                         std::span<BuildSystemProvider* const> providers = {}, bool buildDiscovery = true);

} // namespace mcppls::project
