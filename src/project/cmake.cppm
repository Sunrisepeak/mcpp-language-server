// CMake as a data source: its build database when exported, its compile
// database otherwise, and a private configure in trusted workspaces (design D12).
export module mcppls.project.cmake;

import std;
import mcppls.base.error;
import mcppls.project.detect;
import mcppls.project.infer;
import mcppls.project.provider;

export namespace mcppls::project {

base::Result<InferredDatabase> load_cmake(const Detection& detection, std::string_view privateBuildDirectory, const ProviderContext& context);

// The UUID CMake's CMAKE_EXPERIMENTAL_EXPORT_BUILD_DATABASE gate needs for the
// release whose `cmake --version` printed `versionOutput`, so the private
// configure can ask for CMAKE_EXPORT_BUILD_DATABASE without a wrong UUID
// failing the generate step. nullopt when the version could not be parsed or
// its build database has not been confirmed to merge correctly (see cmake.cpp
// for what was measured and why most releases are deliberately left out); the
// caller then configures exactly as it did before this table existed.
std::optional<std::string> build_database_gate_uuid(std::string_view versionOutput);

// The private configure's own argument list: `-S root -B buildDirectory` and
// the compile database always; the build-database gate and `-G Ninja` only
// when `useNinja` and `gateUuid` (from build_database_gate_uuid) both say to.
// A free function so tests can check it without spawning cmake.
std::vector<std::string> cmake_configure_arguments(std::string_view root, std::string_view buildDirectory, bool useNinja,
                                                   const std::optional<std::string>& gateUuid);

} // namespace mcppls::project
