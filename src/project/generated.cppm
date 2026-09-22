// Recovering a generated module's real source (design P2, the incident of §7 in the workstream
// plan): a build writes some modules only when it runs (a dependency's std shim, a code generator's
// output) into MCPP_OUT_DIR / a package's `out` directory. When the database that describes the
// project is older than the build directory it named — the incident this module exists for: an old
// mcpp's fallback compile_commands.json outlives the `target/` a later `mcpp build` deleted and
// recreated — the file that database names is gone, but the real generated unit often still exists
// where a build leaves it: the project's own build directory, or mcpp's own build-database cache,
// which survives a deleted `target/` because it is keyed by the package and version, not the path.
// A stand-in (robustness design C2) stays the last resort; this is what runs before reaching for it.
export module mcppls.project.generated;

import std;
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

} // namespace mcppls::project
