// A clangd process started over openkal with the engine database of one context (v1 design 15.1):
// the part of the clangd engine that talks to the executable. The process interface exists so the
// clangd engine can be tested without a real clangd.
export module mcppls.engine.clangd.process;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.log;
import mcppls.lsp.connection;

export namespace mcppls::engine::clangd {

struct ProcessConfig {
    std::string executable;
    std::string version;             // e.g. "23.1.0"
    std::string databaseDirectory;   // what clangd reads (--compile-commands-dir)
    std::string workDirectory;
    std::vector<std::string> extraArguments;
    bool verboseLog { false };
    std::size_t workers { 0 };       // clangd's -j; 0: clangd's own default. Extra arguments naming -j win.
};

class Process {
public:
    using MessageHandler = std::function<void(nlohmann::json)>;
    using ClosedHandler = std::function<void()>;
    using LogHandler = std::function<void(std::string_view)>;

    virtual ~Process() = default;
    // Starts clangd with no database; the engine pushes one before it opens any file.
    // Starting again while running stops the previous run first.
    virtual base::Result<void> start(const ProcessConfig& config, MessageHandler onMessage, ClosedHandler onClosed, LogHandler onLog) = 0;
    virtual base::Result<void> send(const nlohmann::json& message) = 0;
    virtual void stop(std::chrono::milliseconds grace) = 0;
    virtual bool running() const = 0;
    // Reads the CPU time the process has used so far (StuckWatch), where the platform can say; empty
    // where it cannot. Self-contained: it may run on any thread, after this process has been stopped
    // or restarted (a reading of a process that is gone is nullopt). It can take a while (ps(1) on
    // macOS), so it is never run on the event loop.
    virtual std::function<std::optional<double>()> cpu_reader() const { return {}; }
};

std::vector<std::string> clangd_arguments(const ProcessConfig& config);

// clangd's log line for a module it could not build:
// "E[..] Failed to build module greet; due to Failed to compile C:/.../std.ixx. Use '--log=verbose' ..."
struct ModuleFailure {
    std::string module;
    std::string reason;
    std::string failedSource;   // the source that did not compile, when the reason names one
};
std::optional<ModuleFailure> parse_module_failure(std::string_view line);
// clangd's own severity for one of its log lines, by the letter before its timestamp
// ("E[10:31:02.1] ..."): E is a problem worth a person's attention, I/V/D are its everyday chatter,
// and a line with no such prefix (a continuation, or something else entirely) is kept at info.
base::log::Level clangd_log_level(std::string_view line);
// A line the system's program loader wrote because clangd cannot run on this machine at all: a
// shared library, or a version of one, it was linked against is missing (glibc's ld.so, musl's, macOS's
// dyld). No restart can change that, so it is not a crash (0.0.3 plan B1).
bool loader_failure(std::string_view line);
// What a module build failure means for the engine database (robustness design C3). `unresolved`:
// clangd found no unit for the module ("Don't get the module unit"); a provider importing it cannot be
// built. `compile`: the unit was found and did not compile; its importers get errors, not a hang (S3).
enum class FailureKind { unresolved, compile, other };
FailureKind failure_kind(const ModuleFailure& failure);
// "clangd version 23.1.0 (https://github.com/llvm/llvm-project ea7d852a70e8...)" -> "23.1.0"
std::string parse_clangd_version(std::string_view output);

class ClangdProcess : public Process {
private:
    std::unique_ptr<lsp::Connection> connection_;
    ProcessConfig config_;

public:
    base::Result<void> start(const ProcessConfig& config, MessageHandler onMessage, ClosedHandler onClosed, LogHandler onLog) override;
    base::Result<void> send(const nlohmann::json& message) override;
    void stop(std::chrono::milliseconds grace) override;
    bool running() const override;
    std::function<std::optional<double>()> cpu_reader() const override;
};

} // namespace mcppls::engine::clangd
