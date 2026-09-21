// What every project data source needs from its caller: whether executing
// programs is allowed, and how to run, scan and probe.
export module mcppls.project.provider;

import std;
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
};

// The first executable found on PATH or at one of the fallback absolute paths.
std::optional<std::string> find_tool(std::string_view name, std::span<const std::string> fallbacks);

} // namespace mcppls::project
