// What keeps one fault of clangd from becoming the whole workspace's (robustness design C4, C6, C7):
// restarts spaced out, files clangd stopped answering for set aside one by one, its log forwarded
// without flooding, and module preparation within a share of the machine.
export module mcppls.engine.clangd.guard;

import std;

export namespace mcppls::engine::clangd {

using GuardClock = std::chrono::steady_clock;

// Restarts in a row are spaced out: the first at once, then at least FIRST_GAP after the previous
// one, doubling while restarts keep coming within WINDOW, never more than MAX_GAP apart.
class RestartGate {
public:
    static constexpr std::chrono::seconds FIRST_GAP { 10 };
    static constexpr std::chrono::minutes MAX_GAP { 5 };
    static constexpr std::chrono::minutes WINDOW { 10 };
    // A stuck file whose closure contains a module that failed to compile is never a reason to
    // restart (P1: a fault only affects where it is); every other reason is still capped, so a
    // machine that keeps finding new reasons to restart does not keep doing it forever either.
    static constexpr std::size_t MAX_RESTARTS_PER_WINDOW { 3 };

    // When the next restart may happen.
    GuardClock::time_point earliest(GuardClock::time_point now) const;
    void record(GuardClock::time_point now);
    std::size_t recent(GuardClock::time_point now) const;
    // WINDOW already has MAX_RESTARTS_PER_WINDOW restarts in it: no more until it ages out. The
    // caller stays down to its own engine for whatever a restart would have tried to fix.
    bool at_cap(GuardClock::time_point now) const;

private:
    std::deque<GuardClock::time_point> restarts_;
};

// Files whose requests clangd stopped answering. clangd 23.1 can stop answering for one file while it
// answers others (experiments S2, S12); such a file is set aside for a while, answered by mcppls's
// own engine, and handed back when it changes or its time is up. When clangd answers nobody, that is
// the engine stalling, not a file: the caller restarts it.
class Quarantine {
public:
    enum class Verdict { wait, quarantined, stalled };
    static constexpr int TIMEOUTS_BEFORE_QUARANTINE { 2 };
    static constexpr std::chrono::minutes FIRST_TERM { 2 };
    static constexpr std::chrono::minutes MAX_TERM { 16 };
    static constexpr std::chrono::seconds STALL_WINDOW { 60 };

    // A request about `uri`, sent at `sent`, timed out at `now`; `lastAnswer` is when clangd last
    // answered any request. A file set aside already only waits. `rebuilding`: the file, or a module
    // it imports, just changed, so clangd is busy with that change rather than stuck on the file --
    // the timeout still counts toward clangd answering nobody, never toward setting the file aside.
    Verdict timed_out(std::string_view uri, GuardClock::time_point sent, GuardClock::time_point now,
                      std::optional<GuardClock::time_point> lastAnswer, bool rebuilding = false);
    // clangd answered a request about `uri`: its timeouts start over.
    void answered(std::string_view uri);
    bool contains(std::string_view uri) const;
    // Set aside now, for a term that doubles each time the same file comes back.
    void put(std::string_view uri, GuardClock::time_point now);
    // The file changed, or the engine restarted: it goes back to clangd.
    bool release(std::string_view uri);
    void release_all();
    // Files whose term is over, released.
    std::vector<std::string> due(GuardClock::time_point now);
    // Of the files that timed out while clangd answered nobody, the one asked about first.
    std::optional<std::string> first_stalled() const;
    std::size_t size() const;
    // The files set aside now.
    std::vector<std::string> members() const;

private:
    struct Entry {
        int timeouts { 0 };
        int terms { 0 };
        std::optional<GuardClock::time_point> until;
    };
    std::map<std::string, Entry, std::less<>> entries_;
    std::deque<std::tuple<GuardClock::time_point, GuardClock::time_point, std::string>> unanswered_;   // (timed out, sent, uri)
};

// clangd answering nothing while it uses next to no CPU is stuck, not slow: whatever it waits for
// is not coming. Seen on slow CI runners after a module's source changed twice within a second: its
// build never finished, clangd answered no request for minutes, and used 2 s of CPU in 105 s. A long
// compile, the case that must not be restarted (the standard library on a slow machine), keeps a
// core busy the whole time, which is what tells the two apart. Quarantine's "answers nobody" verdict
// needs two files timing out within a minute; an editor asking one thing at a time never shows it.
class StuckWatch {
public:
    static constexpr double IDLE_SHARE { 0.05 };   // of one core: using less than this, clangd is idle

