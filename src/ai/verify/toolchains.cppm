// Cross-toolchain verification (overall design 7.3, work item RV6): the project built with each of
// several toolchains, and the errors only some of them report — what a change that compiles on one
// compiler breaks on another. mcpp projects only, trusted workspaces only; mcpp cannot build outside
// the project (experiment X9), so each toolchain builds a copy kept under the user cache.
export module mcppls.ai.verify.toolchains;

import std;
import nlohmann.json;
import mcppls.spec.query;
import mcppls.ai.query.view;

export namespace mcppls::ai::verify {

struct CompilerDiagnostic {
    std::string severity;               // error | warning
    std::string message;
    spec::Location location;            // in the workspace, as S5 names files
};

struct ToolchainBuild {
    std::string toolchain;              // an mcpp toolchain spec, e.g. gcc@16.1.0
    bool built { false };               // the build succeeded
    bool ran { false };                 // mcpp ran and finished in time
    int exitCode { -1 };
    double seconds { 0 };
    std::vector<CompilerDiagnostic> diagnostics;
    std::string output;                 // the end of what mcpp printed, when no diagnostic explains a failure
};

struct Divergence {
    std::string toolchain;              // the one reporting the error
    std::vector<std::string> others;    // the ones that do not
    CompilerDiagnostic diagnostic;
};

struct ToolchainComparison {
    std::vector<ToolchainBuild> builds;
    std::vector<Divergence> divergences;
};

query::Outcome<ToolchainComparison> compare_toolchains(query::View& view, std::span<const std::string> toolchains, query::Clock::time_point deadline);

// GCC and Clang (`file:line:column: error: message`) and MSVC (`file(line,column): error C1234: message`)
// diagnostics in a build's output; paths under `buildRoot` become workspace paths under `root`.
std::vector<CompilerDiagnostic> parse_compiler_output(std::string_view output, std::string_view buildRoot, std::string_view root,
                                                      const std::function<std::string(std::string_view path)>& textOf);

nlohmann::json to_json(const ToolchainComparison& comparison);

} // namespace mcppls::ai::verify
