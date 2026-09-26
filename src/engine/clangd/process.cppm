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
    // The OS process id, where the platform can say (for the per-thread CPU of an incident, fix plan F17).
    virtual std::optional<std::int64_t> pid() const { return std::nullopt; }
    // How the process ended, once it has: its exit status, -1 when a signal ended it; nullopt while it
    // runs or where nothing can say (fix plan F3). Never blocks.
    virtual std::optional<int> exit_code() { return std::nullopt; }
};

std::vector<std::string> clangd_arguments(const ProcessConfig& config);

// What clangd prints when it crashes (fix plan F3), from its own crash handler, whatever --log says:
//   Signalled during AST worker action: Build AST        (or: Signalled while building preamble)
//     Filename: D:/p/NormalJsonTranslator.Core.cpp
//     Directory: ... / Command Line: ... / Version: 1
//   Exception Code: 0x80000003                            (Windows only)
struct CrashContext {
    std::string action;      // "Build AST", "building preamble", ...
    std::string file;        // the file clangd was working on
    std::string exception;   // Windows' exception code; empty elsewhere
};

// "E[..] Scanning modules dependencies for <file> failed: <first line>", continued by lines without a
// severity and ended by "E[..] The command line the scanning tool use is: ..." (fix plan F6). A failed
// scan is how a command clangd rejects shows (#23: `LTO requires -fuse-ld=lld`): no module is built.
struct ScanFailure {
    std::string file;
    std::string reason;   // the first line that says `error:`, from there on, else the header's own text
    bool driver { false };   // the error is the compiler driver's ("clang++: error: ..."), about the command, not a source
};

// Reads clangd's standard error line by line, keeping what spans several lines (fix plan F3, F6).
// One per clangd process; not thread-safe, like the stream it reads.
class LogReader {
public:
    struct Read {
        std::optional<CrashContext> crash;        // a crash context, as soon as its file is known, and again with the exception code
        std::optional<ScanFailure> scanFailure;   // a finished scan failure
        bool important { false };                 // a line the log never leaves out, whatever the limiter says
    };
    Read read(std::string_view line);
    // The stream ended: a scan failure whose closing line never came.
    std::optional<ScanFailure> finish();

private:
    std::optional<CrashContext> crash_;
    bool inCrash_ { false };
    std::optional<ScanFailure> scan_;
    bool scanHasError_ { false };
};

// The latest lines clangd wrote (fix plan F17.1), kept in memory only, and written out with an incident.
// Thread-safe: clangd's standard error is read on a thread of its own.
class LogRing {
public:
    LogRing(std::size_t maxLines, std::size_t maxBytes) : maxLines_ { maxLines }, maxBytes_ { maxBytes } {}
    void add(std::string_view line);
    std::string text() const;
    std::size_t size() const;

private:
    std::size_t maxLines_;
    std::size_t maxBytes_;
    mutable std::mutex mutex_;
    std::deque<std::string> lines_;
    std::size_t bytes_ { 0 };
    std::size_t dropped_ { 0 };
};

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
    std::optional<std::int64_t> pid() const override;
    std::optional<int> exit_code() override;
};

} // namespace mcppls::engine::clangd
