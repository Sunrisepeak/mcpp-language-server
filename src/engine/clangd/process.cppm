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
    std::string fallbackStyle;
    std::vector<std::string> extraArguments;
    bool verboseLog { false };
    std::size_t workers { 0 };       // clangd's -j; 0: clangd's own default. Extra arguments naming -j win.
    // --experimental-modules-support. WA-CLANGD-009: off for a project that uses no modules, where it only costs every
    // request a scan of the file's module dependencies.
    bool modulesSupport { true };
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

std::string format_fallback_style(std::string_view requested, bool supported, bool mcppProject);

std::vector<std::string> clangd_arguments(const ProcessConfig& config);

// What clangd prints when it crashes (fix plan F3), from its own crash handler, whatever --log says:
//   Signalled during AST worker action: Build AST        (or: Signalled while building preamble)
//     Filename: D:/p/NormalJsonTranslator.Core.cpp
//     Directory: ... / Command Line: ... / Version: 1
//   Exception Code: 0x80000003                            (Windows only)
// and LLVM's own crash report after it (K-3, plan 0.0.8), which is what an upstream report needs:
//   Stack dump:
//   0.	Program arguments: clangd ...
//    #0 0x00007ff6a1b2c3d4 (C:\...\clangd.exe+0x1b2c3d4)
struct CrashContext {
    std::string action;      // "Build AST", "building preamble", ...; empty when clangd crashed outside those
    std::string file;        // the file clangd was working on
    std::string exception;   // Windows' exception code; empty elsewhere
    std::vector<std::string> stack;   // the stack dump's lines (numbered entries and frames), at most MAX_STACK_LINES
    static constexpr std::size_t MAX_STACK_LINES { 64 };
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
    bool inStack_ { false };   // inside LLVM's stack dump (K-3)
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

// C-4 (plan 0.0.8 part 2): what clangd's log says building each file cost -- its preambles and the modules it imports
// ("Built preamble of size .. for file F version V in S seconds", "Built prerequisite modules for file F in S seconds"),
// and how many times its AST was built ("ASTWorker building file F version V ..."). The files are as clangd names them.
struct FileBuildTimes {
    std::size_t preambles { 0 };
    double preambleSeconds { 0 };
    double preambleMaxSeconds { 0 };
    std::size_t moduleBuilds { 0 };
    double moduleSeconds { 0 };
    double moduleMaxSeconds { 0 };
    std::size_t asts { 0 };
};
std::map<std::string, FileBuildTimes, std::less<>> build_times(std::string_view log);
// The same, kept as clangd writes its log: `add` takes each line on the thread reading clangd's standard error, and
// `times` is what a report reads -- parsing the whole log for a report held the event loop for seconds.
class BuildTimesLog {
public:
    void add(std::string_view line);
    std::map<std::string, FileBuildTimes, std::less<>> times() const;

private:
    static constexpr std::size_t MAX_FILES { 2000 };
    mutable std::mutex mutex_;
    std::map<std::string, FileBuildTimes, std::less<>> times_;
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
// clangd's line for a module build waiting on another process's lock (clangd 23.1's persistent module
// cache, `<cache>/modules/.locks/<hash>.lock`): "I[..] Still waiting for module lock <path> after 10s". The
// path of the lock, when the line is one of these (C-4, plan 2026-09-30).
std::optional<std::string> parse_module_lock_wait(std::string_view line);
// The directory of clangd's module locks for an engine database directory.
std::string module_lock_directory(std::string_view databaseDirectory);
// Removes every module lock of an engine database directory; how many were removed. Only when no clangd
// is using that cache: a lock left by a clangd that was killed while building a module is never released,
// and on Windows the next clangd waits for it forever (the owner's death is not detected there).
std::size_t clear_module_locks(std::string_view databaseDirectory);
// The process id a lock records ("<host> <pid>" in the file the lock names), when it can be read.
std::optional<std::int64_t> module_lock_owner(std::string_view lockPath);
// C-2 (plan 2026-09-30): clangd 23.1 keeps a unit's BMIs in `<modules>/<unit>-<hash>/<command hash>/`, a directory for
// every command it was ever built with, and removes none. The directories of each unit beyond the `keep` most recently
// written: what commands the unit no longer has left behind (clangd builds again whatever it needs).
std::vector<std::string> stale_module_builds(std::string_view databaseDirectory, std::size_t keep);

// What a module build failure means for the engine database (robustness design C3). `unresolved`:
// clangd found no unit for the module ("Don't get the module unit"); a provider importing it cannot be
// built. `compile`: the unit was found and did not compile; its importers get errors, not a hang (S3).
enum class FailureKind { unresolved, compile, other };
FailureKind failure_kind(const ModuleFailure& failure);

// What a module failure clangd reported means for the session (plan 2026-09-27 Q1-1, Q1-4).
//   ignore      the unit joined an engine database this clangd has not read yet: the report is about the old one
//   use_kit     the standard library's own unit failed to compile, or the plan has no unit for it: the kit replaces it
//   record      everything else: the failure is recorded where it is, as before
enum class FailureAction { ignore, use_kit, record };
struct FailureContext {
    bool standardLibrary { false };   // the module is std or std.compat, or the unit that failed is std's
    bool providerPlanned { false };   // the plan gives the module a unit of the project or the toolchain (not a stand-in)
    bool providerRead { true };       // this clangd has read the engine database in which that unit joined
    bool alreadyOnKit { false };      // the standard library is already the kit's
};
FailureAction failure_action(FailureKind kind, const FailureContext& context);
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
