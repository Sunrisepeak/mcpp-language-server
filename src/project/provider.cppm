// What every project data source needs from its caller: whether executing
// programs is allowed, and how to run, scan and probe; and (B-1, 2026-09-27
// plan §3.2) the `BuildSystemProvider` interface every build tool's data
// source implements, so `detect_project` and `load_project` ask a registry
// instead of each naming every kind in a switch.
export module mcppls.project.provider;

import std;
import mcppls.base.error;
import mcppls.toolchain.probe;
import mcppls.project.infer;

export namespace mcppls::project {

struct ProviderContext {
    bool trusted { false };
    toolchain::Runner runner;               // bounded by the provider's own timeouts
    Scanner scanner;
    Prober prober;
    std::chrono::milliseconds configureTimeout { std::chrono::minutes { 5 } };
    std::string mcppExecutable;             // empty: found on PATH or in the usual install locations

    // How the build tool is run (design 4.2, 4.4). `offline` is the default because a run the
    // server started by itself must not reach the network; the user turns it off through
    // `mcppls.buildTool`. The soft bound only says "still running"; the hard one ends the unit.
    bool offline { true };
    bool runBuildTool { true };             // false: the user asked that the build tool never be run
    std::chrono::milliseconds producerHard { std::chrono::seconds { 60 } };
    std::chrono::milliseconds producerSoft { std::chrono::seconds { 5 } };
    std::chrono::milliseconds environmentWait { 0 };
    std::string rootKey;                    // the workspace a tool-run record belongs to
    std::function<void(std::chrono::milliseconds)> onSlow;   // the producer passed its soft bound

    // Filled in by the provider: which program described this build, and which version of it. The
    // model cache fingerprints these, so a different mcpp on PATH is a different model.
    mutable std::string producerUsed;
    mutable std::string producerVersionUsed;

    // B-1 plan §3.2: `describe()` is given this same context, plus where its private state
    // (a configure directory it owns, never the project's own build directory) lives -- one per
    // workspace, per provider, under the workspace's cache directory. Read-only steps (`existing`)
    // need no private directory of their own; they read what the project's own build already wrote.
    std::string privateDirectory;
};

// The first executable found on PATH or at one of the fallback absolute paths.
std::optional<std::string> find_tool(std::string_view name, std::span<const std::string> fallbacks);

// design plan §3.2: what a provider's cheap, filesystem-only `detect()` found. `provider` and
// `confidence` are for a caller comparing several claims (the registry itself only needs the first,
// since providers are asked in a fixed, meaningful order -- design §3.6, B-1 task 1); the rest is
// the same shape `Detection` (mcppls.project.detect) already carried, so a claim converts to and
// from one without loss: a manifest, and whatever existing build output `detect()` happened to find
// while it was looking (a build directory, a `compile_commands.json`, an S1 `build_database.json`).
struct Claim {
    std::string provider;
    int confidence { 0 };
    std::string root;
    std::string manifest;
    std::string buildDirectory;
    std::string compileCommands;
    std::string buildDatabase;
    std::vector<std::string> markers;   // human-readable, for logs and the status: which preset, which candidate
};

// What `existing()` or `describe()` came back with. `ok` and `partial` both carry a usable
// database (S2 0.3.0's partial answers are `ok` with issues already inside `InferredDatabase`, so
// the two are not distinguished further here); the rest carry no database and say why, in `code`
// and `reason` -- `code` is the issue code the model reports (`cmake-configure-failed`,
// `spec::NEEDS_DOWNLOAD`, ...), reusing the codes each provider already had before this registry.
enum class Outcome { ok, partial, needs_download, failed, timed_out };

struct Answer {
    Outcome outcome { Outcome::failed };
    std::optional<InferredDatabase> database;
    std::string code;
    std::string reason;
    std::vector<std::string> missing;         // needs_download: what the build tool named (packages, FetchContent names)
    std::string terminalCommand;              // the equivalent command the user can run themselves
};

// Converts an `Answer` back into the `base::Result<InferredDatabase>` shape every provider's own
// free functions (`load_mcpp`, ...) already returned, so `load_project` keeps one `accept()` step
// regardless of which provider answered.
base::Result<InferredDatabase> to_result(Answer answer);

// The inverse: wraps one of those free functions' own `base::Result<InferredDatabase>` as an
// `Answer`, so a provider adapting one (`compile_commands.json`, a producer's own free function)
// needs no boilerplate of its own. A `spec::NEEDS_DOWNLOAD` error code becomes `Outcome::needs_download`;
// anything else, `Outcome::failed`.
Answer from_result(base::Result<InferredDatabase> result);

// design plan §3.2, B-1 task 1. `detect()` is filesystem only and cheap (stat calls, no process, no
// file content read beyond a manifest a caller must read anyway) -- typically under 50 ms.
// `existing()` reads build output the project's own build (or a previous `describe()`) already left
// -- still no process started. `describe()` is the one method allowed to run the build tool, into
// `context.privateDirectory`, offline or not per `context.offline` (never into the workspace: the
// side-effect contract every provider's own module documents and the `workspace-unchanged`
// conformance check covers).
class BuildSystemProvider {
public:
    virtual ~BuildSystemProvider() = default;
    virtual std::string_view id() const = 0;
    virtual std::optional<Claim> detect(std::string_view root) const = 0;
    virtual std::optional<Answer> existing(const Claim& claim, const ProviderContext& context) const = 0;
    virtual Answer describe(const Claim& claim, const ProviderContext& context) const = 0;
    // Glob patterns (relative to the root) or absolute paths whose change should make the model be
    // loaded again -- the same list `load_project` used to hard-code per `SourceKind`.
    virtual std::vector<std::string> watch_inputs(const Claim& claim) const = 0;
};

} // namespace mcppls::project
