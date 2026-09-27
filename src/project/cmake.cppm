// CMake as a data source: its build database when exported, its compile
// database otherwise, and a private configure in trusted workspaces (design D12).
export module mcppls.project.cmake;

import std;
import mcppls.base.error;
import mcppls.project.infer;
import mcppls.project.provider;

export namespace mcppls::project {

// The UUID CMake's CMAKE_EXPERIMENTAL_EXPORT_BUILD_DATABASE gate needs for the
// release whose `cmake --version` printed `versionOutput`, so the private
// configure can ask for CMAKE_EXPORT_BUILD_DATABASE without a wrong UUID
// failing the generate step. nullopt when the version could not be parsed or
// its build database has not been confirmed to merge correctly (see cmake.cpp
// for what was measured and why most releases are deliberately left out); the
// caller then configures exactly as it did before this table existed.
std::optional<std::string> build_database_gate_uuid(std::string_view versionOutput);

// B-3(b), 2026-09-27 plan §3.3: a `configurePresets` entry, `inherits` chains resolved and macros
// expanded, kept to the fields the private configure and `existing()` use. `cacheVariables` keeps
// only string-valued and `{type, value}` entries the plan calls for; `architecture`/`toolset` are
// read by nothing here (documented at the call site: a private configure has no IDE generator to
// hand them to).
struct ResolvedCMakePreset {
    std::string name;
    std::string binaryDir;         // expanded, normalized; empty when the preset does not set one
    std::string generator;
    std::string toolchainFile;     // expanded; empty: none
    std::vector<std::pair<std::string, std::string>> cacheVariables;
};

// Reads `CMakePresets.json` and `CMakeUserPresets.json` at `root` (user presets win: a preset the
// user file names again replaces the project's entirely, not merges with it -- the shape a personal
// preset override takes), resolves `inherits` chains (a preset may inherit several, later ones
// overriding earlier; a preset's own fields override anything inherited), and returns the first
// configure preset left that is not `hidden`, in the order presets were first named (CMakePresets.json,
// then any new names CMakeUserPresets.json added). There is no setting yet to name one by name (plan
// §3.3's own note); nullopt when neither file exists, both are empty of configure presets, or every
// one is hidden.
std::optional<ResolvedCMakePreset> resolve_cmake_preset(std::string_view root);

// The private configure's own argument list: `-S root -B buildDirectory` and the compile database
// always; `-DFETCHCONTENT_FULLY_DISCONNECTED=ON` whenever `offline` (B-3(a), 2026-09-27 plan D1 --
// BD7's first-configure exception is withdrawn: it is passed on the very first configure too, and
// never when online); a resolved preset's toolchain file, cache variables and generator when one is
// given; the build-database gate and `-G Ninja` only when `useNinja` and `gateUuid` (from
// build_database_gate_uuid) both say to, and only when the preset (if any) did not choose a
// different generator. A free function so tests can check it without spawning cmake.
std::vector<std::string> cmake_configure_arguments(std::string_view root, std::string_view buildDirectory, bool useNinja,
                                                   const std::optional<std::string>& gateUuid, bool offline,
                                                   const ResolvedCMakePreset* preset = nullptr);

// Every dependency name an offline, `FETCHCONTENT_FULLY_DISCONNECTED=ON` configure's output named as
// not already populated (the measured cmake 4.4.2 message, B-3(a): "FETCHCONTENT_FULLY_DISCONNECTED
// is set to true, which requires the source directory for dependency fmt to already be populated"),
// in first-seen order; empty when `output` does not match this shape (a different configure failure).
std::vector<std::string> fetchcontent_missing_dependencies(std::string_view output);

// B-1: CMake's own `BuildSystemProvider` (registry order: after mcpp). `detect()` is the same
// file-system search `detect_project` used to do inline, now also considering a configure preset's
// `binaryDir` as a candidate before the conventional directory names (plan §3.3: a private configure
// that ignored the user's own preset could pick a different compiler than they actually build with).
class CmakeProvider final : public BuildSystemProvider {
public:
    std::string_view id() const override { return "cmake"; }
    std::optional<Claim> detect(std::string_view root) const override;
    std::optional<Answer> existing(const Claim& claim, const ProviderContext& context) const override;
    Answer describe(const Claim& claim, const ProviderContext& context) const override;
    std::vector<std::string> watch_inputs(const Claim& claim) const override;
};

} // namespace mcppls::project
