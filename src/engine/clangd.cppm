// The clangd engine (overall design 5.4): the core C++ semantics from a pinned clangd, with what
// clangd 23.1 needs from the server kept here rather than in the workspace: the engine database and
// its module hints, parallel module preparation, reading clangd's module cache and its log, request
// deadlines, and restarts.
export module mcppls.engine.clangd;

import std;
import nlohmann.json;
import mcppls.engine;
import mcppls.engine.clangd.process;

export namespace mcppls::engine::clangd {

inline constexpr std::string_view ENGINE_ID { "clangd" };

// The traits table (design 5.4), keyed by clangd version. 23.1.0 is the pinned payload; later 23.1
// releases no longer need aligned allocation turned off (llvm-project#218152, fixed in 23.1.1) but
// have not run through the conformance suite; any other version gets every compensation.
EngineTraits traits_for_version(std::string_view version);

struct Options {
    std::string executable;        // empty, or a file that does not exist: the engine is unavailable
    std::string version;
    bool payloadCorrupt { false };
    bool verboseLog { false };
    std::chrono::milliseconds requestTimeout { std::chrono::seconds { 60 } };
    // StuckWatch: once clangd leaves a request unanswered this long, with nothing answered since, its
    // CPU is watched for `stuckWatch`; still unanswered then, with next to no CPU used, it is stuck and
    // is restarted. Both end within an interactive request's own timeout. Tests shorten them.
    std::chrono::milliseconds stuckAfter { std::chrono::seconds { 3 } };
    std::chrono::milliseconds stuckWatch { std::chrono::seconds { 5 } };
    std::vector<std::string> extraArguments;
    std::function<std::unique_ptr<Process>()> processFactory;   // empty: a real clangd process
};

std::unique_ptr<Engine> make_engine(Options options);

// Requests a person waits for are answered without clangd after this long; the rest wait for the
// configured timeout.
inline constexpr std::chrono::milliseconds INTERACTIVE_TIMEOUT { std::chrono::seconds { 10 } };
inline constexpr std::chrono::milliseconds PREPARING_GRACE { std::chrono::seconds { 5 } };
// The most a person's request waits for clangd in all, from when it arrived: queued while clangd
// starts, held with its file, and kept waiting on preparation together. Past it the request is
// answered unavailable and the next engine answers it, so a request is never left without an
// answer whatever state clangd is in (real-project plan RP1.1: at least L4).
inline constexpr std::chrono::milliseconds INTERACTIVE_LIMIT { std::chrono::seconds { 30 } };
bool is_interactive(std::string_view method);

// When a client request arriving at `arrived` must have been answered: INTERACTIVE_LIMIT for a
// request a person waits for (is_interactive), the configured timeout for the rest.
Clock::time_point wait_limit(std::string_view method, std::chrono::milliseconds requestTimeout, Clock::time_point arrived);

enum class Purpose { client, engine_initialize };

struct PendingRequest {
    Purpose purpose { Purpose::client };
    Json clientId;
    std::string method;
    std::string uri;                  // the client's URI of the request's document
    Clock::time_point deadline;
    int generation { 0 };
    Clock::time_point limit {};       // how long a request may be kept waiting at most (wait_limit); see keep_waiting
    Reply reply;
    Clock::time_point sent {};        // when it was sent to clangd
};

// usable plan W7: a request whose deadline passed while the modules its file imports are still
// being prepared waits on, a little at a time, as long as preparation keeps finishing modules and
// the request's limit allows. `lastProgress` is when preparation last finished a module.
bool keep_waiting(const PendingRequest& request, bool filePreparing, std::optional<Clock::time_point> lastProgress, Clock::time_point now);

} // namespace mcppls::engine::clangd
