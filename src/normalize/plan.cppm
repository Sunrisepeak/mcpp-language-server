// The engine database for one context: which units are written, with which
// arguments, which standard library units are injected, and what could not be
// resolved (design section 14.4, "remaining steps").
export module mcppls.normalize.plan;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.text;
import mcppls.spec.database;
import mcppls.spec.kit;
import mcppls.spec.metadata;
import mcppls.toolchain.probe;
import mcppls.project.scan;

export namespace mcppls::normalize {

// The arguments a standard library unit is built with, from a representative unit's (G-1, plan 2026-09-30).
// A project's own macros do not reach std: the build compiles std on its own (mcpp's `mcpp:std` set,
// CMake's synthetic target), and one of those macros can be a standard library's own header guard --
// GalTranslPP defines _RANGES_, which is MSVC STL's guard of <ranges>, and std built with it has no
// std::views. Kept are only the macros that configure a standard library (debug levels, ABI and
// hardening switches); -include and -imacros go too. `dropped` receives what was left out.
std::vector<std::string> std_unit_arguments(std::span<const std::string> representative, std::vector<std::string>* dropped = nullptr);
// Whether a macro configures the standard library (and so must be the same for std and its importers).
bool standard_library_macro(std::string_view name);

struct EngineEntry {
    std::string directory;
    std::string file;
    std::vector<std::string> arguments;   // argv[0] first, source last
    std::string provides;                 // the module the unit provides when it is importable
    std::string module;                   // the module the unit is part of, whatever its role (M for M's interface, partitions and implementation units)
    std::vector<std::string> imports;     // the modules it imports directly
    // Written before the source (usable plan W7): -fmodule-file=<name>=<path> for every module the
    // unit reaches and -fmodule-output=<path> for the one it provides, at paths nothing writes. They
    // name each module's unit to clangd, which otherwise scans the whole database to find it.
    std::vector<std::string> moduleHints;
};

struct PlanIssue {
    std::string code;                     // unresolved-module | ambiguous-module | module-build-failed | toolchain-not-found | sdk-missing
    std::string message;
    std::string file;
    std::string module;
    // Whose problem it is (S3 status issue category, import-hang plan §6): "code" when the fix is in the
    // file's own source (an import of a module nothing provides, a module that does not compile), else
    // "project" or "environment".
    std::string category { "project" };
};

// A module the engine database provides, for scheduling its build (usable plan W7).
struct PlannedModule {
    std::string name;
    std::vector<std::string> requires_;
    std::string primeFile;                // the `import M;` unit in the engine database; empty for partitions
};

struct EnginePlan {
    std::vector<EngineEntry> entries;
    std::vector<PlannedModule> modules;
    std::vector<std::pair<std::string, std::string>> primeSources;   // prime file -> its content
    // Stand-ins (robustness design C2): an empty unit per module nothing usable provides, and those modules.
    std::vector<std::pair<std::string, std::string>> stubSources;    // stand-in file -> its content
    std::vector<std::string> stubModules;
    std::vector<std::string> openSources;     // open files no set describes, planned with the nearest unit's arguments
    bool standInsDeferred { false };          // a stand-in was held back for a file being edited: plan again once it is not
    std::vector<PlanIssue> issues;
    std::vector<std::string> excludedFiles;   // providers left out because they cannot be built
    std::string contextSet;                   // empty: every set
    std::size_t stdUnits { 0 };
    // Set by the workspace, not by plan_engine (fix plan F4, F14): what the model's toolchain, profile and
    // context are, and where the model came from. A restart for a plan whose toolchain key changed is
    // the person's doing (they switched the toolchain or the context) and is never counted against clangd.
    std::string toolchainKey;
    std::string modelOrigin;                  // cache-fresh | cache-stale | cache-confirmed | producer | inferred
    // R-4 (plan 0.0.8): the model is the provisional one -- sources read with the kit's commands while the build tool,
    // which the project has, has not answered yet. Its module commands are replaced when it does.
    bool provisional { false };
    // The C++ standard the context's module units are read with (C++26 alignment): one for the whole context, since
    // every BMI a unit imports must have been built with its standard. When the units name several, the newest is
    // taken and the others raised to it; `standardsSeen` lists what they named, `standardsRaised` how many were.
    std::string languageStandard;
    std::vector<std::string> standardsSeen;
    std::size_t standardsRaised { 0 };
    // X-3 (plan 0.0.8 part 2): no module unit named a standard, and `languageStandard` is C++23, mcppls's own choice.
    bool standardAssumed { false };
};

// "c++26", "gnu++2c", "c++latest": the year of the C++ standard (2026) and whether GNU extensions are on; nullopt for
// anything that is not a C++ standard ("c17", "gnu11").
std::optional<std::pair<int, bool>> cxx_standard(std::string_view standard);

struct PlanInput {
    const spec::Database* database { nullptr };
    std::string contextSet;                                          // a set name, or empty for every set
    const std::map<std::string, toolchain::ToolchainFacts, std::less<>>* facts { nullptr };   // by toolchain id
    const spec::Kit* kit { nullptr };                                // used by sets without a usable toolchain
    std::string engineDriverDirectory;                               // where synthetic driver names are placed
    std::string macosSdk;                                            // for kits that require it
    std::function<project::ScanResult(std::string_view path)> scanner;
    spec::MetadataReader metadataReader;
    // Modules the engine reported it cannot find, with the reason: providers importing one cannot be
    // built and are left out like providers of an unresolvable import. A module that was found and did
    // not compile is not one of them; its importers stay (robustness design C3).
    std::map<std::string, std::string, std::less<>> unresolvedModules;
    // Where the model being planned came from (the workspace's origin: "inferred" for the provisional model of
    // sources alone, "producer", "cache-..."): what the core engine concluded under a provisional model is no
    // conclusion about the build tool's (plan 2026-09-30 §13, the xlings cold start).
    std::string modelOrigin;
    // Where `import M;` units for parallel preparation are written; empty: none are planned.
    std::string primeDirectory;
    // The directory module hints name; nothing is created there. Empty: no hints.
    std::string moduleHintDirectory;
    // Where stand-ins for modules nothing usable provides are written (robustness design C2). Empty: no
    // stand-ins; providers whose imports cannot resolve leave the database instead.
    std::string stubDirectory;
    // Files the editor has open that no set describes: they join the database with the arguments of the nearest C++
    // unit, so their imports resolve or get stand-ins instead of clangd guessing (robustness design C2).
    std::vector<std::string> openSources;
    // Open files the editor is changing right now (import-hang plan §5): an import of theirs that nothing provides is
    // most likely still being typed, so it gets no stand-in yet, unless the file provides a module itself (building
    // such a unit with an unresolved import is what stalls clangd).
    std::vector<std::string> editingSources;
    // Of those, the modules each imports in its text ON DISK (fix plan F13, WA-CLANGD-002's premise). clangd reads a file's
    // imports from disk (UP-14): an import an autosave has already written there gets its stand-in at once, since clangd
    // builds with it now and stalls on it unresolved (UP-02); only an import in the buffer alone waits for the file to be
    // quiet. A file without an entry here: all of its imports wait, as before.
    std::map<std::string, std::vector<std::string>, std::less<>> editingDiskImports;
    // Engine decisions (overall design 5.4), set by the core engine's configure_plan. Providers whose
    // imports cannot resolve, and providers importing them, stay out of the database: clangd 23.1
    // deadlocks building them (robustness design, experiments S2, S6). Other units always stay.
    bool excludeUnresolvedImports { true };
    // Whether clangd also stalls building a unit that provides no module, or a file whose text on disk imports, a
    // module nothing provides (WA-CLANGD-013). Where it does not, such a unit keeps the import, which clangd reports,
    // and gets no stand-in; a stand-in for a module unit's own import waits while the unit is being edited, on disk or
    // not, until the unit is quiet: only its importers' features hang on it, not clangd.
    bool importersStallUnresolved { true };
    // MSVC STL contexts turn aligned allocation off (clangd 23.1.0, usable plan E8/E9).
    bool noAlignedAllocationWithMsvcStl { true };
    // The core engine could not build the toolchain's standard library module (robustness design C5): C++
    // units are read with the semantic kit instead, as for a set without a usable toolchain.
    bool preferKit { false };
};

EnginePlan plan_engine(const PlanInput& input);
// The compile database clangd reads. Without module hints it is the database's structure: two
// plans that differ only in hints need no engine restart, since clangd rereads the file itself.
nlohmann::json to_compile_commands(const EnginePlan& plan, bool moduleHints = true);
base::Result<void> write_engine_database(std::string_view directory, const EnginePlan& plan);

} // namespace mcppls::normalize
