// What an exit of clangd costs the session, and no more (issue 86, design 2026-11-12): the exit keeps the number the
// system gave it, the file being built when it happened is the suspect, a file that ended the engine stays set aside
// across edits and sessions, and automatic restarts stop while the sandbox still has starts left for the person.
// Logic only: no process, no clock of its own, no file -- the engine gives each what it asks about.
export module mcppls.engine.clangd.exits;

import std;
import nlohmann.json;

export namespace mcppls::engine::clangd {

using Json = nlohmann::json;

// ---- S1: how the engine ended ----------------------------------------------------------------------

enum class ExitHost { posix, windows };

enum class ExitKind {
    returned,   // the program returned a status of its own choosing
    fault,      // the system ended it for something it did: an instruction, an access, a stack, a failed check
    outside,    // it was ended from outside: killed, terminated, interrupted -- nothing it was working on is to blame
};

struct ExitCondition {
    std::uint32_t number { 0 };   // the status; an NTSTATUS on Windows, a signal's number elsewhere, for a fault or an end from outside
    ExitKind kind { ExitKind::returned };
    std::string name;             // "illegal instruction", "SIGSEGV", ...; empty for a returned status
    // "an illegal instruction (0xC000001D)", "SIGILL (signal 4)", "status 3".
    std::string text() const;
    // The number as a report writes it: hexadecimal for an NTSTATUS, decimal otherwise.
    std::string number_text() const;
    bool windows { false };
};

// What mcpplibs.os reports for a program that ended: `code`, and whether the environment ended it. On Windows the
// platform clears the status's top bit when it reports an end by the system (0xC000001D arrives as 0x4000001D), and
// gives 0x7FFFFFFF for a program terminated by its starter.
ExitCondition exit_condition(int code, bool byEnvironment, ExitHost host);

// The runner's report of a fault in the image it runs, a line at a time (okrun where openkal reports faults):
//   openkal: fault: an instruction the program may not execute (SIGILL) at 0x3e8002928f9
//   openkal:   #1 0x1a3db4dafb30 /payload/clangd/openkal/bin/clangd.okb+0x5b3fb2f
// The first gives `what` ("an instruction the program may not execute (SIGILL)"), each of the others a `frame`
// ("clangd.okb+0x5b3fb2f": the image's file name and the offset in it, which is what names a function offline).
struct RunnerFaultLine {
    std::string what;
    std::string frame;
};
std::optional<RunnerFaultLine> runner_fault_line(std::string_view line);

// ---- S2: the builds in flight -----------------------------------------------------------------------

// clangd says when it begins building a file ("ASTWorker building file ..."); the build is over when the file's
// diagnostics come, when its status goes idle, or when it is closed. What is in flight when clangd ends is what it
// was doing.
class BuildsInFlight {
public:
    void began(std::string_view key, std::string_view path);
    void ended(std::string_view key);
    void clear();
    // The files, by path, in the order their builds began.
    std::vector<std::string> files() const;
    bool contains(std::string_view key) const;
    std::size_t size() const { return order_.size(); }

private:
    std::vector<std::pair<std::string, std::string>> order_;   // (key, path)
};

// Who an exit is laid on (design 3.2).
struct ExitSuspects {
    enum class Rule { named, in_flight, guessed, nobody };
    Rule rule { Rule::nobody };
    std::vector<std::string> files;
    // The files are set aside at once; otherwise each gets a strike, and the second sets it aside.
    bool certain { false };
};
// `named`: the file clangd's own crash context names. `inFlight`: the builds in flight at the exit. `guessed`: the
// files of unanswered requests and recent edits.
ExitSuspects exit_suspects(const ExitCondition& condition, std::optional<std::string> named, std::vector<std::string> inFlight,
                           std::vector<std::string> guessed);
std::string_view to_string(ExitSuspects::Rule rule);

// ---- S3: the crash ledger ---------------------------------------------------------------------------

using LedgerClock = std::chrono::system_clock;

struct CrashRecord {
    std::string file;
    std::string engine;       // the engine's sha256: another engine starts clean
    std::string condition;
    int exits { 0 };
    LedgerClock::time_point firstAt;
    LedgerClock::time_point lastAt;
    std::chrono::minutes term { 0 };
    LedgerClock::time_point retryAt;
    std::string content;      // sha256 of the text clangd had
    std::string closure;      // what the modules it imports were built from
};

class CrashLedger {
public:
    // 2, 4, 8, 16 minutes, then an hour, then a day.
    static constexpr std::array<std::chrono::minutes, 6> TERMS { std::chrono::minutes { 2 }, std::chrono::minutes { 4 }, std::chrono::minutes { 8 },
                                                               std::chrono::minutes { 16 }, std::chrono::minutes { 60 }, std::chrono::minutes { 24 * 60 } };
    static constexpr std::chrono::hours KEPT { 24 * 30 };

