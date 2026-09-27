// Recovering a generated module's real source (real-project plan RP2.3, design P2): a build writes
// some modules only when it runs (a dependency's std shim, a code generator's output) into
// MCPP_OUT_DIR / a package's `out` directory. When the database that describes the project is older
// than the build directory it named — the incident this module exists for: an old mcpp's fallback
// compile_commands.json outlives the `target/` a later `mcpp build` deleted and recreated — the
// file that database names is gone, but the real generated unit often still exists where a build
// leaves it: the project's own build directory, or mcpp's own build-database cache, which survives
// a deleted `target/` because it is keyed by the package and version, not the path. A stand-in
// (robustness design C2) stays the last resort; this is what runs before reaching for it.
export module mcppls.project.generated;

import std;
import mcppls.spec.database;
import mcppls.project.infer;

export namespace mcppls::project {

struct GeneratedSourceOptions {
    std::string root;             // the workspace root
    std::string homeDirectory;    // platform::dirs::home_directory()
    Scanner scanner;              // required; a candidate is used only once it declares the module
};

// The first file under a known generated-output location that declares `export module <name>`
// (`std::nullopt` scanner results, or a declaration for a different module, are skipped). Searched,
// in order: the project's own `target/.build-mcpp/deps/<pkg>@<ver>/out/`, then every
// `~/.mcpp/cache/build-database/*/target/.build-mcpp/deps/<pkg>@<ver>/out/` mcpp has ever built (a
// `target/` mcpp built once and the project later deleted). Bounded: only these two conventional
// locations are read, never the whole home directory or a general recursive search.
std::optional<std::string> find_generated_source(std::string_view moduleName, const GeneratedSourceOptions& options);

// Generated build output a producer describes but never writes (plan 2026-09-27 Q1-3). mcpp plans in a
// private directory, `$MCPP_HOME/cache/build-database/<key>/`, and by its own specification runs no action
// there (SPEC-005 R2.1, R2.5), so what a rule's action generates -- `ui_mainwindow.h` from uic, moc's and
// rcc's sources -- is named in the database and never appears. A build of the project puts the same files
// under the project's own `target/`, at the same relative path. Include directories and sources in the
// private directory are replaced by the project's counterpart when that exists, read-only; the ones whose
// counterpart does not exist yet are reported, with where a build will put them, so the model is loaded
// again when it does.
struct GeneratedPaths {
    std::vector<std::string> relocated;   // private paths the project's own build output replaced
    std::vector<std::string> missing;     // private paths with nothing in them yet and no counterpart in the project
    std::vector<std::string> watch;       // glob patterns, relative to the root, where a build writes what is missing
};
GeneratedPaths use_project_build_output(spec::Database& database, std::string_view root);

// The part of `path` after a producer's private planning directory's `target/`, or nullopt when `path` is not
// in one: `<anything>/cache/build-database/<key>/target/.build-mcpp/out/qt` -> `.build-mcpp/out/qt`.
std::optional<std::string> private_target_relative(std::string_view path);

} // namespace mcppls::project
