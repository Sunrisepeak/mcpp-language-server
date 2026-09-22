// The project model a session works with: an S1 database from the best source
// available, the facts about its toolchains, and what degraded along the way.
// Loading never fails; the last resort is inference from sources.
export module mcppls.project.model;

import std;
import mcppls.spec.database;
import mcppls.spec.kit;
import mcppls.toolchain.probe;
import mcppls.project.detect;
import mcppls.project.infer;

export namespace mcppls::project {

struct ModelIssue {
    std::string code;
    std::string message;
};

struct SemanticProfile {
    std::string kind;       // build-toolchain | semantic-kit
    std::string compiler;   // "gcc 16.1.0"; empty for a kit
    std::string stdlib;     // "libstdc++ 16.1.0" | "libc++ 23.1.0"
    std::string target;
};

struct ProjectModel {
    std::string root;
    SourceKind source { SourceKind::inferred };
    SourceKind detected { SourceKind::inferred };   // the kind of project found; `source` is inferred when its data was not available
    int level { 2 };
    int tier { 4 };                                 // S3 `project.tier`: how the model was obtained, the README's L1..L4 (distinct from `level`, S1's own)
    spec::Database database;
    FactsMap facts;
    bool usesKit { false };
    std::vector<std::string> watch;           // glob patterns relative to the root
    std::vector<ModelIssue> issues;
    std::vector<ModelIssue> notices;          // informational: no feature is reduced
    SemanticProfile profile;
    std::string producer;                     // the program that described the build, when one did
    std::string producerVersion;
};

struct LoadOptions {
    bool trusted { false };
    std::string cacheDirectory;               // <cache>/workspaces/<hash>
    std::string configuredDatabase;
    std::string compilerOverride;             // mcppls.compiler; "kit" forces the semantic kit
    std::string mcppExecutable;               // the producer for mcpp projects; empty: found on PATH
    std::string homeDirectory;                // empty: platform::dirs::home_directory() (a test override otherwise)
    bool discoverCompilers { true };          // false: loose sources use the kit
    const spec::Kit* kit { nullptr };
    toolchain::Runner runner;
    toolchain::ProbeCache* probeCache { nullptr };
    Scanner scanner;

    // How the build tool is run (design 4.2, 4.4); passed straight through to ProviderContext.
    bool offline { true };
    bool runBuildTool { true };               // false (mcppls.buildTool = off): never start the build tool
    std::chrono::milliseconds producerHard { std::chrono::seconds { 60 } };
    std::chrono::milliseconds producerSoft { std::chrono::seconds { 5 } };
    std::chrono::milliseconds environmentWait { 0 };
    std::string rootKey;
    std::function<void(std::chrono::milliseconds)> onSlow;
};

ProjectModel load_project(std::string_view root, const LoadOptions& options);
// usable plan W2.3: a Visual Studio whose toolset has no std module is not used for a workspace
// without a build system, and the status says why as a notice, not an issue: nothing is reduced.
std::optional<ModelIssue> visual_studio_notice(const toolchain::ToolchainFacts& facts);
// A stable directory name for a workspace root.
std::string workspace_key(std::string_view root);

struct ModuleManifest {
    std::string path;
    std::string origin;   // stdlib | module-metadata
};

// The P3286 manifests a model's modules may resolve through: each toolchain's
// standard library, the kit's when the model uses it, and every set's metadata.
std::vector<ModuleManifest> module_manifests(const ProjectModel& model, const spec::Kit* kit);

} // namespace mcppls::project