    enum class Verdict {
        clean,   // nothing is held against the file
        held,    // it stays with mcppls's engine
        retry,   // its term is over and something changed, or it has ended the engine only once: it goes back
    };

    // clangd ended on `key`. The term doubles with each exit.
    const CrashRecord& exited(std::string_view key, std::string_view file, std::string_view engine, std::string_view condition,
                              std::string_view content, std::string_view closure, LedgerClock::time_point now);
    // Whether the file may be given to clangd now. An edit, a rebuilt closure, a new session and a changed command
    // each only make it eligible: it goes back when its term is over. After a second exit an unchanged file (same
    // text, same closure) does not go back when the term ends; it waits for a change or for the person.
    Verdict verdict(std::string_view key, std::string_view engine, std::string_view content, std::string_view closure,
                    LedgerClock::time_point now) const;
    // clangd built the file and went on: nothing is held against it any more.
    bool survived(std::string_view key);
    // The person asked for it to be tried again: its term is over now, and "unchanged" does not hold it.
    bool try_again(std::string_view key);
    void try_all_again();
    bool erase(std::string_view key);
    // Records of another engine, and those older than KEPT, go. True when any went.
    bool prune(std::string_view engine, LedgerClock::time_point now);
    const CrashRecord* find(std::string_view key) const;
    bool empty() const { return records_.empty(); }
    std::size_t size() const { return records_.size(); }
    std::vector<std::string> keys() const;

    Json to_json() const;
    // Reads what to_json wrote; anything else leaves the ledger empty.
    void from_json(const Json& value);

private:
    struct Entry {
        CrashRecord record;
        bool asked { false };   // try_again: the next verdict is `retry` whatever the term and the text
    };
    std::map<std::string, Entry, std::less<>> records_;
};

// ---- S4: the start budget ---------------------------------------------------------------------------

// The sandbox attenuates each read-only grant from a table of fixed size for the life of the server process, and
// (openkal-windows through 0.21.0) returns no slot. The engine counts what its starts spent, and keeps a reserve:
// automatic restarts stop while the person can still start clangd RESERVE times.
class StartBudget {
public:
    static constexpr std::size_t RESERVE { 3 };
    static constexpr std::size_t CAPACITY { 64 };

    // `capacity` zero: this system's sandbox counts nothing, and the budget never runs out.
    explicit StartBudget(std::size_t capacity = 0) : capacity_ { capacity } {}
    void set_capacity(std::size_t capacity) { capacity_ = capacity; }
    // A start was made with `readOnlyGrants` of them.
    void started(std::size_t readOnlyGrants);
    std::size_t starts() const { return starts_; }
    std::size_t spent() const { return spent_; }
    bool counted() const { return capacity_ != 0; }
    // How many more starts fit, each spending `readOnlyGrants`; nullopt where nothing is counted.
    std::optional<std::size_t> left(std::size_t readOnlyGrants) const;
    // An automatic restart may be made: more than the reserve is left.
    bool automatic(std::size_t readOnlyGrants) const;
    // A start asked for by the person may be made: one fits.
    bool by_person(std::size_t readOnlyGrants) const;

private:
    std::size_t capacity_ { 0 };
    std::size_t starts_ { 0 };
    std::size_t spent_ { 0 };
};

} // namespace mcppls::engine::clangd