    explicit StuckWatch(std::chrono::milliseconds window) : window_ { window } {}

    // A request went unanswered: watch from this CPU reading. Without a reading (the platform cannot
    // say) there is nothing to go on, and nothing is watched.
    void suspect(GuardClock::time_point now, std::optional<double> cpuSeconds);
    // clangd answered something, or is a new process: nothing to watch.
    void clear();
    bool watching() const;
    // When the watch began.
    std::optional<GuardClock::time_point> started() const;
    // When the watch ends and check() is due.
    std::optional<GuardClock::time_point> due() const;

    struct Verdict {
        bool stuck { false };
        double seconds { 0 };      // how long it was watched
        double cpuSeconds { 0 };   // the CPU it used meanwhile
    };
    // At due(), with the CPU reading then. The watch ends either way; a busy clangd is watched
    // again from the next request it leaves unanswered.
    Verdict check(GuardClock::time_point now, std::optional<double> cpuSeconds);

private:
    std::chrono::milliseconds window_;
    std::optional<std::pair<GuardClock::time_point, double>> since_;   // when the watch began, and the CPU reading then
};

// Up to `burst` lines in each window; the first line of the next window carries how many were left out.
class LineLimiter {
public:
    LineLimiter(std::size_t burst, std::chrono::milliseconds window) : burst_ { burst }, window_ { window } {}
    struct Decision {
        bool forward { false };
        std::size_t suppressedBefore { 0 };   // lines left out in the windows before this line
    };
    Decision admit(GuardClock::time_point now);

private:
    std::size_t burst_;
    std::chrono::milliseconds window_;
    std::optional<GuardClock::time_point> windowStart_;
    std::size_t inWindow_ { 0 };
    std::size_t suppressed_ { 0 };
};

// The modules doomed along with `failed`: it, and every module that imports it, directly or
// transitively, by `imports` (module -> the modules it imports). A module compiling again is never
// discovered by this function; the caller forgets a root when its provider's source or command
// changes and recomputes (real-project plan RP1.1).
std::set<std::string> doomed_modules(const std::map<std::string, std::vector<std::string>, std::less<>>& imports, std::string_view failed);

// Whether clangd's state for a file (textDocument/clangd.fileStatus: "parsing includes", "parsing main file", "running Hover",
// "file is queued", "preamble (queued)" or "idle", several joined by ", ") says it is working on the file, rather than idle or
// waiting for a worker.
bool engine_working(std::string_view state);

// clangd's workers (-j), which also bound its background index: a quarter of the physical cores (hardware
// threads count as two per core except on macOS), at least two. On a 16-core machine xlings' first open
// took 21 cores at its busiest second with clangd's default of one worker per core, 8 with four, and its
// first hover came in 7.5 s instead of 13.5 s (robustness design C7).
std::size_t engine_workers(std::size_t hardwareThreads, bool macos);
// How many modules are prepared at once: half of clangd's workers, which prime units occupy.
//
// A file someone is waiting for used to halve that again, to leave workers for the request. That is
// right only when the waiting file can make progress without preparation — and for a modules
// translation unit it usually cannot, because it is blocked on exactly these BMIs. Throttling then
// slows the one piece of work that would unblock it, and the workers freed sit idle because nothing
// else can run: measured as one busy core on a 32-thread machine during a cold start (2026-09-17).
//
// So `waitingOnPreparation` says the waiting files import a module that is not ready yet. When it is
// true the throttle does not apply; when it is false (the file waits on something else, e.g. a large
// TU whose imports are all built) it does, which is the case C7 measured.
std::size_t preparation_limit(std::size_t hardwareThreads, bool macos, std::size_t waitingFiles,
                              bool waitingOnPreparation = false);

} // namespace mcppls::engine::clangd
