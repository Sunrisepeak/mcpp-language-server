module mcppls.orchestrator.workspace;

import std;
import nlohmann.json;
import mcppls.os;
import mcppls.base.error;
import mcppls.base.glob;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.base.uri;
import mcppls.base.version;
import mcppls.orchestrator.cache;
import mcppls.engine.clangd.process;
import mcppls.platform.dirs;
import mcppls.platform.fs;
import mcppls.platform.process;
import mcppls.platform.toolenv;
import mcppls.platform.toolrun;
import mcppls.platform.task;
import mcppls.lsp.jsonrpc;
import mcppls.lsp.protocol;
import mcppls.spec.database;
import mcppls.spec.discovery;
import mcppls.spec.kit;
import mcppls.spec.metadata;
import mcppls.toolchain.probe;
import mcppls.project.scan;
import mcppls.project.detect;
import mcppls.project.providers;
import mcppls.project.provider;
import mcppls.project.model;
import mcppls.project.modelcache;
import mcppls.normalize.plan;
import mcppls.engine;
import mcppls.engine.payload;
import mcppls.engine.native.index;
import mcppls.engine.native.keywords;
import mcppls.orchestrator.completion;
import mcppls.orchestrator.incidents;
import mcppls.orchestrator.journal;
import mcppls.orchestrator.client;
import mcppls.orchestrator.documents;
import mcppls.orchestrator.instance;
import mcppls.orchestrator.routing;
import mcppls.orchestrator.tokens;

namespace mcppls::orchestrator {

namespace log = base::log;

std::string_view to_string(State state) {
    switch (state) {
    case State::starting: return "starting";
    case State::loading: return "loading";
    case State::preparing: return "preparing";
    case State::ready: return "ready";
    case State::degraded: return "degraded";
    case State::error: return "error";
    }
    return "error";
}

namespace {

constexpr std::array<std::string_view, 10> BUILD_FILES { "mcpp.toml", "mcpp.lock", "CMakeLists.txt", "CMakePresets.json", "CMakeUserPresets.json",
                                                         "xmake.lua", "meson.build", "meson_options.txt", "compile_commands.json", "build_database.json" };

constexpr std::array<std::string_view, 7> WATCH_POLL_SKIP_DIRECTORIES { "target", "build", "node_modules", "out",
                                                                        "_build", "cmake-build-debug", "cmake-build-release" };

// Changes to cxxModules/status that keep its state are sent at most this often (S3 4).
constexpr std::chrono::milliseconds STATUS_COALESCE { 250 };
// import-hang plan §6: a change from a working state to degraded goes out only once it has lasted this
// long, so a condition that passes by itself (a file set aside and handed back as the user types) never
// reaches the editor. error goes out at once.
constexpr std::chrono::milliseconds DEGRADED_HOLD { 3000 };
// K-6 (plan 2026-09-30): what comes and goes by itself as the person types -- a file set aside and handed back, a
// restart held back by its budget -- makes the state degraded only once it has lasted this long. A crash, a clangd
// that stopped answering and every problem of the project or its environment go out after DEGRADED_HOLD.
constexpr std::chrono::milliseconds PASSING_DEGRADED_HOLD { 30000 };
// K-6 (plan 0.0.8): once the workspace has settled, modules rebuilt because of an edit (a save that most of the project
// imports, a module being written) make it preparing again only when that lasts this long. U15 counted the edits
// stage flipping ready -> preparing -> ready up to nine times a minute on mcpp and xlings, for rebuilds of seconds; a
// person saw the status flicker at every save. The way back to ready is never held.
constexpr std::chrono::milliseconds SETTLED_PREPARING_HOLD { 30000 };
constexpr std::array<std::string_view, 2> PASSING_ISSUES { "file-quarantined", "engine-restart-capped" };

// The module structure of a scan, for deciding whether an edit changes the engine database.
std::string structure_of(const project::ScanResult& scan) {
    std::string key { project::provided_name(scan) };
    key += "|" + std::string { spec::to_string(project::role_of(scan)) };
    for (const auto& name : project::required_names(scan)) key += "|" + name;
    return key;
}

// The inputs a producer named in its database's watch list (S2 5), shared with the polling worker.
struct WatchPatterns {
    std::mutex mutex;
    std::vector<std::string> entries;   // glob patterns relative to the root, or absolute paths
    int generation { 0 };               // advanced whenever `entries` is replaced
};

// Whether `path` is one of `entries`: an absolute entry names the file, a relative one is a glob under `root`.
bool matches_watch_entries(std::span<const std::string> entries, std::string_view root, std::string_view path) {
    const auto relative = base::relative_path(path, root);
    return std::ranges::any_of(entries, [&](const std::string& entry) {
        if (base::is_absolute_path(entry)) return base::same_path(base::normalize_path(entry), path);
        return relative && base::glob_match(entry, *relative);
    });
}

// Whether a watch covers `path`: the build descriptions and sources every root watches, and the model's own entries.
bool watch_covers(std::span<const std::string> entries, std::string_view root, std::string_view path) {
    return is_build_file(base::file_name(path)) || project::is_cxx_source_name(path) || matches_watch_entries(entries, root, path);
}

std::map<std::string, platform::fs::FileStamp> watched_files_snapshot_of(std::string_view root, std::span<const std::string> entries) {
    std::map<std::string, platform::fs::FileStamp> files;
    for (const auto& file : platform::fs::list_files(root, {}, WATCH_POLL_SKIP_DIRECTORIES)) {
        if (!watch_covers(entries, root, file)) continue;
        if (auto fileStamp = platform::fs::stamp(file)) files.emplace(file, *fileStamp);
    }
    // An absolute entry outside the root is watched too; the database of S2 stream mode is one.
    for (const auto& entry : entries) {
        if (!base::is_absolute_path(entry) || base::is_within(entry, root)) continue;
        if (auto fileStamp = platform::fs::stamp(entry)) files.emplace(base::normalize_path(entry), *fileStamp);
    }
    return files;
}

std::string uri_of_params(const Json& params) {
    const Json* uri { lsp::find_path(params, { "textDocument", "uri" }) };
    return uri != nullptr && uri->is_string() ? uri->get<std::string>() : std::string {};
}

// G-4 (plan 2026-09-30, revising BD5's one minute): the offline producer's hard deadline. A constant cannot fit
// every project: mcpp describes GalTranslPP in 53-79 s on a 4-core runner, the minute killed it, and a project
// the producer never finishes describing never gets its model -- nor a cache, so every session started over. So
// three times what the producer took last time, within a minute and ten; with no history, five minutes (the
// scanned-sources model serves meanwhile, so the person does not wait for it).
std::chrono::milliseconds producer_deadline(std::int64_t lastProducerMs) {
    using namespace std::chrono_literals;
    if (lastProducerMs <= 0) return std::chrono::milliseconds { 5min };
    return std::clamp<std::chrono::milliseconds>(std::chrono::milliseconds { lastProducerMs * 3 }, 60s, std::chrono::milliseconds { 10min });
}

} // namespace

namespace {

// What the button of a status issue says (fix plan F17.5): what its command does, where the server knows it.
std::string_view command_title(std::string_view command) {
    if (command == "mcppls.showLogs") return "Show Logs";
    if (command == "mcppls.restartServer") return "Restart Server";
    if (command == "mcppls.restartClangd") return "Restart clangd";
    if (command == "mcppls.exportDiagnosticBundle") return "Export Diagnostic Bundle";
    return "Fix";
}

} // namespace

bool is_build_file(std::string_view name) {
    return std::ranges::find(BUILD_FILES, name) != BUILD_FILES.end() || name.ends_with(".cmake");
}

std::map<std::string, platform::fs::FileStamp> watched_files_snapshot(std::string_view root) { return watched_files_snapshot_of(root, {}); }

// ---- Workspace::Impl -------------------------------------------------------------------------

struct Workspace::Impl final : engine::Host {
    std::string root;
    std::string key;
    SessionOptions options;
    std::shared_ptr<EventChannel> events;
    ClientSink& client;
    std::string compilerOverride;
    bool kitEnabled { true };

    std::string cacheDirectory;
    // overall design 6.3: the lease on <cache>/workspaces/<key>; a second instance works in a private directory.
    std::optional<WorkspaceLease> lease;
    // ---- the cache mcppls owns (0.0.10 plan C-7, C-8, C-9, C-13.1) ----------------------
    std::string workspaceDirectory_;              // <cache>/workspaces/<key>, the owner's own root
    std::string ownToken_;                        // this instance's token, marking its report entry
    mutable std::mutex cacheMutex_;               // guards everything below
    Json cacheSnapshot_;                          // the classified numbers the status' `cache` shows
    Json cacheReport_;                            // the 30 s cache behind `cxxModules/cache`
    std::optional<Clock::time_point> cacheReportAt_;
    std::optional<std::int64_t> lastSweepAt_;
    std::uint64_t lastSweepFreed_ { 0 };
    std::size_t lastSweepFiles_ { 0 };
    std::size_t lastSweepFailed_ { 0 };
    std::atomic<bool> sweepRunning_ { false };    // one sweep at a time, in this process
    std::optional<Clock::time_point> leaseRenewAt;
    engine::PayloadPaths payload;
    bool payloadCorrupt { false };
    std::optional<spec::Kit> kit;
    std::string macosSdk;
    DocumentStore documents_;
    mutable std::unordered_map<std::string, std::string> canonicalByUri;
    index::ModuleIndex index;
    spec::MetadataReader metadataReader { spec::caching_metadata_reader() };
    std::map<std::string, platform::fs::FileStamp> watchBaseline;
    std::shared_ptr<WatchPatterns> watchPatterns { std::make_shared<WatchPatterns>() };
    bool dynamicWatch { false };             // the client registers watchers for us (else this root polls)
    int watchRegistration { 0 };             // the id suffix of the model's current watcher registration, 0 for none
    // S2 5: a model whose producer failed to answer again is not replaced by the fallback; this says why.
    std::string staleModelReason;

    Json clientParams;
    bool conflictAdviceSent { false };    // the one-engine-per-file note goes out at most once
    std::string progressToken;           // non-empty while a $/progress is open
    std::string lastProgressMessage;
    std::string clientUri;                   // this folder's URI as the client named it (set_client_uri)
    bool clientSupportsStatus { false };
    // Sending anything before the client has even received its own `initialize` response would be a
    // protocol violation; status can otherwise be computed synchronously from inside start().
    bool initializeAnswered { false };
    std::function<void(Json)> onEngineSettled;

    // Engines: mcppls's own and the core engine, in the order they are asked.
    std::vector<std::unique_ptr<engine::Engine>> engines;
    engine::Engine* coreEngine { nullptr };
    engine::Engine* moduleEngine { nullptr };
    Json coreCapabilities = Json::object();
    bool settledReported { false };
    // Semantic tokens (design doc 2026-09-25 K/§7): rebuilt whenever the core engine settles (even
    // with none), so a request's core-engine tokens are always remapped by the same legend
    // `merge_capabilities` advertised for this same `coreCapabilities`.
    tokens::Legend tokensLegend { tokens::build_legend(Json::object()) };
    bool clientSupportsTokensRefresh { false };
    std::optional<Clock::time_point> tokensRefreshAt;   // coalesced: at most one refresh per ~500ms
    std::uint64_t tokensRefreshes { 0 };                // for the refresh requests' ids
    // Diagnostics published by engines other than mcppls's own, per engine and client URI.
    std::map<std::string, std::map<std::string, Json, std::less<>>, std::less<>> engineDiagnostics;
    std::map<std::string, std::string, std::less<>> publishedDiagnostics;
    // The document version the core engine's latest diagnostics were computed for, by client URI (-1: not said).
    std::map<std::string, std::int64_t, std::less<>> coreDiagnosticsVersions;
    // The review an editor asked for (overall design 7.7): its findings as LSP diagnostics, by client URI.
    std::map<std::string, Json, std::less<>> reviewDiagnostics;
    std::shared_ptr<platform::Process> reviewProcess;

    // robustness design O1, O3: what happened, and how requests fared, for a report of a problem.
    Journal journal;
    struct MethodStats {
        std::size_t count { 0 };
        std::size_t empty { 0 };
        std::size_t errors { 0 };
        std::size_t cancelled { 0 };
        double maxMs { 0 };
        std::deque<double> recentMs;   // the latest durations, for percentiles
        // M-1 (plan 0.0.9): the same requests' time in the engine that answered, and the time outside it, for the ones an
        // engine answered. Pairs, so a request's two parts stay together when the oldest leave.
        std::deque<std::pair<double, double>> recentEngineMs;   // {engine, overhead}
        std::map<std::string, std::size_t, std::less<>> answeredBy;
        std::string lastAt;            // UTC, when the latest one was answered (0.0.8 plan E-3)
    };
    std::map<std::string, MethodStats, std::less<>> requestStats;
    // C-4 (plan 0.0.8 part 2): the same by file, so a report says which files are slow and for what: GalTranslPP's showed
    // slow requests only as a journal of the worst, and "some files complete fast, some slowly" had to be pieced together.
    struct FileStats {
        std::size_t count { 0 };
        std::size_t completionsWithoutCore { 0 };   // answered with the file's words: the core engine was late
        double maxMs { 0 };
        std::map<std::string, std::deque<double>, std::less<>> recentMs;   // by method
    };
    std::map<std::string, FileStats, std::less<>> fileRequestStats;
    static constexpr std::size_t FILE_STATS_LIMIT { 500 };   // files; a session that opens more keeps the first ones
    // F9 (D4): what space-triggered completion costs. Most never pass the gate and cost one look at a line.
    struct SpaceTriggerStats {
        std::size_t count { 0 };
        std::size_t passed { 0 };
        std::int64_t maxMicros { 0 };
        std::int64_t totalMicros { 0 };
    };
    SpaceTriggerStats spaceTrigger;
    std::size_t keywordsWithoutEngine { 0 };   // F15: keyword completions answered before the core engine did
    std::size_t wordsWithoutCore { 0 };        // M-2 (plan 0.0.8): completions the core engine did not answer, given the file's words
    // C-2 (plan 0.0.8 part 2): completions the core engine answered after their budget, and requests given such an answer.
    std::size_t lateCompletionsArrived { 0 };
    std::size_t lateCompletionsServed { 0 };
    // E-3 (plan 0.0.8): how much the person edited, so a report shows edits going on while a request stopped coming.
    std::size_t documentChanges { 0 };
    std::string lastDocumentChangeAt;
    bool vscodeLike { false };                 // the client runs VS Code's commands (completion::vscode_like)

    // Requests in flight across engines.
    struct Job {
        Clock::time_point started {};
        std::string answeredBy;   // the engine whose answer went out; "merged" when several were
        Json clientId;
        std::string method;
        std::vector<engine::Engine*> answerers;
        std::size_t next { 0 };
        std::size_t awaiting { 0 };
        std::vector<std::pair<std::string, Json>> merged;
        std::optional<Json> error;
        bool cancelled { false };
        bool merging { false };
        engine::RequestView view;
        Json params;
        std::string path;
        std::string text;
        Json message;
        // F15: a completion's module-syntax keywords, merged into whatever the engines answer.
        Json keywords;
        // R-7 (plan 2026-09-30): when mcppls answers without the core engine (routing::answer_budget).
        std::optional<Clock::time_point> budgetAt;
        // M-2 (plan 0.0.8): the core engine gave an answer, even an empty one; a completion it did not answer
        // (not asked, unavailable, failed) gets the file's words in finish_job.
        bool coreAnswered { false };
        // C-2 (plan 0.0.8 part 2): the job asked nothing of its own and waits for a late completion of the same word.
        bool waitsForLate { false };
        // M-1 (plan 0.0.9): when the request went to the engine whose answer went out, and when that engine replied.
        // The difference is the engine's share of the request's time; the rest is mcppls's own.
        std::optional<Clock::time_point> engineSentAt;
        std::optional<Clock::time_point> engineRepliedAt;
    };
    std::map<std::uint64_t, Job> jobs;
    std::uint64_t nextJob { 1 };

    // C-2 (plan 0.0.8 part 2): a completion the core engine did not answer in its budget keeps its request running, for
    // as long as the person types in the same word. Cancelled at the budget, as before, clangd's answer never reached
    // anyone: the next keystroke asked again, queued behind the same rebuild, and missed its budget too -- in a file clangd
    // rebuilds for seconds after each edit, no completion of clangd's ever showed (GalTranslPP: 25 of 48 answered with the
    // file's words). Now the requests that word makes meanwhile wait for that one answer, and the next one is given it.
    static constexpr std::chrono::seconds LATE_COMPLETION_KEEP { 10 };
    struct LateCompletion {
        std::string uri;
        completion::WordKey key;
        Json clientId;                          // the request the core engine is still working on
        Clock::time_point until;
        std::vector<std::uint64_t> waiting;     // jobs of the same word waiting for its answer
    };
    std::map<std::uint64_t, LateCompletion> lateCompletions;   // by the job that asked
    struct LateAnswer {
        completion::WordKey key;
        Json result;
        Clock::time_point at;
    };
    // A late answer is for the word as it was a moment ago: the file around it may have changed since (a member added
    // elsewhere), so it serves the next requests of that word and not a return to it minutes later.
    static constexpr std::chrono::seconds LATE_ANSWER_KEPT { 5 };
    std::map<std::string, LateAnswer, std::less<>> lateAnswers;   // document URI -> the last late answer, for its word

    // Project model and plan.
    std::shared_ptr<project::ProjectModel> model;
    int modelGeneration { 0 };
    std::string modelDescription;            // describe_model of `model`, to recognize an unchanged reload
    bool loading { false };
    bool reloadAfterLoad { false };
    normalize::EnginePlan plan;
    bool firstPlanWritten { false };
    std::string contextSet;
    std::map<std::string, std::string, std::less<>> structures;   // path key -> module structure at planning time
    // robustness design C2: C++ sources of the workspace that the editor opened and no set describes. They join the engine
    // database with the nearest unit's arguments and stay for the session, so opening one again leaves the database as it is.
    std::set<std::string> openedOutsideModel;
    std::set<std::string> plannedFiles;      // path keys of the plan's units and of the files it left out
    std::vector<std::string> loggedStandards;   // the standards the last log line about raising them named
    bool loggedStandardAssumed { false };        // X-3 (plan 0.0.8 part 2): said once that C++23 was chosen for the module units
    // S5 2.2: advanced whenever a document, a watched file, the model or the plan changes.
    std::uint64_t snapshotGeneration { 0 };

    // Timers.
    std::optional<Clock::time_point> reloadAt;
    // reloadAt may be moved by edits (schedule_reload(true)): it was set by edits, or is a retry minutes away. A reload
    // a build description asked for is not moved.
    bool reloadMovable { false };
    std::optional<Clock::time_point> replanAt;
    // import-hang plan §5: when each open file (path key) was last changed in the editor. An import that nothing
    // provides in a file changed within EDITING_WINDOW is most likely still being typed.
    std::map<std::string, Clock::time_point, std::less<>> editedAt;
    static constexpr std::chrono::seconds EDITING_WINDOW { 5 };
    // Fix plan F13: a change to a file's imports or module declaration made in the editor is planned once the file
    // has been quiet this long, so a name being typed is not planned letter by letter.
    static constexpr std::chrono::milliseconds EDIT_SETTLE { 2000 };
    std::optional<Clock::time_point> loadGiveUpAt;       // the producer has not answered: take what there is (design 4.1)
    std::optional<Clock::time_point> lastResortAt;       // nothing at all came: serve without a database rather than nothing
    std::optional<Clock::time_point> sdkCheckAt;

    // Model sources (design 4.1). The cache is per source and carries a fingerprint of everything
    // the producer read, so a session can start from it and confirm it in the background.
    project::SourceKind detectedSource { project::SourceKind::inferred };
    std::string detectedManifest;
    std::optional<project::CachedModel> staleCache;      // a cache whose fingerprint no longer matches
    std::string modelOrigin;                             // cache-fresh | cache-stale | cache-confirmed | producer | inferred
    std::string firstModelOrigin;                        // where the model this session started with came from
    std::string producerPath;                            // for the fingerprint of the next save
    std::string producerVersion;
    std::string needsDownload;                           // the producer, run offline, cannot go on without a download
    // D-2 (plan 0.0.9): that load was offline (an online one that fails says producer-install-failed instead), so a client may
    // offer to fetch what is missing; and what the load that failed with the network allowed said, named in the status.
    bool needsDownloadFromOfflineRun { false };
    std::string installFailed;
    // D-5 (plan 0.0.9): how the last description the person asked for with the network (mcppls.describeOnline) ended,
    // S3 `onlineRun`: {outcome, message, at}; null before the first. It stays until the next such run ends.
    Json onlineRun;
    bool loadRunsOffline { true };                       // the load running now: the producer is started offline
    // Plan 2026-09-27 B-2, §9.2: fetching what the build description needs is the person's decision, made once per
    // workspace in their editor; `onlineOnce` makes the next load one that may reach the network, and only that one.
    bool onlineOnce { false };
    bool describingOnline { false };                     // the load running now is that one
    // §9.2 rule 4: needing a download is not the end of it. The person may build or install in their own terminal at
    // any time; the offline description is asked again after 30 s, 1, 2 and then every 5 minutes, and at once when a
    // watched input changes, so the better model comes on its own.
    std::size_t downloadRetries { 0 };
    int invalidFileRetries { 0 };            // X-5: rereads of a database that was invalid while its writer was at it
    std::optional<Clock::time_point> downloadRetryAt;
    bool inferredLoadStarted { false };
    // Plan 2026-09-27 D5 (revising fix plan F4, D1): with nothing cached, the build tool has FIRST_MODEL_WAIT to
    // describe the project; after it, the project is served from its scanned sources (L4) -- mcppls's engine and clangd
    // alike -- while the build tool goes on, and its model replaces the provisional one in a single switch that is
    // never counted against clangd's restart budget. CORE_WAIT_LIMIT is what clangd waits beyond that: nothing.
    static constexpr std::chrono::milliseconds FIRST_MODEL_WAIT { 2500 };
    static constexpr std::chrono::milliseconds CORE_WAIT_LIMIT { 0 };
    // P-2 (plan 0.0.9): what clangd waits beyond FIRST_MODEL_WAIT when this project's build tool is known to answer soon
    // after it -- 1.2 times its last measured time, up to CORE_WAIT_CAP. Starting clangd on the scanned model costs a
    // preamble built with the wrong commands and a restart when the build tool answers (issue #37: two 3 s preambles on
    // vulkan-hpp); mcppls's own engine answers either way. With no measurement it is CORE_WAIT_LIMIT, as before.
    static constexpr std::chrono::milliseconds CORE_WAIT_CAP { 5500 };
    std::optional<Clock::time_point> coreWaitUntil;
    bool coreWaitOver { false };
    std::optional<std::chrono::milliseconds> producerElapsed;   // set while the producer is past its soft bound
    // Written by the load thread, read by the event loop: how long the producer has been running
    // once it passed its soft bound. Nothing else crosses that boundary.
    std::shared_ptr<std::atomic<std::int64_t>> producerSlowMs { std::make_shared<std::atomic<std::int64_t>>(0) };
    std::optional<Clock::time_point> loadStartedAt;   // when the load in flight started
    std::int64_t lastProducerMs { 0 };                // how long this project's producer last took to answer; 0 unknown
    std::optional<Clock::time_point> producerSoftAt;
    std::optional<Clock::time_point> lastManualReloadAt;
    std::string journaledToolEnvironment;                // the environment source the journal last recorded

    std::string lastStatus;                  // the last cxxModules/status sent, serialized
    std::function<void(std::string)> autoBundleRequest;          // K-7
    std::map<std::string, std::string, std::less<>> autoBundles;  // issue code -> the bundle written for it
    std::set<std::string, std::less<>> autoBundleRequested;       // codes a bundle was asked for in this session
    State lastSentState { State::starting };
    std::optional<Clock::time_point> lastStatusSentAt;
    std::optional<Clock::time_point> statusFlushAt;   // a coalesced change goes out then
    std::optional<Clock::time_point> degradedSince;   // when compute_state() turned degraded, while that is held back
    std::optional<Clock::time_point> preparingSince;  // K-6: when a settled workspace turned preparing, while that is held back
    bool settled { false };                           // K-6: it has been ready or degraded with the core engine serving
    State lastReportedState { State::starting };      // the state last let through DEGRADED_HOLD

    Impl(std::string root_, std::string key_, SessionOptions options_, engine::PayloadPaths payload_, bool payloadCorrupt_,
         bool kitEnabled_, std::string compilerOverride_, std::shared_ptr<EventChannel> events_, ClientSink& client_)
        : root { std::move(root_) }, key { std::move(key_) }, options { std::move(options_) }, events { std::move(events_) }, client { client_ },
          compilerOverride { std::move(compilerOverride_) }, kitEnabled { kitEnabled_ }, payload { std::move(payload_) },
          payloadCorrupt { payloadCorrupt_ } {
        const std::string workspaceDirectory { base::join_path(platform::dirs::cache_directory(), base::join_path("workspaces", project::workspace_key(root))) };
        lease = WorkspaceLease::acquire(workspaceDirectory, std::chrono::system_clock::now(), root);
        workspaceDirectory_ = workspaceDirectory;
        ownToken_ = lease->token();
        cacheDirectory = lease->directory();
        // The heartbeat tick is every instance's own (C-9): a guest keeps its `instance.json` fresh
        // with it, so a sweep can tell it from a dead one without touching the owner's lease.
        leaseRenewAt = Clock::now() + LEASE_RENEWAL;
        // Under its one name, like every file the engines are given (engine_uri): the prime units
        // and the database live here, and clangd answers for them under the name it was given.
        (void)platform::fs::create_directories(cacheDirectory);
        cacheDirectory = platform::fs::canonical_path(cacheDirectory);
        // usable plan W9.3: taken now, before this root is even started, so the client cannot
        // possibly have created or changed a watched file yet (see start_watch_polling below).
        watchBaseline = watched_files_snapshot(root);
        if (options.engineFactories) {
            EngineFactories factories { options.engineFactories(options, payload, payloadCorrupt) };
            if (factories.modules) {
                engines.push_back(factories.modules(index));
                moduleEngine = engines.back().get();
            }
            if (factories.core) {
                if (auto core = factories.core()) {
                    engines.push_back(std::move(core));
                    coreEngine = engines.back().get();
                }
            }
        }
        // C-7/C-8/C-9 (plan 2026-10-03): every start begins in a cache the sweepers have made fit
        // again -- dead instances reaped, the previous generation's copies gone, the budget asked.
        // All in the background: the start path itself waits for nothing (plan §5).
        start_cache_task_("startup");
    }

    // ---- engine::Host ---------------------------------------------------------

    const std::string& root_directory() const override { return root; }
    const std::string& cache_directory() const override { return cacheDirectory; }
    const Json& client_initialize_params() const override { return clientParams; }

    std::function<void(Json)> event_sink(std::string_view engineId) override {
        auto queue = events;
        return [queue, rootKey = key, id = std::string { engineId }](Json event) {
            queue->push(Event { EventKind::engine_event, std::move(event), 0, {}, rootKey, id });
        };
    }

    void send_to_client(const Json& message) override { client.send(message); }

    std::string client_request_id(std::string_view engineId, int generation, const Json& engineRequestId) const override {
        return make_engine_request_key(key, engineId, generation, engineRequestId);
    }

    void publish_engine_diagnostics(std::string_view engineId, const std::string& uri, Json diagnostics, std::optional<std::int64_t> version) override {
        engineDiagnostics[std::string { engineId }][uri] = std::move(diagnostics);
        if (coreEngine != nullptr && engineId == coreEngine->id()) coreDiagnosticsVersions[uri] = version.value_or(-1);
        publish_diagnostics(uri, true);
    }

    void forget_engine_diagnostics(std::string_view engineId) override {
        engineDiagnostics.erase(std::string { engineId });
        if (coreEngine != nullptr && engineId == coreEngine->id()) coreDiagnosticsVersions.clear();
    }

    void engine_settled(std::string_view engineId, const Json& serverCapabilities) override {
        if (coreEngine != nullptr && engineId != coreEngine->id()) return;
        if (coreEngine != nullptr) coreCapabilities = serverCapabilities;
        // Rebuilt from the very capabilities merge_capabilities is about to see (or already saw,
        // for the first root), so a request's core-engine tokens are always remapped by the same
        // legend the client was told about.
        tokensLegend = tokens::build_legend(coreCapabilities);
        if (settledReported || !onEngineSettled) return;
        settledReported = true;
        auto callback = std::move(onEngineSettled);
        onEngineSettled = {};
        callback(merge_capabilities(coreCapabilities));
    }

    void status_changed() override { update_status(); }
    void request_replan() override { schedule_replan(); }
    void record_event(std::string_view kind, Json detail) override { journal.add(kind, std::move(detail)); }

    // Fix plan F17.2: an engine's incident, with what led up to it from this workspace's side (its latest events,
    // the model and the plan), written to the cache directory off the event loop.
    void record_incident(std::string_view kind, Json detail, std::vector<engine::IncidentFile> files, std::optional<std::int64_t> pid) override {
        Json incident {
            { "format", 1 },
            { "kind", std::string { kind } },
            { "at", std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now())) },
            { "server", std::string { base::VERSION } },
            { "root", root },
            { "model", model ? Json { { "source", std::string { project::to_string(model->source) } }, { "level", model->level }, { "origin", modelOrigin },
                                      { "profile", profile_json() }, { "stale", staleModelReason } }
                             : Json(nullptr) },
            { "plan", Json { { "entries", plan.entries.size() }, { "standIns", plan.stubModules }, { "openSources", plan.openSources },
                             { "leftOut", plan.excludedFiles.size() }, { "issues", plan.issues.size() } } },
            { "detail", std::move(detail) },
            { "events", journal.recent(60) },
        };
        journal.add("incident", Json { { "kind", std::string { kind } } });
        std::thread { [cache = cacheDirectory, kind = std::string { kind }, incident = std::move(incident), files = std::move(files), pid]() mutable {
            if (auto written = incidents::write(cache, kind, std::move(incident), std::move(files), pid)) {
                log::info("incident {} written to {}", kind, *written);
            } else {
                log::warning("incident {} could not be written: {}", kind, written.error().message);
            }
        } }.detach();
    }

    // ---- the cache mcppls owns (0.0.10 plan C-7, C-8, C-9, C-13.1) ------------------------------

    cache::Budget cache_budget() const {
        cache::Budget budget;
        if (const auto bytes = cache::parse_bytes(options.settings.string_value("cache.maxBytes"))) budget.perWorkspace = *bytes;
        if (const auto bytes = cache::parse_bytes(options.settings.string_value("cache.totalBytes"))) budget.total = *bytes;
        return budget;
    }

    std::chrono::seconds cache_grace() const { return options.settings.seconds_value("cache.instanceGrace"); }

    // One background pass: dead instances first (they can free the most), then the copies, then the
    // budget across the workspaces nothing has open. `before` is the sweep's bound on the file
    // clock; the result arrives as a `cache_swept` event, so the journal, the status and the report
    // cache are all touched on the session loop, as everything else is.
    void start_cache_task_(std::string_view origin, std::int64_t before = platform::fs::modified_now()) {
        const cache::Budget budget { cache_budget() };
        const auto grace { cache_grace() };
        const std::string workspaceDirectory { workspaceDirectory_ };
        const std::string ownCacheDirectory { cacheDirectory };
        const std::string ownKey { base::file_name(workspaceDirectory_) };
        const std::string ownToken { ownToken_ };
        auto queue = events;
        const std::string rootKey { key };
        std::thread { [budget, grace, workspaceDirectory, ownCacheDirectory, ownKey, ownToken, origin = std::string { origin }, before, queue, rootKey]() mutable {
            const auto now { std::chrono::system_clock::now() };
            std::uint64_t bytes { 0 };
            std::size_t files { 0 }, instances { 0 }, failed { 0 };
            const cache::Sweep dead { cache::sweep_instances(workspaceDirectory, now, grace) };
            bytes += dead.bytes;
            files += dead.files;
            instances += dead.instances;
            failed += dead.failed;
            for (const auto& context : cache::contexts_of(ownCacheDirectory)) {
                const cache::Sweep one { cache::sweep_copies(cache::modules_root(context), before) };
                bytes += one.bytes;
                files += one.files;
                failed += one.failed;
            }
            const cache::Sweep over { cache::enforce_budget(base::join_path(platform::dirs::cache_directory(), "workspaces"), budget, ownKey, now) };
            bytes += over.bytes;
            files += over.files;
            failed += over.failed;
            Json report;
            try {
                report = cache::report(ownCacheDirectory, budget, std::chrono::system_clock::now(), ownToken);
            } catch (...) {
                report = Json::object();   // a report is never worth a crashed sweeper thread
            }
            queue->push(Event { EventKind::cache_swept,
                                Json { { "origin", origin }, { "bytes", bytes }, { "files", files }, { "instances", instances },
                                       { "failed", failed }, { "report", std::move(report) } },
                                0, {}, rootKey, {} });
        } }.detach();
    }

    // C-7: the engine is starting -- nothing of this instance's engines uses the cache tree, and
    // `before` (the new generation's start) protects whatever it writes from here on.
    void cache_sweep_due(std::int64_t before) override { start_cache_task_("engine-start", before); }

    // The status' `cache`: coarse numbers only (100 MB grain), so a cache that changes under
    // clangd's hands does not turn every status notification into a new one (S3-4-29).
    Json cache_fragment_() {
        std::lock_guard lock(cacheMutex_);
        if (!cacheSnapshot_.is_object()) return Json();
        const std::uint64_t limit { cacheSnapshot_.value("limits", Json::object()).value("perWorkspace", std::uint64_t { 0 }) };
        const std::uint64_t bytes { cacheSnapshot_.value("bytes", std::uint64_t { 0 }) };
        constexpr std::uint64_t GRAIN { std::uint64_t { 100 } * 1000 * 1000 };
        Json fragment { { "bytes", (bytes + GRAIN - 1) / GRAIN * GRAIN },
                        { "limitBytes", limit },
                        { "state", cacheSnapshot_.value("level", std::string { "ok" }) },
                        { "copies", cacheSnapshot_.value("copies", Json::object()) },
                        { "instances", Json { { "count", cacheSnapshot_.value("instances", Json::object()).value("count", std::size_t { 0 }) },
                                              { "bytes", cacheSnapshot_.value("instances", Json::object()).value("bytes", std::uint64_t { 0 }) } } } };
        if (lastSweepAt_) {
            fragment["lastSweep"] = Json { { "at", *lastSweepAt_ }, { "freedBytes", lastSweepFreed_ }, { "files", lastSweepFiles_ } };
        }
        return fragment;
    }

    // The facts both prompts are rendered from (D19): what a local agent needs to look, and nothing
    // that would have to leave the machine.
    Json cache_prompt_facts_() const {
        Json facts { { "version", std::string { base::VERSION } },
                     { "root", root },
                     { "cacheRoot", workspaceDirectory_ },
                     { "os", std::string { mcppls::os::FAMILY_NAME } },
                     { "logDirectory", base::parent_path(log::file_path()) } };
        if (clientParams.is_object()) {
            const Json& info { clientParams.value("clientInfo", Json::object()) };
            facts["editor"] = info.value("name", std::string {});
            facts["editorVersion"] = info.value("version", std::string {});
        }
        if (model) facts["buildSystem"] = std::string { project::to_string(model->source) };
        Json list = Json::array();
        for (const auto& engine : engines) {
            const engine::EngineStatus status { engine->status() };
            list.push_back(Json { { "name", status.name }, { "version", status.version } });
        }
        facts["engines"] = std::move(list);
        return facts;
    }

    // Semantic tokens (design doc 2026-09-25 K/§7): coalesced to at most one
    // workspace/semanticTokens/refresh every ~500ms, and never before this root's own initialize
    // was answered (the same gate update_status uses).
    void semantic_tokens_changed() override {
        if (!clientSupportsTokensRefresh || !initializeAnswered) return;
        if (tokensRefreshAt) return;
        tokensRefreshAt = Clock::now() + std::chrono::milliseconds { 500 };
    }

    std::vector<engine::DocumentView> documents() const override {
        std::vector<engine::DocumentView> views;
        for (const Document* document : documents_.all()) views.push_back(view_of(*document));
        return views;
    }

    bool has_document(std::string_view clientUri) const override { return documents_.find(clientUri) != nullptr; }

    // The engines are given every file under the one name the model uses for it (v1 design 14.3's
    // "one file, one name"): clangd matches an unsaved buffer to the module source it builds by name.
    std::string engine_uri(std::string_view uri) const override {
        if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) {
            const std::string path { path_of_uri(uri) };
            if (path.size() < 2 || path[1] != ':') return std::string { uri };
            return "file:///" + path.substr(0, 2) + base::percent_encode_path(std::string_view { path }.substr(2));
        } else {
            const auto written = base::uri_to_path(uri);
            if (!written) return std::string { uri };
            const std::string path { path_of_uri(uri) };
            return path.empty() || path == *written ? std::string { uri } : base::path_to_uri(path);
        }
    }

    // The client's URI for a document an engine names in its own form.
    std::string client_uri(std::string_view engineUri) const override {
        if (documents_.find(engineUri) != nullptr) return std::string { engineUri };
        const std::string path { path_of_uri(engineUri) };
        if (const Document* document = path.empty() ? nullptr : documents_.find_by_path(path)) return document->uri;
        return std::string { engineUri };
    }

    void client_view(Json& value) const override {
        if (value.is_array()) {
            for (auto& element : value) client_view(element);
            return;
        }
        if (!value.is_object()) return;
        for (auto item = value.begin(); item != value.end(); ++item) {
            if ((item.key() == "uri" || item.key() == "targetUri") && item.value().is_string()) {
                item.value() = client_uri(item.value().get<std::string>());
            } else if (item.key() == "changes" && item.value().is_object()) {
                Json renamed = Json::object();
                for (auto change = item.value().begin(); change != item.value().end(); ++change) renamed[client_uri(change.key())] = change.value();
                item.value() = std::move(renamed);
            } else {
                client_view(item.value());
            }
        }
    }

    // A file's path under the one name the model uses for it (see engine_uri).
    std::string path_of_uri(std::string_view uri) const override {
        const std::string key_ { uri };
        if (const auto cached = canonicalByUri.find(key_); cached != canonicalByUri.end()) return cached->second;
        auto path = base::uri_to_path(uri);
        std::string canonical { path ? platform::fs::canonical_path(*path) : std::string {} };
        if (canonicalByUri.size() > 4096) canonicalByUri.clear();
        canonicalByUri.emplace(key_, canonical);
        return canonical;
    }

    std::vector<std::string> imports_of(std::string_view path) const override {
        const auto* scan = index.scan_of(path);
        return scan == nullptr ? std::vector<std::string> {} : project::required_names(*scan);
    }

    // ---- plumbing ---------------------------------------------------------------

    static engine::DocumentView view_of(const Document& document) {
        return engine::DocumentView { document.uri, document.path, document.languageId, document.version, document.text };
    }

    std::optional<Clock::time_point> next_deadline() const {
        std::optional<Clock::time_point> deadline;
        auto consider = [&](const std::optional<Clock::time_point>& at) {
            if (at && (!deadline || *at < *deadline)) deadline = at;
        };
        consider(reloadAt);
        consider(replanAt);
        if (!coreWaitOver) consider(coreWaitUntil);
        consider(loadGiveUpAt);
        consider(downloadRetryAt);
        consider(lastResortAt);
        consider(producerSoftAt);
        consider(sdkCheckAt);
        consider(statusFlushAt);
        consider(leaseRenewAt);
        consider(tokensRefreshAt);
        for (const auto& [id, job] : jobs) consider(job.budgetAt);
        for (const auto& [id, late] : lateCompletions) consider(late.until);
        for (const auto& engine : engines) consider(engine->next_deadline());
        return deadline;
    }

    // usable plan W9.3: every 2s, compares size and modification time of the same build description
    // and source files a dynamic watch would cover, and turns a difference into the exact
    // notification workspace/didChangeWatchedFiles would have carried, so handle_watched_files treats
    // a polled change exactly like an editor's own. The thread touches only its own snapshot and the
    // shared event queue; the workspace changes state only on the event loop.
    void start_watch_polling() {
        auto queue = events;
        const std::string rootPath { root };
        std::thread { [queue, rootPath, known = watchBaseline, patterns = watchPatterns]() mutable {
            std::vector<std::string> knownEntries;
            int knownGeneration { 0 };
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds { 2 });
                std::vector<std::string> entries;
                int generation { 0 };
                {
                    const std::lock_guard lock { patterns->mutex };
                    entries = patterns->entries;
                    generation = patterns->generation;
                }
                std::map<std::string, platform::fs::FileStamp> current { watched_files_snapshot_of(rootPath, entries) };
                // A model that names new inputs brings files into the watch that were there all along:
                // they start from what they are now rather than being reported as created.
                const bool entriesChanged { generation != knownGeneration };
                Json changes = Json::array();
                for (const auto& [file, fileStamp] : current) {
                    const auto previous = known.find(file);
                    if (previous == known.end()) {
                        if (entriesChanged && !watch_covers(knownEntries, rootPath, file)) continue;
                        changes.push_back(Json { { "uri", base::path_to_uri(file) }, { "type", 1 } });
                    } else if (previous->second != fileStamp) {
                        changes.push_back(Json { { "uri", base::path_to_uri(file) }, { "type", 2 } });
                    }
                }
                for (const auto& [file, fileStamp] : known) {
                    if (current.contains(file)) continue;
                    if (entriesChanged && !watch_covers(entries, rootPath, file)) continue;
                    changes.push_back(Json { { "uri", base::path_to_uri(file) }, { "type", 3 } });
                }
                known = std::move(current);
                knownEntries = std::move(entries);
                knownGeneration = generation;
                if (changes.empty()) continue;
                // `Json message { make_notification(...) }` would wrap the result in a one-element array.
                Json message = lsp::make_notification("workspace/didChangeWatchedFiles", Json { { "changes", std::move(changes) } });
                queue->push(Event { EventKind::client_message, std::move(message) });
            }
        } }.detach();
    }

    // ---- requests ---------------------------------------------------------------------

    void route_client_request(const Json& message) {
        const std::uint64_t jobId { nextJob++ };
        Job& job = jobs[jobId];
        job.started = Clock::now();
        job.clientId = message["id"];
        job.method = message.value("method", std::string {});
        job.message = message;
        // Semantic tokens (design doc 2026-09-25 K/§7): this server advertises `full` with no
        // `delta` (contract T0), so it never hands out a resultId a delta request could build on.
        // A client that sends one anyway is answered like `full`, which LSP allows.
        if (job.method == "textDocument/semanticTokens/full/delta") {
            job.method = "textDocument/semanticTokens/full";
            job.message["method"] = job.method;
            if (job.message.contains("params") && job.message["params"].is_object()) job.message["params"].erase("previousResultId");
        }
        job.params = job.message.contains("params") ? job.message["params"] : Json::object();
        const std::string uri { uri_of_params(job.params) };
        job.path = uri.empty() ? std::string {} : path_of_uri(uri);
        const Document* document { uri.empty() ? nullptr : documents_.find(uri) };
        if (document != nullptr) job.text = document->text;
        job.view = engine::RequestView { job.method, &job.params, job.path, job.text };
        if (job.method == lsp::method::TEXT_DOCUMENT_COMPLETION) {
            if (completion::is_space_trigger(job.params)) {
                route_space_triggered(jobId);
                return;
            }
            if (document != nullptr && !job.path.empty()) {
                if (const auto position = position_of(job.params)) {
                    job.keywords = index::keyword_completion(job.text, *position, index.scan_of(job.path),
                                                             index::KeywordOptions { .suggestModulesAfterImport = vscodeLike });
                    if (answered_late(jobId, uri, *position)) return;
                }
            }
        }

        std::vector<engine::Engine*> candidates;
        const bool coreWaits { core_waits_for_producer() };
        for (const auto& engine : engines) {
            if (!(coreWaits && engine.get() == coreEngine)) candidates.push_back(engine.get());
        }
        Selection selection { select_engines(candidates, job.view) };
        if (!selection.mergers.empty()) {
            job.merging = true;
            job.awaiting = selection.mergers.size();
            job.engineSentAt = Clock::now();
            // Semantic tokens: the core engine's own answer arrives in its own legend's indices;
            // remapped into this server's legend right here, once, so routing::merge_results (and
            // everything downstream) only ever sees the server's own index space (routing itself
            // stays a pure function of what several engines already answered).
            const bool remapCoreTokens { coreEngine != nullptr
                                        && (job.method == "textDocument/semanticTokens/full" || job.method == "textDocument/semanticTokens/range") };
            for (engine::Engine* merger : selection.mergers) {
                const std::string engineId { merger->id() };
                engine::Reply reply { [this, jobId, engineId](engine::Answer answer) { merge_answer(jobId, engineId, std::move(answer)); } };
                if (remapCoreTokens && engineId == coreEngine->id()) {
                    reply = [this, jobId, engineId](engine::Answer answer) {
                        if (answer.kind == engine::Answer::Kind::result) answer.value = tokens::remap_core_tokens(answer.value, tokensLegend);
                        merge_answer(jobId, engineId, std::move(answer));
                    };
                }
                merger->request(job.view, job.message, std::move(reply));
                if (!jobs.contains(jobId)) return;
            }
            return;
        }
        if (selection.answerers.empty()) {
            finish_job(jobId, Json(nullptr));
            return;
        }
        job.answerers = std::move(selection.answerers);
        if (const auto budget = answer_budget(job.method); budget && coreEngine != nullptr
            && std::ranges::find(job.answerers, coreEngine) != job.answerers.end()) {
            job.budgetAt = job.started + *budget;
        }
        ask_next(jobId);
    }

    static std::optional<base::Position> position_of(const Json& params) {
        const Json* position { lsp::find(params, "position") };
        if (position == nullptr || !position->is_object()) return std::nullopt;
        const auto line = lsp::int_at(*position, "line");
        const auto character = lsp::int_at(*position, "character");
        if (!line || !character) return std::nullopt;
        return base::Position { static_cast<int>(*line), static_cast<int>(*character) };
    }

    // F9 (D4 layer 2): a completion the client asked for because a space was typed. Only the line
    // before the cursor is looked at: anything but `[export] import ` is answered at once, empty,
    // without an engine, a lookup or a log line; an import line gets the module names from mcppls's
    // own engine, never from the core engine.
    void route_space_triggered(std::uint64_t jobId) {
        Job& job = jobs.at(jobId);
        const auto gateStart = Clock::now();
        std::optional<std::string_view> prefix;
        if (const auto position = position_of(job.params)) prefix = completion::line_prefix(job.text, *position);
        const bool passed { prefix && completion::is_import_line_prefix(*prefix) };
        const std::int64_t micros { std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - gateStart).count() };
        ++spaceTrigger.count;
        spaceTrigger.maxMicros = std::max(spaceTrigger.maxMicros, micros);
        spaceTrigger.totalMicros += micros;
        if (!passed) {
            const Json id = job.clientId;
            jobs.erase(jobId);
            client.reply(id, completion::empty_list());
            return;
        }
        ++spaceTrigger.passed;
        if (moduleEngine != nullptr && moduleEngine->claims(job.view)) job.answerers = { moduleEngine };
        if (job.answerers.empty()) {
            finish_job(jobId, completion::empty_list());
            return;
        }
        ask_next(jobId);
    }

    // R-7 (plan 2026-09-30): the core engine has not answered in the request's budget (answer_budget). mcppls answers
    // with what it has -- a completion the file's words as an incomplete list, with the keywords (F15) merged in by
    // finish_job; a hover what explain_if_preparing says -- and the core engine's request is cancelled. The job is
    // finished first, so the engines' answers to the cancellation find nothing to finish. The core engine's own
    // timeouts and watchdogs still see the request until clangd lets go of it.
    void answer_without_core(std::uint64_t jobId) {
        Job& job = jobs.at(jobId);
        const Json clientId { job.clientId };
        if (job.keywords.is_array() && !job.keywords.empty()) ++keywordsWithoutEngine;
        job.answeredBy = "mcppls";
        Json result;
        bool keepCore { false };
        if (job.method == lsp::method::TEXT_DOCUMENT_COMPLETION) {
            const auto position = position_of(job.params);
            result = completion::without_engine(position ? completion::document_words(job.text, *position) : Json::array());
            // C-2: the core engine's request stays, for the requests the same word makes next.
            const bool coreAsked { !job.waitsForLate && job.next > 0 && job.answerers[job.next - 1] == coreEngine };
            if (coreAsked && position) {
                if (auto key = completion::word_key(job.text, *position)) {
                    lateCompletions[jobId] = LateCompletion { uri_of_params(job.params), std::move(*key), clientId, Clock::now() + LATE_COMPLETION_KEEP, {} };
                    keepCore = true;
                }
            }
        }
        finish_job(jobId, std::move(result));
        for (const auto& engine : engines) {
            if (keepCore && engine.get() == coreEngine) engine->detach(clientId);
            else engine->cancel(clientId);
        }
    }

    // C-2: a completion in a word the core engine answered late is given that answer, at once; one in a word the core
    // engine is still working on waits for it, within its own budget. A completion in another word ends the core
    // engine's work on the one before: nobody is typing it any more. An import directive's module names are mcppls's own.
    bool answered_late(std::uint64_t jobId, const std::string& uri, base::Position position) {
        if (coreEngine == nullptr) return false;
        Job& job = jobs.at(jobId);
        const auto prefix = completion::line_prefix(job.text, position);
        auto key = completion::word_key(job.text, position);
        if (!prefix || completion::in_import_directive(*prefix) || !key) return false;
        if (const auto answer = lateAnswers.find(uri); answer != lateAnswers.end()) {
            if (completion::typed_on(answer->second.key, *key) && Clock::now() - answer->second.at <= LATE_ANSWER_KEPT) {
                job.coreAnswered = true;
                job.answeredBy = std::string { coreEngine->id() };
                ++lateCompletionsServed;
                finish_job(jobId, completion::retarget(answer->second.result, position));
                return true;
            }
            lateAnswers.erase(answer);
        }
        for (auto it = lateCompletions.begin(); it != lateCompletions.end();) {
            if (it->second.uri != uri) {
                ++it;
            } else if (completion::typed_on(it->second.key, *key)) {
                it->second.waiting.push_back(jobId);
                job.waitsForLate = true;
                if (const auto budget = answer_budget(job.method)) job.budgetAt = job.started + *budget;
                return true;
            } else {
                coreEngine->cancel(it->second.clientId);
                it = lateCompletions.erase(it);
            }
        }
        return false;
    }

    // C-2: the core engine answered a completion whose budget had passed.
    void late_completion_answered(std::uint64_t jobId, const engine::Answer& answer) {
        const auto it = lateCompletions.find(jobId);
        if (it == lateCompletions.end()) return;
        LateCompletion late { std::move(it->second) };
        lateCompletions.erase(it);
        // An empty answer or an error is nothing to give: whoever waits for it gets the file's words at its budget.
        if (answer.kind != engine::Answer::Kind::result || completion::is_empty(answer.value)) return;
        ++lateCompletionsArrived;
        lateAnswers[late.uri] = LateAnswer { late.key, answer.value, Clock::now() };
        for (const std::uint64_t waiter : late.waiting) {
            auto job = jobs.find(waiter);
            if (job == jobs.end()) continue;
            const auto position = position_of(job->second.params);
            if (!position) continue;
            job->second.coreAnswered = true;
            job->second.answeredBy = std::string { coreEngine->id() };
            ++lateCompletionsServed;
            finish_job(waiter, completion::retarget(answer.value, *position));
        }
    }

    void ask_next(std::uint64_t jobId) {
        auto it = jobs.find(jobId);
        if (it == jobs.end()) return;
        Job& job = it->second;
        if (job.next >= job.answerers.size()) {
            finish_job(jobId, Json(nullptr));
            return;
        }
        engine::Engine* answerer { job.answerers[job.next++] };
        job.engineSentAt = Clock::now();
        answerer->request(job.view, job.message, [this, jobId, engineId = std::string { answerer->id() }](engine::Answer answer) {
            auto current = jobs.find(jobId);
            if (current == jobs.end()) {
                late_completion_answered(jobId, answer);
                return;
            }
            switch (answer.kind) {
            case engine::Answer::Kind::result:
                if (coreEngine != nullptr && engineId == coreEngine->id()) current->second.coreAnswered = true;
                if (!answer.value.is_null()) {
                    current->second.engineRepliedAt = Clock::now();
                    current->second.answeredBy = engineId;
                    finish_job(jobId, std::move(answer.value));
                    return;
                }
                ask_next(jobId);
                return;
            case engine::Answer::Kind::unavailable: ask_next(jobId); return;
            case engine::Answer::Kind::error:
                // F15, M-2 (plan 0.0.8): a completion gives what mcppls has -- the file's words, the keywords --
                // rather than the engine's error, which an editor shows as no completion at all.
                if (current->second.method == lsp::method::TEXT_DOCUMENT_COMPLETION) finish_job(jobId, Json(nullptr));
                else finish_job_with_error(jobId, std::move(answer.value));
                return;
            case engine::Answer::Kind::cancelled: finish_job_cancelled(jobId); return;
            }
        });
    }

    void merge_answer(std::uint64_t jobId, const std::string& engineId, engine::Answer answer) {
        auto it = jobs.find(jobId);
        if (it == jobs.end()) return;
        Job& job = it->second;
        switch (answer.kind) {
        case engine::Answer::Kind::result: job.merged.emplace_back(engineId, std::move(answer.value)); break;
        case engine::Answer::Kind::unavailable: break;
        case engine::Answer::Kind::error:
            if (!job.error) job.error = std::move(answer.value);
            break;
        case engine::Answer::Kind::cancelled: job.cancelled = true; break;
        }
        if (--job.awaiting > 0) return;
        job.answeredBy = "merged";
        job.engineRepliedAt = Clock::now();
        if (job.cancelled) {
            finish_job_cancelled(jobId);
        } else if (job.error) {
            finish_job_with_error(jobId, std::move(*job.error));
        } else {
            finish_job(jobId, merge_results(job.method, job.merged));
        }
    }

    // One request done: its duration, outcome and answering engine counted by method (robustness design O3).
    void note_request(const Job& job, std::string_view outcome) {
        auto& stats = requestStats[job.method];
        ++stats.count;
        if (outcome == "empty") ++stats.empty;
        else if (outcome == "error") ++stats.errors;
        else if (outcome == "cancelled") ++stats.cancelled;
        const double ms { std::chrono::duration<double, std::milli>(Clock::now() - job.started).count() };
        stats.recentMs.push_back(ms);
        if (stats.recentMs.size() > 256) stats.recentMs.pop_front();
        stats.maxMs = std::max(stats.maxMs, ms);
        if (!job.answeredBy.empty()) ++stats.answeredBy[job.answeredBy];
        if (job.engineSentAt && job.engineRepliedAt && outcome == "result") {
            const double engineMs { std::chrono::duration<double, std::milli>(*job.engineRepliedAt - *job.engineSentAt).count() };
            stats.recentEngineMs.emplace_back(engineMs, std::max(0.0, ms - engineMs));
            if (stats.recentEngineMs.size() > 256) stats.recentEngineMs.pop_front();
        }
        stats.lastAt = std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now()));
        if (!job.path.empty() && (fileRequestStats.size() < FILE_STATS_LIMIT || fileRequestStats.contains(job.path))) {
            auto& file = fileRequestStats[job.path];
            ++file.count;
            file.maxMs = std::max(file.maxMs, ms);
            if (job.method == lsp::method::TEXT_DOCUMENT_COMPLETION && job.answeredBy == "mcppls") ++file.completionsWithoutCore;
            auto& recent = file.recentMs[job.method];
            recent.push_back(ms);
            if (recent.size() > 64) recent.pop_front();
        }
        if (ms >= 5000) {
            journal.add("slow-request", Json { { "method", job.method }, { "file", job.path }, { "ms", static_cast<std::int64_t>(ms) },
                                               { "outcome", std::string { outcome } }, { "answeredBy", job.answeredBy } });
        }
    }

    // A hover that would answer nothing, while modules are still being prepared, says so instead
    // (cold-start plan 4.3).
    //
    // An empty answer and "not ready yet" look identical to a person: they hover, nothing appears,
    // and they conclude the feature is broken. The status bar carries the same fact, but it is
    // read by someone who goes looking; this reaches the person at the moment they are confused,
    // with the cursor already on the symbol.
    //
    // Only `textDocument/hover`, and only when the engines produced nothing. Hover is the one
    // request whose result can carry prose — `definition` answers with locations and has nowhere
    // to put a sentence — and replacing a real answer would be worse than the silence it fixes.
    Json explain_if_preparing(const Job& job, Json result) const {
        if (!result.is_null() || job.method != "textDocument/hover" || coreEngine == nullptr) return result;
        const engine::EngineStatus core { coreEngine->status() };
        const bool preparing { core.toPrepare > 0 && core.prepared < core.toPrepare };
        // The core engine is building what this file needs on its own, without preparation of ours to
        // count (real-project plan RP1.1): a module it imports changed and is being compiled again.
        // Silence would read as "nothing here"; this says to come back.
        if (!preparing && !coreEngine->busy_with(job.path)) return result;
        const std::string value { preparing
            ? std::format("**mcppls** is still preparing modules ({}/{}).\n\n"
                          "Answers for this file need the modules it imports to be built first. Try again shortly.",
                          core.prepared, core.toPrepare)
            : std::string { "**mcppls**: the modules this file imports are still being built.\n\n"
                            "Answers for this file need them first. Try again shortly." } };
        return Json { { "contents", Json { { "kind", "markdown" }, { "value", value } } } };
    }

    void finish_job(std::uint64_t jobId, Json result) {
        auto it = jobs.find(jobId);
        if (it == jobs.end()) return;
        // M-2 (plan 0.0.8): a completion the core engine did not answer -- not asked, because the file is set
        // aside, doomed or the engine is backing off, or unable to -- has at least what R-7's budget answer has:
        // the file's words, as an incomplete list. Never on an import line, where only module names belong.
        if (it->second.method == lsp::method::TEXT_DOCUMENT_COMPLETION && coreEngine != nullptr && !it->second.coreAnswered
            && completion::is_empty(result)) {
            if (const auto position = position_of(it->second.params)) {
                const auto prefix = completion::line_prefix(it->second.text, *position);
                Json words = prefix && !completion::in_import_directive(*prefix) ? completion::document_words(it->second.text, *position) : Json::array();
                if (!words.empty()) {
                    result = completion::without_engine(std::move(words));
                    if (it->second.answeredBy.empty()) it->second.answeredBy = "mcppls";
                    ++wordsWithoutCore;
                }
            }
        }
        // F15: the module-syntax keywords, with the engine's items, or alone (and incomplete, so the
        // client asks again) when the engine gave none.
        if (it->second.keywords.is_array() && !it->second.keywords.empty()) {
            result = result.is_null() ? completion::keywords_only(it->second.keywords) : completion::merge(result, it->second.keywords);
        }
        result = explain_if_preparing(it->second, std::move(result));
        note_request(it->second, result.is_null() ? "empty" : "result");
        const Json id = it->second.clientId;
        jobs.erase(it);
        client.reply(id, std::move(result));
    }

    void finish_job_with_error(std::uint64_t jobId, Json error) {
        auto it = jobs.find(jobId);
        if (it == jobs.end()) return;
        note_request(it->second, "error");
        Json response { { "jsonrpc", "2.0" }, { "id", it->second.clientId }, { "error", std::move(error) } };
        jobs.erase(it);
        client.send(response);
    }

    void finish_job_cancelled(std::uint64_t jobId) {
        auto it = jobs.find(jobId);
        if (it == jobs.end()) return;
        note_request(it->second, "cancelled");
        const Json id = it->second.clientId;
        jobs.erase(it);
        client.reply_error(id, lsp::REQUEST_CANCELLED, "cancelled");
    }

    // A unit of the model whose module declaration or imports changed needs a new plan; a new
    // source file of an inferred model needs a new model.
    void note_structure_change(std::string_view path, bool edited = false) {
        if (!model) return;
        const auto it = structures.find(base::path_key(path));
        if (it != structures.end()) {
            if (const auto* scan = index.scan_of(path); scan != nullptr && it->second != structure_of(*scan)) {
                schedule_replan(edited ? EDIT_SETTLE : REPLAN_DELAY);
            }
            return;
        }
        if (model->source == project::SourceKind::inferred && project::is_cxx_source_name(path) && base::is_within(path, root)) schedule_reload();
    }

    // A C++ source of the workspace the plan has no unit for is planned at once (robustness design C2); the core engine
    // holds it until then. A new source of an inferred model is in the model reloaded for it.
    void note_opened(std::string_view path) {
        if (!model || model->source == project::SourceKind::inferred) return;
        if (!project::is_cxx_source_name(path) || !base::is_within(path, root) || plannedFiles.contains(base::path_key(path))) return;
        const auto soon = Clock::now() + std::chrono::milliseconds { 100 };
        if (!replanAt || soon < *replanAt) replanAt = soon;
    }

    void document_event(engine::DocumentChange change, const Document& document, const Json* message) {
        const engine::DocumentEvent event { change, view_of(document), message };
        for (const auto& engine : engines) engine->document(event);
    }

    // ---- model and plan -----------------------------------------------------------

    // Design 4.1. The cache is not a warm index any more: it is a model, and a model can be used.
    // A session that has one whose inputs have not changed plans with it at once, and only then asks
    // the producer --- so the editor has a database in the time it takes to read a file, and the
    // build tool being slow, offline-blocked or hung is something the user never sees.
    //
    // The waits below are the whole of the "user waits for the build tool" budget: three seconds
    // when there is a cache whose fingerprint no longer matches (it is still a far better guess than
    // scanning), FIRST_MODEL_WAIT when there is none at all.
    void adopt_cached_model() {
        const auto providers = allowed_providers();
        const project::Detection detection { project::detect_project(root, options.database, providers, options.buildDiscovery != "off") };
        detectedSource = detection.kind;
        detectedManifest = detection.manifest;
        if (detection.kind == project::SourceKind::inferred) return;   // no producer: nothing to wait for
        // An untrusted workspace runs no build tool, and a model a trusted session once cached is
        // that build tool's word: scanned sources are what this workspace gets (design 4.1).
        if (!options.trusted) return;

        auto cached = project::load_model(cacheDirectory, detection.kind);
        // P-1 (plan 0.0.9): a second instance starts with no cache of its own; the owner's model is read, never written,
        // so it plans at once instead of from scanned sources (issue #37: 3-4.4 s of slow first requests and a clangd
        // restart when the build tool's model came). The owner writes it atomically.
        if (!cached && lease && lease->shared()) {
            cached = project::load_model(lease->workspace_directory(), detection.kind);
            if (cached) log::info("this instance plans with the model the instance owning {} cached", lease->workspace_directory());
        }
        if (!cached) {
            loadGiveUpAt = Clock::now() + FIRST_MODEL_WAIT;
            lastProducerMs = read_producer_timing(detection.kind);   // P-2
            return;
        }
        producerPath = cached->producer;
        producerVersion = cached->producerVersion;
        lastProducerMs = cached->producerMs;
        const std::string current { project::inputs_fingerprint(root, cached->model.watch, detection.manifest,
                                                                cached->producer, cached->producerVersion) };
        if (current == cached->fingerprint) {
            log::info("the cached {} model for {} matches its inputs; planning with it while the producer confirms it",
                      project::to_string(detection.kind), root);
            adopt_model(std::make_shared<project::ProjectModel>(std::move(cached->model)), "cache-fresh");
            return;
        }
        log::info("the cached {} model for {} is older than its inputs; the producer has three seconds before it is used anyway",
                  project::to_string(detection.kind), root);
        staleCache = std::move(cached);
        loadGiveUpAt = Clock::now() + std::chrono::seconds { 3 };
    }

    // The producer has not answered within its wait. Use what there is --- never nothing.
    void use_what_there_is() {
        if (model) return;
        if (staleCache) {
            auto cached = std::move(*staleCache);
            staleCache.reset();
            staleModelReason = std::format("{} has not answered yet; the model from the last session is in use and may be stale",
                                           project::to_string(cached.model.source));
            adopt_model(std::make_shared<project::ProjectModel>(std::move(cached.model)), "cache-stale");
            return;
        }
        start_inferred_load();
    }

    // Scanned sources, with no external program involved: what the server can always produce. It
    // runs beside the real load, which replaces it when it answers.
    void start_inferred_load() {
        if (inferredLoadStarted) return;
        inferredLoadStarted = true;
        log::info("no model for {} yet; planning from scanned sources until the producer answers", root);
        project::LoadOptions load;
        load.trusted = false;              // no external program: this must be fast and must always answer
        load.cacheDirectory = cacheDirectory;
        load.compilerOverride = compilerOverride;
        load.discoverCompilers = false;
        std::shared_ptr<const spec::Kit> kitCopy = kit ? std::make_shared<const spec::Kit>(*kit) : nullptr;
        const std::string rootCopy { root };
        auto queue = events;
        const std::string rootKey { key };
        const int generation { modelGeneration };
        std::thread { [queue, generation, load, kitCopy, rootCopy, rootKey]() mutable {
            load.kit = kitCopy.get();
            auto inferred = std::make_shared<project::ProjectModel>(project::load_project(rootCopy, load));
            // X-6 (plan 0.0.8 part 2): this load is untrusted only so that no program runs; it says nothing about the workspace.
            // Whether the workspace is trusted is reported by the orchestrator alone (update_status).
            std::erase_if(inferred->issues, [](const project::ModelIssue& issue) { return issue.code == "untrusted-workspace"; });
            queue->push(Event { EventKind::model_loaded, Json { { "origin", "inferred" } }, generation, std::move(inferred), rootKey });
        } }.detach();
    }

    // Everything about a model that the index and the plan are built from, as one comparable string.
    static std::string describe_model(const project::ProjectModel& candidate) {
        Json facts = Json::object();
        for (const auto& [driver, driverFacts] : candidate.facts) facts[driver] = Json::parse(toolchain::facts_to_json(driverFacts).dump());
        const Json description {
            { "source", std::string { project::to_string(candidate.source) } },
            { "level", candidate.level },
            { "usesKit", candidate.usesKit },
            { "profile", Json::array({ candidate.profile.kind, candidate.profile.compiler, candidate.profile.stdlib, candidate.profile.target }) },
            { "facts", std::move(facts) },
            { "database", Json::parse(spec::to_json(candidate.database).dump()) },
            // Issue #30: the plan builds the engine's arguments from derived options and from stated ones
            // differently, so two models that differ only here are not the same model.
            { "optionsDerived", Json::parse(project::derived_options_to_json(candidate.database).dump()) },
        };
        return description.dump();
    }

    // One file per source, so a model built from scanned sources can never replace what the build
    // tool said (design P5), and a fingerprint of the inputs, so the next session can tell whether
    // what it has is still current (design 4.1).
    // D-5 (plan 0.0.9): the outcome of a description run with the network because the person asked (S3-4-26). It fetched
    // what was needed when the build tool described the project and nothing is missing any more; otherwise what failed
    // is said, in the build tool's words where it gave some.
    void note_online_run(const project::ProjectModel& loaded) {
        const std::string tool { project::to_string(detectedSource) };
        const bool fetched { needsDownload.empty() && installFailed.empty() && loaded.source != project::SourceKind::inferred };
        std::string message;
        if (fetched) {
            message = std::format("{} fetched what the build description needed; the project is described by {} now", tool, tool);
        } else if (!installFailed.empty()) {
            message = installFailed;
        } else if (!needsDownload.empty()) {
            message = std::format("{} still needs a download after the run with the network: {}", tool, needsDownload);
        } else {
            message = std::format("{} did not describe the project with the network either; the log says why", tool);
            for (const auto& issue : loaded.issues) {
                if (!issue.message.empty()) {
                    message = issue.message;
                    break;
                }
            }
        }
        onlineRun = Json { { "outcome", fetched ? "fetched" : "failed" }, { "message", message },
                           { "at", std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now())) } };
        log::info("the description of {} with the network {}: {}", root, fetched ? "fetched what it needed" : "failed", message);
        journal.add("online-run", onlineRun);
    }

    // P-2 (plan 0.0.9): what clangd waits for the producer beyond FIRST_MODEL_WAIT (CORE_WAIT_CAP).
    std::chrono::milliseconds core_wait_limit() const {
        if (lastProducerMs <= 0) return CORE_WAIT_LIMIT;
        const std::chrono::milliseconds expected { lastProducerMs * 6 / 5 };
        return std::clamp<std::chrono::milliseconds>(expected - FIRST_MODEL_WAIT, CORE_WAIT_LIMIT, CORE_WAIT_CAP);
    }

    // P-2: how long each build tool last took to describe this project, kept apart from the model cache: it outlives a
    // cache that is gone or of another version, and a second instance reads the owner's (P-1).
    std::string producer_timing_path(std::string_view directory) const { return base::join_path(std::string { directory }, "producer-timing.json"); }

    std::int64_t read_producer_timing(project::SourceKind kind) const {
        std::vector<std::string> directories { cacheDirectory };
        if (lease && lease->shared()) directories.push_back(lease->workspace_directory());
        for (const auto& directory : directories) {
            const auto text = platform::fs::read_file(producer_timing_path(directory));
            if (!text) continue;
            const Json timing = Json::parse(*text, nullptr, false);
            if (timing.is_object() && timing.value(std::string { project::to_string(kind) }, std::int64_t { 0 }) > 0) {
                return timing.value(std::string { project::to_string(kind) }, std::int64_t { 0 });
            }
        }
        return 0;
    }

    void write_producer_timing(project::SourceKind kind, std::int64_t milliseconds) const {
        const std::string path { producer_timing_path(cacheDirectory) };
        const auto text = platform::fs::read_file(path);
        Json timing = text ? Json::parse(*text, nullptr, false) : Json::object();
        if (!timing.is_object()) timing = Json::object();
        timing[std::string { project::to_string(kind) }] = milliseconds;
        (void)platform::fs::write_file_atomic(path, timing.dump());
    }

    void save_model_cache() const {
        if (!model) return;
        project::CachedModel cached;
        cached.model = *model;
        cached.producer = producerPath;
        cached.producerVersion = producerVersion;
        cached.producerMs = lastProducerMs;
        cached.fingerprint = project::inputs_fingerprint(root, model->watch, detectedManifest, producerPath, producerVersion);
        cached.savedAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (auto saved = project::save_model(cacheDirectory, cached); !saved) {
            log::info("the {} model of {} could not be cached: {}", project::to_string(model->source), root, saved.error().message);
        }
    }

    void note_model_source(std::string origin) {
        modelOrigin = std::move(origin);
        if (firstModelOrigin.empty()) firstModelOrigin = modelOrigin;
        staleCache.reset();   // whatever it was waiting to be used instead of, that has happened
        journal.add("model-source", Json { { "origin", modelOrigin },
                                           { "source", model ? std::string { project::to_string(model->source) } : std::string {} },
                                           { "level", model ? model->level : 0 },
                                           { "stale", !staleModelReason.empty() },
                                           { "needsDownload", needsDownload } });
    }

    // S2 5: the inputs the model's producer named are watched like the build files are. The client
    // watches them when it registers watchers dynamically, as patterns under this root; otherwise the
    // polling worker reads the same entries. A new model replaces the previous registration.
    void register_model_watch() {
        if (!dynamicWatch || !model || !initializeAnswered) return;
        if (watchRegistration != 0) {
            Json unregister { { "unregisterations", Json::array({ Json { { "id", std::format("mcppls-model-watch:{}:{}", key, watchRegistration) },
                                                                        { "method", "workspace/didChangeWatchedFiles" } } }) } };
            client.send(lsp::make_request(std::format("w:{}:u{}", key, watchRegistration), "client/unregisterCapability", std::move(unregister)));
            watchRegistration = 0;
        }
        // An inferred model watches the sources the session's own watchers already cover.
        if (model->watch.empty() || model->source == project::SourceKind::inferred) return;
        const Json* relative { lsp::find_path(clientParams, { "capabilities", "workspace", "didChangeWatchedFiles", "relativePatternSupport" }) };
        const bool relativePatterns { relative != nullptr && relative->is_boolean() && relative->get<bool>() };
        Json watchers = Json::array();
        for (const auto& entry : model->watch) {
            const bool absolute { base::is_absolute_path(entry) };
            if (relativePatterns) {
                const std::string base { absolute ? base::parent_path(entry) : root };
                const std::string pattern { absolute ? std::string { base::file_name(entry) } : entry };
                watchers.push_back(Json { { "globPattern", Json { { "baseUri", base::path_to_uri(base) }, { "pattern", pattern } } } });
            } else {
                watchers.push_back(Json { { "globPattern", absolute ? base::normalize_path(entry) : base::join_path(root, entry) } });
            }
        }
        static int nextRegistration { 0 };
        watchRegistration = ++nextRegistration;
        Json registrations { { "registrations", Json::array({ Json { { "id", std::format("mcppls-model-watch:{}:{}", key, watchRegistration) },
                                                                    { "method", "workspace/didChangeWatchedFiles" },
                                                                    { "registerOptions", Json { { "watchers", std::move(watchers) } } } } }) } };
        client.send(lsp::make_request(std::format("w:{}:r{}", key, watchRegistration), "client/registerCapability", std::move(registrations)));
    }

    void start_model_load() {
        if (loading) {
            reloadAfterLoad = true;
            return;
        }
        loading = true;
        const int generation { ++modelGeneration };
        project::LoadOptions load;
        load.trusted = options.trusted;
        load.cacheDirectory = cacheDirectory;
        load.compilerOverride = compilerOverride;
        load.mcppExecutable = options.mcpp;
        load.configuredDatabase = options.database;
        load.discoverCompilers = options.discoverCompilers;
        // Plan 2026-09-27 B-7: whether the build system is detected at all, and by which providers.
        load.buildDiscovery = options.buildDiscovery != "off";
        load.providers = options.buildDiscoveryProviders;
        // Design 4.4: a run the server starts by itself is offline unless the user allowed the
        // network; `off` means the build tool is not run at all, and what is cached or scanned is
        // all there is. Design 4.2: the hard bound follows the last run (producer_deadline, G-4), ten
        // minutes when the user allowed the network and a download may be part of the answer.
        const bool online { options.buildTool == "online" || onlineOnce };
        describingOnline = onlineOnce && options.buildTool != "online";
        if (describingOnline) journal.add("describe-online");
        onlineOnce = false;
        load.offline = !online;
        loadRunsOffline = !online;
        load.runBuildTool = options.buildTool != "off";
        load.producerHard = options.producerTimeout.count() > 0 ? std::chrono::milliseconds { options.producerTimeout }
                            : online                             ? std::chrono::milliseconds { std::chrono::minutes { 10 } }
                                                                 : producer_deadline(lastProducerMs);
        load.producerSoft = std::chrono::seconds { 5 };
        // With a model already in hand nothing waits for the environment; without one, the producer
        // is worth the wait, because a producer found through the wrong PATH describes another build.
        load.environmentWait = model ? std::chrono::milliseconds { 0 } : std::chrono::milliseconds { std::chrono::seconds { 10 } };
        load.rootKey = key;
        producerSlowMs->store(0);
        loadStartedAt = Clock::now();
        load.onSlow = [slow = producerSlowMs](std::chrono::milliseconds elapsed) { slow->store(elapsed.count()); };
        producerSoftAt = Clock::now() + load.producerSoft + std::chrono::milliseconds { 200 };
        std::shared_ptr<const spec::Kit> kitCopy = kit ? std::make_shared<const spec::Kit>(*kit) : nullptr;
        const std::string rootCopy { root };
        const std::string probeCachePath { base::join_path(platform::dirs::cache_directory(), "toolchains/probe.json") };
        auto queue = events;
        const std::string rootKey { key };
        std::thread { [queue, generation, load, kitCopy, rootCopy, probeCachePath, rootKey]() mutable {
            toolchain::ProbeCache cache { probeCachePath };
            load.kit = kitCopy.get();
            load.runner = toolchain::process_runner(std::chrono::seconds { 20 });
            load.probeCache = &cache;
            auto loadedModel = std::make_shared<project::ProjectModel>(project::load_project(rootCopy, load));
            queue->push(Event { EventKind::model_loaded, {}, generation, std::move(loadedModel), rootKey });
        } }.detach();
        update_status();
    }

    // X-5 (plan 0.0.8 part 2): a database the user's own tool is writing is invalid for a moment (E1: mcppls read the half of
    // a compile_commands.json and warned). When the reload found a database invalid and a file it reads was written within
    // the last two seconds, the reload is tried again in a second, up to five times, with no warning and no stale issue meanwhile:
    // the last model stays. Only a database that stays invalid is reported, as before.
    bool wait_for_file_being_written(const project::ProjectModel& loaded) {
        using namespace std::chrono_literals;
        const auto invalid = std::ranges::find_if(loaded.issues, [](const project::ModelIssue& issue) {
            return issue.code == "compdb-invalid" || issue.code == "database-invalid";
        });
        if (invalid == loaded.issues.end()) {
            invalidFileRetries = 0;
            return false;
        }
        if (!model) return false;   // nothing to keep meanwhile: the failure is the answer
        // The files a reload reads that are the user's: the ones the failing model still names (an explicit database), what
        // the last model watched (a CMake build directory's or the xmake fallback's compile_commands.json) and, when the error
        // names it, the file itself. A failed load names none of its own in the xmake case, hence the last model's.
        std::vector<std::string> candidates;
        for (const std::vector<std::string>* watch : { &loaded.watch, static_cast<const std::vector<std::string>*>(&model->watch) }) {
            for (const auto& entry : *watch) {
                if (base::is_absolute_path(entry) && entry.ends_with(".json")) candidates.push_back(entry);
            }
        }
        if (const std::size_t end { invalid->message.find(" is not valid JSON") }; end != std::string::npos) candidates.push_back(invalid->message.substr(0, end));
        // "A moment ago" is generous: the load that found the file cut short may itself have waited seconds for the event loop
        // or the watch to settle -- on a loaded machine 2 s missed a writer that finished 2.5 s after it started.
        const auto now { std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::file_clock::now().time_since_epoch()).count() };
        const bool written { std::ranges::any_of(candidates, [&](const std::string& path) {
            const auto stamp { platform::fs::stamp(path) };
            return stamp && now - stamp->modified < std::chrono::nanoseconds { 10s }.count();
        }) };
        if (!written || invalidFileRetries >= 5) {
            invalidFileRetries = 0;
            return false;
        }
        ++invalidFileRetries;
        log::info("a database of {} is invalid and was written a moment ago; reading it again in a second ({} of 5)", root, invalidFileRetries);
        journal.add("model-reread", Json { { "attempt", invalidFileRetries } });
        reloadAfterLoad = false;   // the read in a second includes whatever asked for this one
        reloadAt = Clock::now() + 1s;
        reloadMovable = false;
        update_status();
        return true;
    }

    void handle_model_loaded(std::shared_ptr<project::ProjectModel> loadedModel, bool fromProducer = true) {
        if (!fromProducer) {
            // The scanned-sources model that stands in until the producer answers (design 4.1). The
            // producer is still running, so nothing about its state is touched here, and a model that
            // arrived meanwhile wins: this one is never an upgrade.
            if (model) return;
            adopt_model(std::move(loadedModel), "inferred");
            return;
        }
        loading = false;
        loadGiveUpAt.reset();
        producerElapsed.reset();
        // R-4 (plan 0.0.8): the build tool answered, whatever it said, so the provisional model is provisional no more: a
        // model it keeps (it failed, or described nothing better) is planned again, and the project's modules are prepared.
        if (model && modelOrigin == "inferred") replanAt = Clock::now();
        // G-4: what this project's producer takes is what its next deadline is made of.
        if (loadStartedAt && loadedModel->source != project::SourceKind::inferred) {
            lastProducerMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - *loadStartedAt).count();
            write_producer_timing(loadedModel->source, lastProducerMs);   // P-2
        }
        loadStartedAt.reset();
        // Fix plan F4: the build tool answered, whatever it said; clangd waits no longer. A model kept below
        // (a failed or poorer reload) is planned again for it.
        if (coreWaitUntil && !coreWaitOver) {
            coreWaitOver = true;
            if (model) replanAt = Clock::now();
        }
        ++snapshotGeneration;
        needsDownload.clear();
        installFailed.clear();
        const bool askedOnline { describingOnline };
        describingOnline = false;
        for (const auto& issue : loadedModel->issues) {
            if (issue.code == spec::NEEDS_DOWNLOAD) needsDownload = issue.message;
            if (issue.code == spec::INSTALL_FAILED) installFailed = issue.message;
        }
        if (askedOnline) note_online_run(*loadedModel);
        needsDownloadFromOfflineRun = !needsDownload.empty() && loadRunsOffline;
        if (needsDownload.empty()) {
            downloadRetries = 0;
            downloadRetryAt.reset();
        } else {
            static constexpr std::array<std::chrono::seconds, 4> BACKOFF { std::chrono::seconds { 30 }, std::chrono::seconds { 60 },
                                                                          std::chrono::seconds { 120 }, std::chrono::seconds { 300 } };
            downloadRetryAt = Clock::now() + BACKOFF[std::min(downloadRetries, BACKOFF.size() - 1)];
            ++downloadRetries;
        }
        if (wait_for_file_being_written(*loadedModel)) return;
        // S2 5-9: a producer that answered before and fails now (or answers with nothing) leaves the last
        // model in place, and the status says it may be stale, rather than the project falling back to
        // scanned sources. A project that is no longer that kind of project takes the new model.
        //
        // Design 4.1, P5: this is also what protects the cache. The model in hand may have come from
        // the cache rather than from a producer of this session --- that is the whole point of starting
        // from it --- and a failed producer must not replace it, nor its cache file, with scanned sources.
        const bool fellBack { loadedModel->detected != project::SourceKind::inferred && loadedModel->source == project::SourceKind::inferred };
        if (model && model->source != project::SourceKind::inferred && model->source == loadedModel->detected && fellBack) {
            const std::string why { loadedModel->issues.empty() ? std::string {} : std::format(" ({})", loadedModel->issues.front().message) };
            staleModelReason = std::format("{} could not describe the project again{}; the last model that loaded is kept and may be stale",
                                           project::to_string(model->source), why);
            for (const auto& issue : loadedModel->issues) log::warning("model reload failed ({}): [{}] {}", root, issue.code, issue.message);
            journal.add("model-stale", Json { { "reason", staleModelReason } });
            // Design 4.1: try again in five minutes, or sooner if a build file changes (which sets
            // this to a second and a half). Nothing else would ever ask again.
            reloadAt = Clock::now() + std::chrono::minutes { 5 };
            reloadMovable = true;
            if (reloadAfterLoad) {
                reloadAfterLoad = false;
                start_model_load();
            }
            update_status();
            return;
        }
        // Design 4.1: a worse source never replaces a better result. A producer that now describes the
        // project less completely than the model in hand -- the build database a cache holds against the
        // compile database an older mcpp answers with -- leaves that model in place, kept the same way
        // as a failed reload: marked possibly stale and asked again later.
        if (model && model->detected == loadedModel->detected && loadedModel->tier > model->tier) {
            staleModelReason = std::format("{} now describes the project less completely (L{} instead of L{}); the last model is kept and may be stale",
                                           project::to_string(loadedModel->source), loadedModel->tier, model->tier);
            log::warning("model reload ({}): {}", root, staleModelReason);
            journal.add("model-kept", Json { { "reason", staleModelReason }, { "kept", model->tier }, { "offered", loadedModel->tier } });
            reloadAt = Clock::now() + std::chrono::minutes { 5 };
            reloadMovable = true;
            if (reloadAfterLoad) {
                reloadAfterLoad = false;
                start_model_load();
            }
            update_status();
            return;
        }
        staleModelReason.clear();
        adopt_model(std::move(loadedModel), "producer");
    }

    // Everything that makes a model this workspace's model: the watch registration, the index, the
    // plan, the cache. One path, whether the model came from the producer, from the cache, or from
    // scanned sources; `origin` is only recorded.
    void adopt_model(std::shared_ptr<project::ProjectModel> loadedModel, std::string origin) {
        if (!loadedModel->producer.empty()) {
            producerPath = loadedModel->producer;
            producerVersion = loadedModel->producerVersion;
        }
        const bool watchChanged { !model || model->watch != loadedModel->watch };
        std::string description { describe_model(*loadedModel) };
        const bool unchanged { model && description == modelDescription };
        model = std::move(loadedModel);
        if (watchChanged) {
            {
                const std::lock_guard lock { watchPatterns->mutex };
                watchPatterns->entries = model->watch;
                ++watchPatterns->generation;
            }
            register_model_watch();
        }
        if (unchanged) {
            // The usual answer to a saved source the producer watches: the same project. The index
            // already has the file and the plan already has the graph, so the work of a new model is skipped.
            // When it is the producer confirming what the cache said, the cache is only re-stamped.
            log::info("project model ({}) loaded again: unchanged", root);
            journal.add("model-unchanged");
            if (origin == "producer") {
                // The producer says what the cache said. Nothing to replan; the cache is re-stamped
                // so the next session finds its fingerprint current, and the origin records that
                // this model has now been confirmed rather than merely restored.
                note_model_source(modelOrigin.starts_with("cache") ? std::string { "cache-confirmed" } : std::move(origin));
                save_model_cache();
            }
            for (const auto& issue : model->issues) log::info("model issue [{}] {}", issue.code, issue.message);
            if (reloadAfterLoad) {
                reloadAfterLoad = false;
                start_model_load();
            }
            update_status();
            return;
        }
        note_model_source(std::move(origin));
        modelDescription = std::move(description);
        log::info("project model ({}): source {}, level {}, {} sets, profile {} {} {}", root, project::to_string(model->source), model->level,
                  model->database.sets.size(), model->profile.kind, model->profile.compiler, model->profile.stdlib);
        for (const auto& issue : model->issues) log::info("model issue [{}] {}", issue.code, issue.message);
        {
            Json issues = Json::array();
            for (const auto& issue : model->issues) issues.push_back(Json { { "code", issue.code }, { "message", issue.message } });
            journal.add("model-loaded", Json { { "source", std::string { project::to_string(model->source) } }, { "level", model->level },
                                               { "sets", model->database.sets.size() }, { "profile", profile_json() }, { "issues", std::move(issues) } });
        }

        if (modelOrigin == "producer") save_model_cache();
        index.clear();
        for (const auto& set : model->database.sets) {
            for (const auto& unit : set.units) {
                const std::string path { spec::absolute_source(unit) };
                if (index.contains(path)) continue;
                if (const Document* document = documents_.find_by_path(path)) {
                    index.update(path, document->text);
                } else if (auto text = platform::fs::read_file(path)) {
                    index.update(path, *text);
                }
            }
        }
        for (const Document* document : documents_.all()) {
            if (!document->path.empty()) index.update(document->path, document->text);
        }
        std::vector<std::pair<std::string, std::string>> manifests;
        for (auto& manifest : project::module_manifests(*model, kit ? &*kit : nullptr)) manifests.emplace_back(manifest.path, manifest.origin);
        auto external = index::external_modules(manifests, metadataReader);
        index.set_external(std::move(external));
        const auto& profile = model->profile;
        index.set_profile_label(profile.kind == "semantic-kit" ? std::format("{} (semantic kit, {})", profile.stdlib, profile.target)
                                                               : std::format("{} · {} ({})", profile.compiler, profile.stdlib, profile.target));
        replan();
        if (reloadAfterLoad) {
            reloadAfterLoad = false;
            start_model_load();
        }
    }

    void replan() {
        replanAt.reset();
        if (!model) return;
        ++snapshotGeneration;
        normalize::PlanInput input;
        input.database = &model->database;
        input.contextSet = contextSet;
        input.facts = &model->facts;
        input.kit = kit ? &*kit : nullptr;
        input.macosSdk = macosSdk;
        input.scanner = [this](std::string_view path) {
            if (const auto* scan = index.scan_of(path)) return *scan;
            auto text = platform::fs::read_file(path);
            return text ? project::scan_source(*text) : project::ScanResult {};
        };
        input.metadataReader = metadataReader;
        std::set<std::string> openSources;
        for (const auto& path : openedOutsideModel) {
            if (platform::fs::is_regular_file(path)) openSources.insert(path);
        }
        for (const Document* document : documents_.all()) {
            if (!document->path.empty() && project::is_cxx_source_name(document->path) && base::is_within(document->path, root)) openSources.insert(document->path);
        }
        input.openSources.assign(openSources.begin(), openSources.end());
        const auto now = Clock::now();
        std::optional<Clock::time_point> lastEdit;
        for (const Document* document : documents_.all()) {
            if (document->path.empty()) continue;
            const auto edited = editedAt.find(base::path_key(document->path));
            if (edited == editedAt.end() || now - edited->second >= EDITING_WINDOW) continue;
            input.editingSources.push_back(document->path);
            // Fix plan F13: what an autosave already put on disk is what clangd builds with.
            if (auto disk = platform::fs::read_file(document->path)) input.editingDiskImports[document->path] = project::required_names(project::scan_source(*disk));
            if (!lastEdit || edited->second > *lastEdit) lastEdit = edited->second;
        }
        input.modelOrigin = modelOrigin;
        if (coreEngine != nullptr) coreEngine->configure_plan(input);
        normalize::EnginePlan newPlan { normalize::plan_engine(input) };
        // A stand-in held back for a file being edited is planned once the file has been quiet for EDITING_WINDOW.
        if (newPlan.standInsDeferred && lastEdit) {
            const auto quiet = *lastEdit + EDITING_WINDOW + std::chrono::milliseconds { 100 };
            if (!replanAt || quiet < *replanAt) replanAt = quiet;
        }
        plan = std::move(newPlan);
        openedOutsideModel = { plan.openSources.begin(), plan.openSources.end() };
        plannedFiles.clear();
        for (const auto& entry : plan.entries) plannedFiles.insert(base::path_key(entry.file));
        for (const auto& file : plan.excludedFiles) plannedFiles.insert(base::path_key(file));
        structures.clear();
        for (const auto& path : index.files()) {
            if (const auto* scan = index.scan_of(path)) structures[base::path_key(path)] = structure_of(*scan);
        }
        firstPlanWritten = true;
        // Fix plan F14: what the person chose; a change of it restarts clangd without counting against it.
        plan.toolchainKey = std::format("{}|{}|{}|{}|{}", model->profile.kind, model->profile.compiler, model->profile.stdlib, model->profile.target, contextSet);
        plan.modelOrigin = modelOrigin;
        plan.provisional = model_is_provisional();
        journal.add("plan", Json { { "context", contextSet.empty() ? std::string { "default" } : contextSet }, { "entries", plan.entries.size() },
                                   { "stdUnits", plan.stdUnits }, { "standIns", plan.stubModules }, { "openSources", plan.openSources },
                                   { "leftOut", plan.excludedFiles.size() },
                                   { "issues", plan.issues.size() }, { "languageStandard", plan.languageStandard },
                                   { "standardsRaised", plan.standardsRaised }, { "standardAssumed", plan.standardAssumed } });
        if (plan.standardAssumed && !loggedStandardAssumed) {
            loggedStandardAssumed = true;
            log::info("the build of {} names no C++ standard for its module units; they are read as {}, the standard `import std` is for "
                      "(a standard the build sets is followed instead)", root, plan.languageStandard);
        }
        if (plan.standardsRaised > 0 && loggedStandards != plan.standardsSeen) {
            loggedStandards = plan.standardsSeen;
            std::string seen;
            for (const auto& each : plan.standardsSeen) seen += (seen.empty() ? "" : ", ") + each;
            log::info("the module units of {} name {}; {} of them are read as {}, since every module they import must be built with one standard",
                      root, seen, plan.standardsRaised, plan.languageStandard);
        }
        const bool coreWaits { core_waits_for_producer() };
        if (coreWaits && !coreWaitUntil) {
            coreWaitUntil = Clock::now() + core_wait_limit();
            log::info("clangd waits for {} to describe {} (at most {} ms more); mcppls's own engine answers meanwhile", project::to_string(detectedSource), root,
                      core_wait_limit().count());
            journal.add("engine-waits-for-producer", Json { { "detected", std::string { project::to_string(detectedSource) } } });
        }
        for (const auto& engine : engines) {
            if (coreWaits && engine.get() == coreEngine) continue;
            engine->apply(&plan);
        }
        for (const Document* document : documents_.all()) publish_diagnostics(document->uri);
        update_status();
    }

    static constexpr std::chrono::milliseconds REPLAN_DELAY { 800 };
    void schedule_replan(std::chrono::milliseconds delay = REPLAN_DELAY) { replanAt = Clock::now() + delay; }

    // mcppls.buildDiscovery.providers (plan 2026-09-27 B-7): the registry, less the providers the setting leaves out.
    std::vector<project::BuildSystemProvider*> allowed_providers() const {
        std::vector<project::BuildSystemProvider*> allowed;
        for (project::BuildSystemProvider* provider : project::registered_providers()) {
            if (std::ranges::find(options.buildDiscoveryProviders, std::string { provider->id() }) != options.buildDiscoveryProviders.end()) {
                allowed.push_back(provider);
            }
        }
        return allowed;
    }

    // mcppls.buildDiscovery.askBeforeDownload (plan 2026-09-27 B-7).
    bool ask_before_download() const { return options.buildDiscoveryAskBeforeDownload; }

    bool core_waits_for_producer() const { return coreEngine != nullptr && model && !coreWaitOver && model_is_provisional(); }
    // R-4 (plan 0.0.8): the model in hand is the provisional one, the sources read with the kit's commands, and the
    // project's build tool is still expected to describe it.
    bool model_is_provisional() const {
        return modelOrigin == "inferred" && loading && detectedSource != project::SourceKind::inferred && options.trusted && options.buildTool != "off";
    }
    // `fromEdits`: asked for by changes under the workspace rather than by a build description. Such a reload waits as
    // long as the producer takes to answer (at most a minute), so edits that keep coming are one run, not a queue of them.
    // A reload a build description asked for is not put off by edits that follow it: it reads them too.
    void schedule_reload(bool fromEdits = false) {
        using namespace std::chrono_literals;
        if (fromEdits && reloadAt && !reloadMovable) return;
        const auto wait = fromEdits ? std::clamp<std::chrono::milliseconds>(std::chrono::milliseconds { lastProducerMs }, 1500ms, 60s) : 1500ms;
        reloadAt = Clock::now() + wait;
        reloadMovable = fromEdits;
    }

    // K-7: the issues nothing here recovers from by itself.
    static constexpr std::array<std::string_view, 5> UNRECOVERABLE_ISSUES { "engine-crash-loop", "engine-start-failed", "engine-incompatible",
                                                                           "payload-corrupt", "preparation-stalled" };

    // K-7: the first time one of them appears, its bundle is asked for, whatever the client: the log says where it is too.
    void request_auto_bundles() {
        if (!autoBundleRequest) return;
        for (const auto& engine : engines) {
            for (const auto& issue : engine->status().issues) {
                if (std::ranges::find(UNRECOVERABLE_ISSUES, issue.code) == UNRECOVERABLE_ISSUES.end()) continue;
                if (!autoBundles.contains(issue.code) && autoBundleRequested.insert(issue.code).second) autoBundleRequest(issue.code);
            }
        }
    }

    // K-7: and the status issue carries the bundle once it is written.
    void attach_auto_bundles(Json& issues) const {
        for (auto& issue : issues) {
            const std::string code { issue.value("code", std::string {}) };
            if (std::ranges::find(UNRECOVERABLE_ISSUES, code) == UNRECOVERABLE_ISSUES.end()) continue;
            if (const auto written = autoBundles.find(code); written != autoBundles.end()) issue["bundle"] = written->second;
        }
    }

    // G-5: whether a source's module structure on disk (or in the editor) is other than what the model says of its unit.
    // A source the model has no unit for is new to it; a unit that states neither what it provides nor what it imports
    // is compared with the last plan's view of it.
    bool model_structure_differs(std::string_view path, const project::ScanResult& disk) const {
        const auto* scan = &disk;
        if (!model) return true;
        const std::string key { base::path_key(path) };
        for (const auto& set : model->database.sets) {
            for (const auto& unit : set.units) {
                if (base::path_key(spec::absolute_source(unit)) != key) continue;
                if (unit.providedModules.empty() && unit.requiredModules.empty()) {
                    const auto known = structures.find(key);
                    return known == structures.end() || known->second != structure_of(*scan);
                }
                const std::string provided { unit.providedModules.empty() ? std::string {} : unit.providedModules.front().first };
                std::vector<std::string> modelRequires { unit.requiredModules };
                std::vector<std::string> scannedRequires { project::required_names(*scan) };
                // An implementation unit's own module (`module m;` requires m) is implied, whether or not a producer lists it.
                if (scan->declaration) {
                    const std::string& own { scan->declaration->module };
                    std::erase(modelRequires, own);
                    std::erase(scannedRequires, own);
                }
                std::ranges::sort(modelRequires);
                std::ranges::sort(scannedRequires);
                modelRequires.erase(std::ranges::unique(modelRequires).begin(), modelRequires.end());
                scannedRequires.erase(std::ranges::unique(scannedRequires).begin(), scannedRequires.end());
                return provided != project::provided_name(*scan) || modelRequires != scannedRequires;
            }
        }
        return true;
    }

    // ---- diagnostics and status -------------------------------------------------------

    void publish_diagnostics(std::string_view uri, bool force = false) {
        const Document* document { documents_.find(uri) };
        if (document == nullptr) {
            // A review's findings reach files nobody opened.
            const auto review = reviewDiagnostics.find(uri);
            if (!force && review == reviewDiagnostics.end()) return;
            client.notify("textDocument/publishDiagnostics",
                          Json { { "uri", std::string { uri } }, { "diagnostics", review == reviewDiagnostics.end() ? Json::array() : review->second } });
            return;
        }
        const Json moduleDiagnostics = document->path.empty() ? Json::array() : index.diagnostics(document->path);
        Json fromEngines = Json::array();
        for (const auto& [engineId, byUri] : engineDiagnostics) {
            const auto entry = byUri.find(uri);
            if (entry == byUri.end() || !entry->second.is_array()) continue;
            for (const auto& diagnostic : entry->second) fromEngines.push_back(diagnostic);
        }
        std::string label;
        if (model) label = model->profile.kind == "semantic-kit" ? model->profile.stdlib + " kit" : model->profile.compiler;
        Json merged = merge_diagnostics(fromEngines, moduleDiagnostics, label);
        if (const auto review = reviewDiagnostics.find(uri); review != reviewDiagnostics.end()) {
            for (const auto& diagnostic : review->second) merged.push_back(diagnostic);
        }
        std::string serialized { lsp::dump(merged) };
        auto& previous = publishedDiagnostics[std::string { uri }];
        if (!force && previous == serialized) return;
        previous = std::move(serialized);
        client.notify("textDocument/publishDiagnostics", Json { { "uri", std::string { uri } }, { "diagnostics", std::move(merged) } });
    }

    Json profile_json() const {
        Json profile = Json::object();
        if (model) {
            profile["kind"] = model->profile.kind;
            if (!model->profile.compiler.empty()) profile["compiler"] = model->profile.compiler;
            profile["stdlib"] = model->profile.stdlib;
            profile["target"] = model->profile.target;
            // C++26 alignment: the standard the context's module units are read with (S3 SemanticProfile.standard).
            if (!plan.languageStandard.empty()) profile["standard"] = plan.languageStandard;
        } else if (kit) {
            profile = Json { { "kind", "semantic-kit" }, { "stdlib", std::format("{} {}", kit->stdlibName, kit->stdlibVersion) }, { "target", kit->target } };
        } else {
            profile = Json { { "kind", "semantic-kit" }, { "stdlib", "unknown" }, { "target", std::string { mcppls::os::PLATFORM } } };
        }
        return profile;
    }

    // K-6: whether what makes the state degraded is only what passes by itself (PASSING_ISSUES).
    bool degraded_only_in_passing() const {
        if ((model && !model->issues.empty()) || !staleModelReason.empty() || (kit && spec::requires_macos_sdk(*kit) && macosSdk.empty())) return false;
        if (std::ranges::any_of(plan.issues, [](const auto& issue) { return issue.category != "code"; })) return false;
        bool passing { false };
        for (const auto& engine : engines) {
            for (const auto& issue : engine->status().issues) {
                if (issue.category == "code") continue;
                if (std::ranges::find(PASSING_ISSUES, issue.code) == PASSING_ISSUES.end()) return false;
                passing = true;
            }
        }
        return passing;
    }

    State compute_state() const {
        const std::optional<engine::EngineStatus> core { coreEngine != nullptr ? std::optional { coreEngine->status() } : std::nullopt };
        // usable plan W9.4: a corrupt payload is not merely degraded: only syntax-level features are
        // trustworthy, the same as three engine crashes in a row.
        if (core && core->failed) return State::error;
        if (!model) return loading ? State::loading : State::starting;
        if (loading) return State::loading;
        if (core && core->preparing) return State::preparing;
        // usable plan W5.4: degraded regardless of whether any open file happens to need std yet.
        if (kit && spec::requires_macos_sdk(*kit) && macosSdk.empty()) return State::degraded;
        // import-hang plan §6: a problem in the user's own code is told as a diagnostic where it is, never as a
        // server that lost a feature; only the other categories make the state degraded.
        const auto notCode = [](const auto& issue) { return issue.category != "code"; };
        bool engineIssues { false };
        for (const auto& engine : engines) engineIssues = engineIssues || std::ranges::any_of(engine->status().issues, notCode);
        const bool planIssues { std::ranges::any_of(plan.issues, notCode) };
        // S2 5: a kept model the producer could not confirm may be stale: said, not hidden in a ready state.
        if (engineIssues || !model->issues.empty() || planIssues || !staleModelReason.empty()) return State::degraded;
        return State::ready;
    }

    // ---- standard LSP progress (usable plan; cold-start plan 4.1) ----------------------------
    //
    // `cxxModules/status` carries far more (level, profile, engines, issues), but only this
    // repository's VS Code extension understands it — and that extension renders it in a
    // LanguageStatusItem, behind the `{}` icon. Zed, nvim, Helix and every other client showed
    // nothing at all, which is why a cold start looked like a hang rather than like work.
    //
    // `$/progress` is what all of them already render in their status bar, so one notification
    // reaches every editor. The two are not redundant: this one says "something is happening and
    // how far along", the other says what was found.
    bool progressSupported() const {
        const Json* supported { lsp::find_path(clientParams, { "capabilities", "window", "workDoneProgress" }) };
        return supported != nullptr && supported->is_boolean() && supported->get<bool>();
    }

    // ---- one C++ engine per file (cold-start plan 4.11) ----------------------------------------
    //
    // LSP gives a server no way to see its siblings: mcppls cannot detect that the editor is also
    // running clangd, or cpptools, over the same files. Detection is therefore the editor's job,
    // and this is the part that is not — saying so, once, in the editor's own terms.
    //
    // A client that does its own detection says `conflictArbitration: "client"` in
    // initializationOptions and is never told anything. This repository's VS Code extension does;
    // Zed, nvim, Helix and the rest do not, and for them a single message beats a paragraph in a
    // README nobody reaches until something is already wrong.
    //
    // The distinction that matters, and the one people get wrong: mcppls DRIVES clangd. Seeing
    // `mcppls` and `clangd` both at work is not a conflict — it is one engine and its child. A
    // second, editor-started clangd is.
    void say_one_engine_per_file_once() {
        if (conflictAdviceSent) return;
        conflictAdviceSent = true;
        const Json* options { lsp::find(clientParams, "initializationOptions") };
        if (options != nullptr && options->is_object()
            && options->value("conflictArbitration", std::string {}) == "client") {
            return;
        }
        const Json* info { lsp::find(clientParams, "clientInfo") };
        const std::string editor { info != nullptr && info->is_object() ? info->value("name", std::string {}) : std::string {} };

        std::string advice { "mcppls runs clangd itself with the module database it built, so another "
                             "C or C++ language server over the same files means two engines answering." };
        if (editor == "Zed") {
            advice += " In Zed's settings.json: \"languages\": {\"C++\": {\"language_servers\": [\"mcppls\", \"!clangd\"]}, "
                      "\"C\": {\"language_servers\": [\"mcppls\", \"!clangd\"]}}";
        } else {
            advice += " Put mcppls first and disable the editor's own clangd for C and C++.";
        }
        client.notify("window/showMessage", Json { { "type", 3 }, { "message", advice } });   // 3 = Info
    }

    void update_progress(State state, std::optional<engine::EngineStatus> core) {
        if (!progressSupported()) return;
        const bool busy { state == State::starting || state == State::loading || state == State::preparing };

        std::string message;
        std::optional<unsigned> percentage;
        if (state == State::loading || state == State::starting) {
            // The 30 seconds before any module is built: asking the build tool what the project is.
            // Nothing else reported this, so it read as a freeze.
            message = producerPath.empty() ? "reading the project" : "asking the build tool about the project";
        } else if (state == State::preparing) {
            if (core && core->toPrepare > 0) {
                message = std::format("preparing modules {}/{}", core->prepared, core->toPrepare);
                percentage = static_cast<unsigned>(core->prepared * 100 / core->toPrepare);
            } else {
                message = "preparing modules";
            }
        }

        if (busy && !progressToken.empty() && message == lastProgressMessage) return;

        if (busy && progressToken.empty()) {
            progressToken = std::format("mcppls/{}", key);
            // A request, but its answer is not needed: the token's work begins with the first
            // $/progress either way, and a client that refuses simply ignores what follows.
            client.send(lsp::make_request(std::format("w:{}:p", key), "window/workDoneProgress/create",
                                          Json { { "token", progressToken } }));
            Json begin { { "kind", "begin" }, { "title", "mcppls" }, { "message", message }, { "cancellable", false } };
            if (percentage) begin["percentage"] = *percentage;
            client.notify("$/progress", Json { { "token", progressToken }, { "value", std::move(begin) } });
            lastProgressMessage = std::move(message);
            return;
        }
        if (busy) {
            Json report { { "kind", "report" }, { "message", message } };
            if (percentage) report["percentage"] = *percentage;
            client.notify("$/progress", Json { { "token", progressToken }, { "value", std::move(report) } });
            lastProgressMessage = std::move(message);
            return;
        }
        if (!progressToken.empty()) {
            client.notify("$/progress", Json { { "token", progressToken },
                                               { "value", Json { { "kind", "end" } } } });
            progressToken.clear();
            lastProgressMessage.clear();
        }
    }

    void update_status() {
        if (!initializeAnswered) return;   // see the field's own comment
        request_auto_bundles();
        const State computed { compute_state() };
        State state { computed };
        if (state == State::degraded && lastReportedState != State::degraded) {
            const auto now = Clock::now();
            if (!degradedSince) degradedSince = now;
            const auto hold = degraded_only_in_passing() ? PASSING_DEGRADED_HOLD : DEGRADED_HOLD;
            if (now < *degradedSince + hold) {
                if (!statusFlushAt || *degradedSince + hold < *statusFlushAt) statusFlushAt = *degradedSince + hold;
                return;
            }
        }
        if (state != State::degraded) degradedSince.reset();
        // K-6 (plan 0.0.8): only the state is held -- the issues and the engines still go out as they are. "Settled" is
        // a ready or degraded state while the core engine serves: the ready of a workspace whose clangd is still starting
        // is not one (failure-at-base: preparation right after it was held, and the failure it found with it).
        if (state == State::preparing && settled && (lastReportedState == State::ready || lastReportedState == State::degraded)) {
            const auto now = Clock::now();
            if (!preparingSince) preparingSince = now;
            if (now < *preparingSince + SETTLED_PREPARING_HOLD) {
                if (!statusFlushAt || *preparingSince + SETTLED_PREPARING_HOLD < *statusFlushAt) statusFlushAt = *preparingSince + SETTLED_PREPARING_HOLD;
                state = lastReportedState;
            }
        }
        if (computed != State::preparing) preparingSince.reset();
        if ((computed == State::ready || computed == State::degraded) && (coreEngine == nullptr || coreEngine->status().accepting)) settled = true;
        lastReportedState = state;
        // Before the gate below, not after it. `clientSupportsStatus` means the client understands
        // this repository's own `cxxModules/status` — which is its VS Code extension and nothing
        // else. Every client this progress exists for (Zed, nvim, Helix, …) fails that test, so
        // sending progress after the gate sends it to the only client that already had it.
        update_progress(state, coreEngine != nullptr ? std::optional { coreEngine->status() } : std::nullopt);
        if (state == State::ready || state == State::degraded) say_one_engine_per_file_once();
        if (!clientSupportsStatus) return;
        Json issues = Json::array();
        auto add = [&](std::string_view code, std::string_view message, std::string_view command, std::string_view title = "Fix",
                       std::string_view category = "project") {
            if (issues.size() >= 20) return;
            Json issue { { "code", std::string { code } }, { "message", std::string { message } }, { "category", std::string { category } } };
            if (!command.empty()) issue["command"] = Json { { "title", std::string { title } }, { "command", std::string { command } } };
            issues.push_back(std::move(issue));
        };
        for (const auto& engine : engines) {
            for (const auto& issue : engine->status().issues) add(issue.code, issue.message, issue.command, command_title(issue.command), issue.category);
        }
        if (!staleModelReason.empty()) {
            // Decision 8: no version table, no comparison --- one sentence that points at the one
            // thing the user can do about a build tool that hangs, fails or cannot go on.
            add("model-stale", std::format("{}. It may also be an older build tool: updating it is worth trying", staleModelReason),
                "mcppls.showLogs", "Show Logs");
        }
        if (!needsDownload.empty()) {
            // The producer ran offline (design P3) and cannot go on without fetching something. The
            // model in hand stays; the user decides, in their own terminal, where their proxy and
            // credentials are (1.2: those live in the terminal session and nothing here can reach them).
            add("producer-needs-download",
                std::format("the build description needs a download: {}. The project is served from its sources meanwhile; "
                            "fetch it here, or run the build tool in your terminal (the description is read again when that is done). "
                            "It may also be an older build tool: updating it is worth trying",
                            needsDownload),
                "mcppls.runBuildToolInTerminal", "Run in Terminal", "environment");
            // Plan 2026-09-27 B-2 (S3): a client that knows `askOnline` may offer, once and without blocking anything
            // (§9.2), to fetch it through mcppls.describeOnline; one that does not keeps the terminal action above.
            if (!issues.empty() && issues.back().value("code", std::string {}) == "producer-needs-download") {
                // D-2: only a load that was offline has anything to repeat online.
                issues.back()["askOnline"] = ask_before_download() && !describingOnline && needsDownloadFromOfflineRun;
            }
        }
        if (!installFailed.empty()) {
            // D-2: the network was allowed and the install failed; the reason is the build tool's own, there is nothing to ask for.
            add(std::string { spec::INSTALL_FAILED},
                std::format("the build description could not be made: {}. The project is served from its sources meanwhile; "
                            "run the build tool in your terminal to see it in full (the description is read again when that is done)",
                            installFailed),
                "mcppls.runBuildToolInTerminal", "Run in Terminal", "environment");
        }
        if (describingOnline && loading) {
            add("producer-online", std::format("fetching what the build description of {} needs; the project is served from its sources meanwhile",
                                               base::file_name(root)),
                "mcppls.showLogs", "Show Logs", "environment");
        }
        if (producerElapsed) {
            add("producer-slow", std::format("reading the build description ({}, {} s)",
                                             project::to_string(detectedSource), producerElapsed->count() / 1000),
                "mcppls.showLogs", "Show Logs");
        }
        if (model) {
            for (const auto& issue : model->issues) {
                // Needing a download is reported above, with what to do about it; the load's own
                // issue says the same thing with nothing to do, and saying it twice helps nobody.
                if (issue.code == spec::NEEDS_DOWNLOAD || issue.code == spec::INSTALL_FAILED) continue;
                // Plan 2026-09-27 Q1-3: what only a build makes is made by building; the model is loaded again when it is.
                if (issue.code == "generated-files-missing") {
                    add(issue.code, issue.message, "mcppls.runBuildToolInTerminal", "Build in Terminal", "environment");
                    continue;
                }
                add(issue.code, issue.message, "mcppls.showLogs");
            }
        }
        for (const auto& issue : plan.issues) {
            // sdk-missing is reported once below, workspace-wide, with the fix command (W5.4).
            if (issue.code == "sdk-missing") continue;
            add(issue.code, std::format("{} ({})", issue.message, base::file_name(issue.file)), "", "Fix", issue.category);
        }
        if (!options.trusted) add("untrusted-workspace", "the workspace is not trusted: build tools and compilers are not run", "", "Fix", "environment");
        if (kit && spec::requires_macos_sdk(*kit) && macosSdk.empty()) {
            add("sdk-missing", "the macOS SDK was not found; install the Command Line Tools", "mcppls.installCommandLineTools", "Install Command Line Tools",
                "environment");
        }
        Json notices = Json::array();
        if (model) {
            for (const auto& notice : model->notices) notices.push_back(Json { { "code", notice.code }, { "message", notice.message } });
        }
        if (!payload.kitNotice.empty()) notices.push_back(Json { { "code", "kit-version-mismatch" }, { "message", payload.kitNotice } });
        if (lease && lease->shared()) {
            notices.push_back(Json { { "code", "shared-workspace" },
                                     { "message", "another mcppls instance serves this workspace; this one keeps a private cache and starts cold" } });
        }
        // S3 4 (overall design 5.2): every engine serving the root, with its role and state.
        Json engineList = Json::array();
        for (const auto& engine : engines) {
            const engine::EngineStatus engineStatus { engine->status() };
            engineList.push_back(Json { { "name", engineStatus.name }, { "version", engineStatus.version }, { "role", engineStatus.role },
                                        { "state", engineStatus.state } });
            for (const auto& notice : engineStatus.notices) notices.push_back(Json { { "code", notice.code }, { "message", notice.message } });
        }
        // usable plan W9.1: `project.root` is this root's own path, so a multi-root session's several
        // notifications are told apart by it.
        Json project { { "root", clientUri.empty() ? base::path_to_uri(root) : clientUri },
                       { "source", model ? std::string { project::to_string(model->source) } : std::string { "inferred" } } };
        if (model) project["level"] = model->level;
        if (model) project["tier"] = model->tier;
        const std::optional<engine::EngineStatus> core { coreEngine != nullptr ? std::optional { coreEngine->status() } : std::nullopt };
        Json params {
            { "state", std::string { to_string(state) } },
            { "project", project },
            { "profile", profile_json() },
            { "engine", core ? Json { { "name", core->name }, { "version", core->version } } : Json { { "name", "none" }, { "version", "" } } },
            { "engines", std::move(engineList) },
            { "issues", issues },
        };
        if (!notices.empty()) params["notices"] = std::move(notices);
        if (!onlineRun.is_null()) params["onlineRun"] = onlineRun;   // D-5, S3-4-26
        if (core && core->toPrepare > 0) params["progress"] = Json { { "done", core->prepared }, { "total", core->toPrepare } };
        if (const Json cache = cache_fragment_(); cache.is_object()) params["cache"] = std::move(cache);
        attach_auto_bundles(params["issues"]);
        std::string serialized { lsp::dump(params) };
        if (serialized == lastStatus) {
            statusFlushAt.reset();
            return;
        }
        // S3 4: a new state goes out at once; other changes within STATUS_COALESCE of the last
        // notification (module counts while many modules build) go out together when it has passed.
        const auto now = Clock::now();
        if (lastStatusSentAt && state == lastSentState && now - *lastStatusSentAt < STATUS_COALESCE) {
            if (!statusFlushAt) statusFlushAt = *lastStatusSentAt + STATUS_COALESCE;
            return;
        }
        statusFlushAt.reset();
        lastStatus = std::move(serialized);
        lastSentState = state;
        lastStatusSentAt = now;
        client.notify("cxxModules/status", std::move(params));
    }

    // ---- timers -----------------------------------------------------------------------

    void handle_timers() {
        for (const auto& engine : engines) {
            const auto before = Clock::now();
            engine->handle_timers();
            // Plan 2026-09-30 §13: which engine held the event loop (the session says that it was held).
            if (const auto took = Clock::now() - before; took >= std::chrono::milliseconds { 250 }) {
                journal.add("timers-slow", Json { { "engine", std::string { engine->id() } },
                                                  { "ms", std::chrono::duration_cast<std::chrono::milliseconds>(took).count() } });
            }
        }
        const auto now = Clock::now();
        {
            std::vector<std::uint64_t> due;
            for (const auto& [id, job] : jobs) {
                if (job.budgetAt && *job.budgetAt <= now) due.push_back(id);
            }
            for (const auto id : due) {
                if (jobs.contains(id)) answer_without_core(id);
            }
            for (auto it = lateCompletions.begin(); it != lateCompletions.end();) {
                if (it->second.until > now) {
                    ++it;
                    continue;
                }
                if (coreEngine != nullptr) coreEngine->cancel(it->second.clientId);
                it = lateCompletions.erase(it);
            }
        }
        if (leaseRenewAt && *leaseRenewAt <= now) {
            lease->renew(std::chrono::system_clock::now());
            // C-9: the tick's cheap part -- stat the few `instance.json` files and rename the dead
            // directories aside; their removal (however large) runs in the background, so the tick
            // stays at milliseconds. A live guest's own heartbeat protects it here, owner or not.
            const auto heartbeat { std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()) };
            if (cache::rename_dead_instances(workspaceDirectory_, heartbeat, ownToken_) > 0) start_cache_task_("tick", platform::fs::modified_now());
            leaseRenewAt = now + LEASE_RENEWAL;
        }
        if (reloadAt && *reloadAt <= now) {
            reloadAt.reset();
            start_model_load();
        }
        if (downloadRetryAt && *downloadRetryAt <= now) {
            downloadRetryAt.reset();
            log::info("asking {} again, offline, whether it can describe {} now", project::to_string(detectedSource), root);
            start_model_load();
        }
        if (replanAt && *replanAt <= now) replan();
        if (coreWaitUntil && !coreWaitOver && *coreWaitUntil <= now) {
            coreWaitOver = true;
            if (modelOrigin == "inferred" && model) {
                log::info("{} has not described {} yet; clangd starts with the model scanned from its sources, and the build tool's replaces it when it comes",
                          project::to_string(detectedSource), root);
                journal.add("engine-wait-over", Json { { "milliseconds", (FIRST_MODEL_WAIT + core_wait_limit()).count() } });
                replan();
            }
        }
        if (statusFlushAt && *statusFlushAt <= now) {
            statusFlushAt.reset();
            update_status();
        }
        if (tokensRefreshAt && *tokensRefreshAt <= now) {
            tokensRefreshAt.reset();
            // A request, not a notification (LSP 3.16): its answer carries nothing, and an id the session cannot
            // parse as an engine's is dropped, like the watcher registrations' answers.
            client.send(lsp::make_request(std::format("w:{}:t{}", key, ++tokensRefreshes), lsp::method::WORKSPACE_SEMANTIC_TOKENS_REFRESH, nullptr));
        }
        if (sdkCheckAt && *sdkCheckAt <= now) {
            sdkCheckAt.reset();
            if (kit && spec::requires_macos_sdk(*kit) && macosSdk.empty()) {
                if (std::string found { engine::macos_sdk_path() }; !found.empty()) {
                    log::info("the macOS SDK appeared at {}; re-probing and refreshing the model ({})", found, root);
                    macosSdk = found;
                    start_model_load();   // re-probes toolchains and replans; no restart of the server itself
                } else {
                    sdkCheckAt = now + std::chrono::seconds { 30 };
                }
            }
        }
        if (producerSoftAt && *producerSoftAt <= now) {
            producerSoftAt.reset();
            const std::int64_t elapsed { producerSlowMs->load() };
            if (loading && elapsed > 0) {
                producerElapsed = std::chrono::milliseconds { elapsed };
                producerSoftAt = now + std::chrono::seconds { 2 };
                update_status();
            } else if (loading) {
                producerSoftAt = now + std::chrono::seconds { 2 };
            }
        }
        if (loadGiveUpAt && *loadGiveUpAt <= now) {
            loadGiveUpAt.reset();
            use_what_there_is();
        }
        if (lastResortAt && *lastResortAt <= now) {
            lastResortAt.reset();
            if (!firstPlanWritten) {
                log::warning("the project model ({}) is still loading after two minutes; serving without it", root);
                firstPlanWritten = true;
                for (const auto& engine : engines) engine->apply(nullptr);
            }
        }
    }
};

// ---- Workspace --------------------------------------------------------------------------

Workspace::Workspace(std::string root, std::string key, SessionOptions options, engine::PayloadPaths payload, bool payloadCorrupt, bool kitEnabled,
                     std::string compilerOverride, std::shared_ptr<EventChannel> events, ClientSink& client)
    : root_ { root }, key_ { key },
      impl_ { std::make_unique<Impl>(std::move(root), std::move(key), std::move(options), std::move(payload), payloadCorrupt, kitEnabled,
                                    std::move(compilerOverride), std::move(events), client) } {}

Workspace::~Workspace() = default;

void Workspace::set_client_uri(std::string uri) { impl_->clientUri = std::move(uri); }

bool Workspace::owns_path(std::string_view path) const { return !path.empty() && base::is_within(path, root_); }

void Workspace::start(Json clientParams, bool clientSupportsStatus, bool usePolling, std::function<void(Json)> onEngineSettled) {
    impl_->clientParams = std::move(clientParams);
    impl_->clientSupportsStatus = clientSupportsStatus;
    // Semantic tokens (design doc 2026-09-25 K/§7): workspace.semanticTokens.refreshSupport.
    if (const Json* supported = lsp::find_path(impl_->clientParams, { "capabilities", "workspace", "semanticTokens", "refreshSupport" });
        supported != nullptr && supported->is_boolean()) {
        impl_->clientSupportsTokensRefresh = supported->get<bool>();
    }
    impl_->onEngineSettled = std::move(onEngineSettled);
    impl_->vscodeLike = completion::vscode_like(impl_->clientParams);
    impl_->dynamicWatch = !usePolling;
    if (usePolling) impl_->start_watch_polling();
    log::info("mcppls {} ({}) root {}", base::VERSION, mcppls::os::FAMILY_NAME, root_);
    if (!impl_->payloadCorrupt && impl_->kitEnabled && !impl_->payload.kit.empty()) {
        if (auto kit = spec::load_kit(impl_->payload.kit)) {
            impl_->kit = std::move(*kit);
            if (spec::requires_macos_sdk(*impl_->kit)) {
                impl_->macosSdk = engine::macos_sdk_path();
                // usable plan W5.4 / U7: keep looking every 30s so installing the Command Line Tools
                // while the server runs is picked up without a restart.
                if (impl_->macosSdk.empty()) impl_->sdkCheckAt = Clock::now() + std::chrono::seconds { 30 };
            }
            log::info("semantic kit {} at {} ({})", impl_->kit->name, impl_->kit->root, root_);
        } else {
            log::warning("semantic kit unusable ({}): {}", root_, kit.error().message);
        }
    }
    for (const auto& engine : impl_->engines) engine->start(*impl_);
    // Without a core engine nothing else settles initialize.
    if (impl_->coreEngine == nullptr) impl_->engine_settled("", Json::object());
    // Only now: adopting the cached model plans with it at once, and a plan handed to an engine
    // that has not started yet would be applied before its handshake.
    impl_->adopt_cached_model();
    impl_->start_model_load();
    // Design D6: clangd always gets a database. The wait for the producer was set by
    // adopt_cached_model --- three seconds beside a stale cache, ten with none --- and what happens
    // when it expires is that scanned sources are planned, not that the engine is told to serve
    // without a database. This last resort stays for the case where even that produced nothing.
    impl_->lastResortAt = Clock::now() + std::chrono::seconds { 120 };
}

void Workspace::set_auto_bundle_request(std::function<void(std::string)> request) { impl_->autoBundleRequest = std::move(request); }

void Workspace::note_auto_bundle(const std::string& code, const std::string& path) {
    impl_->autoBundles[code] = path;
    impl_->journal.add("auto-bundle", Json { { "code", code }, { "path", path } });
    impl_->update_status();
}

void Workspace::allow_status_notifications() {
    if (impl_->initializeAnswered) return;
    impl_->initializeAnswered = true;
    impl_->register_model_watch();
    impl_->update_status();
}

void Workspace::shut_down() {
    if (impl_->reviewProcess) impl_->reviewProcess->kill();
    for (const auto& engine : impl_->engines) engine->shut_down();
    if (impl_->lease) impl_->lease->release();
}

void Workspace::did_open(const Json& params) {
    const Json* item { lsp::find(params, "textDocument") };
    if (item == nullptr) return;
    const std::string uri { item->value("uri", std::string {}) };
    const std::string path { impl_->path_of_uri(uri) };
    ++impl_->snapshotGeneration;
    const Document& document = impl_->documents_.open(uri, path, item->value("languageId", std::string { "cpp" }),
                                                      item->value("version", std::int64_t { 0 }), item->value("text", std::string {}));
    if (!path.empty()) {
        impl_->index.update(path, document.text);
        impl_->note_structure_change(path);
        impl_->note_opened(path);
    }
    impl_->publish_diagnostics(uri);
    impl_->document_event(engine::DocumentChange::opened, document, nullptr);
}

void Workspace::did_change(const Json& message, const Json& params) {
    const std::string uri { uri_of_params(params) };
    const std::int64_t version { lsp::find_path(params, { "textDocument", "version" }) != nullptr
                                     ? params["textDocument"].value("version", std::int64_t { 0 }) : 0 };
    if (!impl_->documents_.change(uri, version, params.value("contentChanges", Json::array()))) return;
    ++impl_->snapshotGeneration;
    ++impl_->documentChanges;
    impl_->lastDocumentChangeAt = std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now()));
    const Document* document { impl_->documents_.find(uri) };
    if (!document->path.empty()) {
        impl_->editedAt[base::path_key(document->path)] = Clock::now();
        impl_->index.update(document->path, document->text);
        impl_->note_structure_change(document->path, true);
    }
    impl_->publish_diagnostics(uri);
    impl_->document_event(engine::DocumentChange::changed, *document, &message);
}

void Workspace::did_close(const Json& message, const Json& params) {
    const std::string uri { uri_of_params(params) };
    const Document* found { impl_->documents_.find(uri) };
    if (found == nullptr) return;
    const Document document { *found };
    impl_->documents_.close(uri);
    if (!document.path.empty()) impl_->editedAt.erase(base::path_key(document.path));
    ++impl_->snapshotGeneration;
    if (!document.path.empty()) {
        if (auto text = platform::fs::read_file(document.path)) impl_->index.update(document.path, *text);
    }
    impl_->document_event(engine::DocumentChange::closed, document, &message);
    // Diagnostics of a closed file are cleared; an engine may send its own empty set too.
    impl_->publishedDiagnostics.erase(uri);
    impl_->coreDiagnosticsVersions.erase(uri);
    for (auto& [engineId, byUri] : impl_->engineDiagnostics) byUri.erase(uri);
    impl_->client.notify("textDocument/publishDiagnostics", Json { { "uri", uri }, { "diagnostics", Json::array() } });
    if (impl_->reviewDiagnostics.contains(uri)) impl_->publish_diagnostics(uri);
    impl_->update_status();
}

void Workspace::did_save(const Json& message, const Json& params) {
    const std::string uri { uri_of_params(params) };
    const std::string path { impl_->path_of_uri(uri) };
    Document saved;
    if (const Document* document = impl_->documents_.find(uri)) {
        saved = *document;
    } else {
        saved.uri = uri;
        saved.path = path;
    }
    impl_->document_event(engine::DocumentChange::saved, saved, &message);
    // Fix plan F13: an import of a file being edited that the save just put on disk gets its stand-in now, not once the
    // file is quiet (clangd builds with what is on disk).
    if (!saved.path.empty()) {
        const auto edited = impl_->editedAt.find(base::path_key(saved.path));
        if (edited != impl_->editedAt.end() && Clock::now() - edited->second < Impl::EDITING_WINDOW) impl_->schedule_replan();
    }
}

void Workspace::handle_watched_files(const Json& changes) {
    ++impl_->snapshotGeneration;
    bool reload { false };
    bool describedAgain { false };   // a build file or another input of the producer's, not a source: reloaded without the edit delay
    bool replan { false };
    for (const auto& change : changes) {
        const std::string path { impl_->path_of_uri(change.value("uri", std::string {})) };
        if (path.empty()) continue;
        const std::string_view name { base::file_name(path) };
        const int type { change.value("type", 2) };
        // S2 5: an input the producer named changes what it would answer, so the model is loaded again.
        const bool producerInput { impl_->model && impl_->model->source != project::SourceKind::inferred && name != "compile_commands.json"
                                   && matches_watch_entries(impl_->model->watch, impl_->root, path) };
        if (is_build_file(name)) {
            // mcpp rewrites its own compile_commands.json while the model loads.
            if (name == "compile_commands.json" && impl_->model && impl_->model->source == project::SourceKind::mcpp) continue;
            reload = describedAgain = true;
            continue;
        }
        if (!project::is_cxx_source_name(path)) {
            if (producerInput) reload = describedAgain = true;   // an input the producer named that is not a source: what it reads changed
            continue;
        }
        const bool open { impl_->documents_.find_by_path(path) != nullptr };
        std::optional<std::string> diskText;
        if (type != 3) {
            if (auto text = platform::fs::read_file(path)) diskText = std::move(*text);
        }
        if (!open) {
            if (type == 3) impl_->index.remove(path);
            else if (diskText) impl_->index.update(path, *diskText);
        }
        // G-5 (plan 2026-09-30): a producer names every source as an input (mcpp: src/**/*.cpp and the rest), yet what
        // it answers changes only with the set of sources and with a source's module structure -- the module it
        // provides and those it imports. Every save used to run it again, and with autosave that was once for every
        // pause in typing: on GalTranslPP a minute of CPU each time, beside clangd. An edit inside functions leaves
        // the model as it is; the index and the plan already have it.
        // What is on disk is what the producer reads: an open file changed under the editor (a checkout) counts too.
        if (producerInput && (type != 2 || !diskText || impl_->model_structure_differs(path, project::scan_source(*diskText)))) reload = true;
        if (open) continue;
        if (impl_->model && impl_->model->source == project::SourceKind::inferred && type != 2) reload = true;
        else replan = true;
    }
    if (reload || replan) {
        for (const auto& engine : impl_->engines) engine->sources_changed();
    }
    if (reload) impl_->schedule_reload(!describedAgain);
    else if (replan) impl_->schedule_replan();
    const Json message = lsp::make_notification("workspace/didChangeWatchedFiles", Json { { "changes", changes } });
    for (const auto& engine : impl_->engines) engine->notify(message);
    for (const Document* document : impl_->documents_.all()) impl_->publish_diagnostics(document->uri);
}

void Workspace::cancel(const Json& id) {
    for (const auto& engine : impl_->engines) engine->cancel(id);
}

void Workspace::route_client_request(const Json& message) { impl_->route_client_request(message); }

void Workspace::handle_client_response(const EngineRequestKey& key, const Json& response) {
    for (const auto& engine : impl_->engines) {
        if (engine->id() == key.engineId) engine->client_response(key.generation, key.engineRequestId, response);
    }
}

void Workspace::forward_other_notification(const Json& message) {
    for (const auto& engine : impl_->engines) engine->notify(message);
}

Json Workspace::graph() const { return impl_->index.graph(); }

Json Workspace::module_info(std::string_view name) const { return impl_->index.module_info(name); }

Json Workspace::module_info_at(std::string_view path, base::Position at) const {
    const auto hit = impl_->index.module_at(path, at);
    return hit ? impl_->index.module_info(hit->name) : Json(nullptr);
}

Json Workspace::contexts() const {
    Json available = Json::array();
    available.push_back(Json { { "id", "default" }, { "label", "All targets" }, { "profile", impl_->profile_json() } });
    if (impl_->model) {
        for (const auto& set : impl_->model->database.sets) {
            std::string label { set.name };
            if (!set.kind.empty() && set.kind != "other") label += std::format(" ({})", set.kind);
            available.push_back(Json { { "id", set.name }, { "label", label }, { "profile", impl_->profile_json() } });
        }
    }
    return Json { { "current", impl_->contextSet.empty() ? std::string { "default" } : impl_->contextSet }, { "available", available } };
}

Json Workspace::report() const {
    const auto& impl = *impl_;
    Json project = nullptr;
    if (impl.model) {
        Json issues = Json::array();
        for (const auto& issue : impl.model->issues) issues.push_back(Json { { "code", issue.code }, { "message", issue.message } });
        Json toolchains = Json::array();
        for (const auto& [id, facts] : impl.model->facts) toolchains.push_back(id);
        Json notices = Json::array();
        for (const auto& notice : impl.model->notices) notices.push_back(Json { { "code", notice.code }, { "message", notice.message } });
        project = Json { { "source", std::string { project::to_string(impl.model->source) } }, { "level", impl.model->level },
                         { "tier", impl.model->tier },
                         { "sets", impl.model->database.sets.size() }, { "toolchains", std::move(toolchains) }, { "profile", impl.profile_json() },
                         { "issues", std::move(issues) }, { "notices", std::move(notices) }, { "staleReason", impl.staleModelReason } };
        // Design 4.6: where this model came from, how the build tool is run, and what the last runs
        // of it cost --- the questions a report of "it has the wrong arguments" always ends at.
        project["origin"] = impl.modelOrigin;
        project["firstOrigin"] = impl.firstModelOrigin;
        project["detected"] = std::string { project::to_string(impl.detectedSource) };
        project["producer"] = impl.producerPath;
        project["producerVersion"] = impl.producerVersion;
        project["buildTool"] = impl.options.buildTool;
        if (!impl.needsDownload.empty()) project["needsDownload"] = impl.needsDownload;
        if (impl.staleCache) project["cacheAwaitingProducer"] = true;
    }
    // The environment build tools are started in: where it came from, and the NAMES of the variables
    // that differ from this process's. Never the values --- a proxy variable can carry a credential.
    const auto toolEnvironment = platform::toolenv::get();
    Json environment { { "source", toolEnvironment.source }, { "resolved", toolEnvironment.resolved },
                       { "durationMs", toolEnvironment.duration.count() }, { "differing", toolEnvironment.differing },
                       { "mode", std::string { platform::toolenv::mode_name(platform::toolenv::mode()) } } };
    if (!toolEnvironment.reason.empty()) environment["reason"] = toolEnvironment.reason;
    // The one run a report about a wrong or missing build description is almost always about. It is
    // looked for over everything kept, not over the twenty listed: a load that probes a dozen
    // compilers afterwards would otherwise push the producer out of sight.
    Json lastProducerRun = nullptr;
    const auto kept = platform::toolrun::recent(platform::toolrun::KEPT);
    for (const auto& record : kept) {
        if (!record.root.empty() && record.root != key_) continue;
        if (record.purpose == "producer" || record.purpose.starts_with("configure")) lastProducerRun = platform::toolrun::to_json(record);
    }
    if (project.is_object()) project["producerRun"] = std::move(lastProducerRun);
    Json toolRuns = Json::array();
    for (const auto& record : kept | std::views::drop(kept.size() > 20 ? kept.size() - 20 : 0)) {
        if (!record.root.empty() && record.root != key_) continue;
        toolRuns.push_back(platform::toolrun::to_json(record));
    }
    Json leftOut = Json::array();
    for (const auto& file : impl.plan.excludedFiles) {
        if (leftOut.size() >= 100) break;
        leftOut.push_back(file);
    }
    Json planIssues = Json::array();
    for (const auto& issue : impl.plan.issues) {
        if (planIssues.size() >= 100) break;
        planIssues.push_back(Json { { "code", issue.code }, { "message", issue.message }, { "file", issue.file }, { "module", issue.module } });
    }
    Json plan { { "context", impl.contextSet.empty() ? std::string { "default" } : impl.contextSet }, { "entries", impl.plan.entries.size() },
                { "languageStandard", impl.plan.languageStandard }, { "standardsSeen", impl.plan.standardsSeen }, { "standardsRaised", impl.plan.standardsRaised },
                { "standardAssumed", impl.plan.standardAssumed },
                { "stdUnits", impl.plan.stdUnits }, { "standIns", impl.plan.stubModules }, { "openSources", impl.plan.openSources },
                { "leftOutCount", impl.plan.excludedFiles.size() },
                { "leftOut", std::move(leftOut) }, { "issueCount", impl.plan.issues.size() }, { "issues", std::move(planIssues) } };
    Json engines = Json::array();
    for (const auto& engine : impl.engines) {
        const engine::EngineStatus status { engine->status() };
        Json issues = Json::array();
        for (const auto& issue : status.issues) issues.push_back(Json { { "code", issue.code }, { "message", issue.message } });
        Json entry { { "name", status.name }, { "version", status.version }, { "role", status.role }, { "state", status.state },
                     { "accepting", status.accepting }, { "prepared", status.prepared }, { "toPrepare", status.toPrepare }, { "issues", std::move(issues) } };
        entry["details"] = engine->report();
        engines.push_back(std::move(entry));
    }
    Json requests = Json::object();
    for (const auto& [method, stats] : impl.requestStats) {
        std::vector<double> durations { stats.recentMs.begin(), stats.recentMs.end() };
        std::ranges::sort(durations);
        const auto percentile = [&](double fraction) -> std::int64_t {
            if (durations.empty()) return 0;
            const std::size_t index { std::min(durations.size() - 1, static_cast<std::size_t>(fraction * static_cast<double>(durations.size()))) };
            return static_cast<std::int64_t>(durations[index]);
        };
        Json answeredBy = Json::object();
        for (const auto& [engineId, count] : stats.answeredBy) answeredBy[engineId] = count;
        Json entry { { "count", stats.count }, { "empty", stats.empty }, { "errors", stats.errors }, { "cancelled", stats.cancelled },
                     { "p50Ms", percentile(0.5) }, { "p95Ms", percentile(0.95) }, { "maxMs", static_cast<std::int64_t>(stats.maxMs) },
                     { "answeredBy", std::move(answeredBy) }, { "lastAt", stats.lastAt } };
        // M-1 (plan 0.0.9): where the time of the requests an engine answered went, in the engine and outside it.
        if (!stats.recentEngineMs.empty()) {
            std::vector<double> engineMs, overheadMs;
            for (const auto& [engine, overhead] : stats.recentEngineMs) {
                engineMs.push_back(engine);
                overheadMs.push_back(overhead);
            }
            std::ranges::sort(engineMs);
            std::ranges::sort(overheadMs);
            const auto at = [](const std::vector<double>& sorted, double fraction) {
                return static_cast<std::int64_t>(sorted[std::min(sorted.size() - 1, static_cast<std::size_t>(fraction * static_cast<double>(sorted.size())))]);
            };
            entry["engineAnswered"] = engineMs.size();
            entry["engineP50Ms"] = at(engineMs, 0.5);
            entry["engineP95Ms"] = at(engineMs, 0.95);
            entry["overheadP50Ms"] = at(overheadMs, 0.5);
            entry["overheadP95Ms"] = at(overheadMs, 0.95);
        }
        requests[method] = std::move(entry);
    }
    // C-4 (plan 0.0.8 part 2): the ten files whose slowest method is slowest at the 95th percentile.
    std::vector<std::pair<std::int64_t, Json>> files;
    for (const auto& [path, stats] : impl.fileRequestStats) {
        Json methods = Json::object();
        std::int64_t worst { 0 };
        for (const auto& [method, recent] : stats.recentMs) {
            std::vector<double> durations { recent.begin(), recent.end() };
            std::ranges::sort(durations);
            const std::int64_t p95 { durations.empty() ? 0 : static_cast<std::int64_t>(durations[std::min(durations.size() - 1, durations.size() * 95 / 100)]) };
            methods[method] = Json { { "count", durations.size() }, { "p95Ms", p95 } };
            worst = std::max(worst, p95);
        }
        files.emplace_back(worst, Json { { "file", path }, { "count", stats.count }, { "maxMs", static_cast<std::int64_t>(stats.maxMs) },
                                         { "completionsWithoutCore", stats.completionsWithoutCore }, { "methods", std::move(methods) } });
    }
    std::ranges::sort(files, std::greater {}, [](const auto& entry) { return entry.first; });
    Json slowestFiles = Json::array();
    for (auto& [worst, entry] : files | std::views::take(10)) slowestFiles.push_back(std::move(entry));
    // F9, F15: what space-triggered completion cost, and how often keywords answered without the core engine.
    Json completionCosts { { "spaceTrigger", Json { { "count", impl.spaceTrigger.count }, { "passed", impl.spaceTrigger.passed },
                                                     { "maxMicros", impl.spaceTrigger.maxMicros }, { "totalMicros", impl.spaceTrigger.totalMicros } } },
                           { "keywordsWithoutEngine", impl.keywordsWithoutEngine }, { "wordsWithoutCore", impl.wordsWithoutCore },
                           { "late", Json { { "arrived", impl.lateCompletionsArrived }, { "served", impl.lateCompletionsServed } } } };
    return Json { { "root", root_ }, { "key", key_ }, { "cacheDirectory", impl.cacheDirectory },
                  { "trusted", impl.options.trusted }, { "state", std::string { to_string(impl.compute_state()) } }, { "project", std::move(project) },
                  { "toolEnvironment", std::move(environment) }, { "toolRuns", std::move(toolRuns) },
                  { "plan", std::move(plan) }, { "engines", std::move(engines) }, { "requests", std::move(requests) },
                  { "slowestFiles", std::move(slowestFiles) },
                  { "completion", std::move(completionCosts) },
                  { "documents", Json { { "changes", impl.documentChanges }, { "lastChangeAt", impl.lastDocumentChangeAt } } },
                  { "eventTotals", impl.journal.totals() }, { "events", impl.journal.recent(300) } };
}

std::uint64_t Workspace::reset_cache() {
    auto& impl = *impl_;
    std::uint64_t freed { 0 };
    for (const auto& engine : impl.engines) freed += engine->clear_cache_on_request();
    for (const auto& entry : platform::fs::list_directory(impl.cacheDirectory)) {
        const std::string_view name { base::file_name(entry) };
        // C-9: the instance's self-description is reset with everything else -- the next heartbeat
        // tick writes it anew; leaving it would make this instance look gone to a reaper.
        if (name != "instance.json" && (!name.starts_with("model.") || !name.ends_with(".json"))) continue;
        if (const auto stamp = platform::fs::stamp(entry)) freed += stamp->size;
        platform::fs::remove_all(entry);
    }
    log::info("the cache of {} was reset on request ({} bytes freed)", root_, freed);
    impl.journal.add("cache-reset", Json { { "bytes", freed } });
    {
        std::lock_guard lock(impl.cacheMutex_);
        impl.cacheReportAt_.reset();   // the numbers the old cache held are gone with it
    }
    // The model in hand is planned into the empty directories at once, so clangd starts on a database; the build
    // tool is asked again as well, and its answer is cached anew.
    if (impl.model) impl.replan();
    if (impl.coreEngine != nullptr) (void)impl.coreEngine->restart_on_request();
    impl.start_model_load();
    impl.update_status();
    return freed;
}

// ---- the cache mcppls owns (0.0.10 plan C-13.1) ----------------------------------------------

void Workspace::handle_cache_swept(const Json& outcome) {
    auto& impl = *impl_;
    const std::uint64_t bytes { outcome.value("bytes", std::uint64_t { 0 }) };
    const std::size_t files { outcome.value("files", std::size_t { 0 }) };
    const std::size_t instances { outcome.value("instances", std::size_t { 0 }) };
    const std::size_t failed { outcome.value("failed", std::size_t { 0 }) };
    {
        std::lock_guard lock(impl.cacheMutex_);
        impl.cacheSnapshot_ = outcome.value("report", Json::object());
        impl.cacheReport_ = impl.cacheSnapshot_;
        impl.cacheReportAt_ = Clock::now();
        if (bytes > 0 || files > 0 || instances > 0 || failed > 0) {
            impl.lastSweepAt_ = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            impl.lastSweepFreed_ = bytes;
            impl.lastSweepFiles_ = files;
            impl.lastSweepFailed_ = failed;
        }
    }
    impl.journal.add("cache-swept", Json { { "bytes", bytes }, { "files", files }, { "instances", instances },
                                           { "failed", failed }, { "origin", outcome.value("origin", std::string {}) } });
    impl.update_status();
}

Json Workspace::cache_report() const {
    auto& impl = *impl_;
    Json numbers;
    {
        std::lock_guard lock(impl.cacheMutex_);
        // At most 30 s old: a hub opening twice in a minute does not walk the same tree twice
        // (C-13.1: the server is the only source, and it answers from its cache).
        if (!impl.cacheReportAt_ || Clock::now() - *impl.cacheReportAt_ > std::chrono::seconds { 30 }) {
            impl.cacheReport_ = cache::report(impl.cacheDirectory, impl.cache_budget(), std::chrono::system_clock::now(), impl.ownToken_);
            impl.cacheReportAt_ = Clock::now();
        }
        numbers = impl.cacheReport_;
    }
    Json project { { "name", std::string { base::file_name(root_) } },
                   { "source", impl.model ? std::string { project::to_string(impl.model->source) } : std::string { "inferred" } } };
    if (impl.model) {
        project["level"] = impl.model->level;
        project["tier"] = impl.model->tier;
    }
    Json engines = Json::array();
    for (const auto& engine : impl.engines) {
        const engine::EngineStatus status { engine->status() };
        engines.push_back(Json { { "name", status.name }, { "version", status.version }, { "role", status.role }, { "state", status.state } });
    }
    const std::optional<engine::EngineStatus> core { impl.coreEngine != nullptr ? std::optional { impl.coreEngine->status() } : std::nullopt };
    const normalize::EnginePlan& plan { impl.plan };
    Json envelope { { "state", std::string { to_string(impl.compute_state()) } },
                    { "project", std::move(project) },
                    { "plan", Json { { "units", plan.entries.size() }, { "modules", plan.modules.size() } } },
                    { "engines", std::move(engines) },
                    { "profile", impl.profile_json() },
                    { "bytes", numbers.value("bytes", std::uint64_t { 0 }) },
                    { "canonical", numbers.value("canonical", Json::object()) },
                    { "copies", numbers.value("copies", Json::object()) },
                    { "trash", numbers.value("trash", Json::object()) },
                    { "instances", numbers.value("instances", Json::object()) },
                    { "largest", numbers.value("largest", Json::array()) },
                    { "limits", numbers.value("limits", Json::object()) },
                    { "contexts", numbers.value("contexts", Json::array()) },
                    { "paths", Json { { "cacheRoot", impl.workspaceDirectory_ }, { "logDirectory", base::parent_path(log::file_path()) } } },
                    { "cli", Json { { "cacheQuery", "mcppls cache --format json" }, { "sweep", "mcppls cache --prune --dry-run" } } },
                    { "prompts", Json { { "agent", cache::agent_prompt(impl.cache_prompt_facts_()) },
                                        { "issue", cache::issue_prompt(impl.cache_prompt_facts_()) } } } };
    if (core && core->toPrepare > 0) envelope["progress"] = Json { { "done", core->prepared }, { "total", core->toPrepare } };
    std::lock_guard lock(impl.cacheMutex_);
    if (impl.lastSweepAt_) {
        envelope["lastSweep"] = Json { { "at", *impl.lastSweepAt_ }, { "freedBytes", impl.lastSweepFreed_ },
                                       { "files", impl.lastSweepFiles_ }, { "failed", impl.lastSweepFailed_ } };
    }
    return envelope;
}

Json Workspace::sweep_cache(const Json& params) {
    auto& impl = *impl_;
    const Json given { params.value("categories", Json::array()) };
    const auto wants = [&](std::string_view category, bool byDefault) {
        if (!given.is_array() || given.empty()) return byDefault;
        return std::ranges::find(given, Json(std::string { category })) != given.end();
    };
    const bool dryRun { params.value("dryRun", false) };
    if (impl.sweepRunning_.exchange(true)) {
        // C-13.1: one sweep at a time; a caller while one runs learns it and does the math itself.
        return Json { { "ok", true }, { "alreadyRunning", true }, { "dryRun", dryRun }, { "freedBytes", std::uint64_t { 0 } },
                      { "files", std::size_t { 0 } }, { "instances", std::size_t { 0 } }, { "roots", 1 } };
    }
    struct Running {
        std::atomic<bool>& flag;
        ~Running() { flag = false; }
    } running { impl.sweepRunning_ };

    // The safety rules of S3 5.8: nothing an engine holds, no canonical BMI, no engine stopped.
    std::int64_t bound { platform::fs::modified_now() };
    bool engineLive { false };
    for (const auto& engine : impl.engines) {
        if (const std::int64_t started { engine->generation_started_at() }; started > 0) {
            engineLive = true;
            bound = std::min(bound, started);
        }
    }
    const auto now { std::chrono::system_clock::now() };
    std::uint64_t freed { 0 };
    std::size_t files { 0 }, removed { 0 }, failed { 0 };
    if (wants("copies", true)) {
        for (const auto& context : cache::contexts_of(impl.cacheDirectory)) {
            const cache::Sweep one { cache::sweep_copies(cache::modules_root(context), bound, dryRun) };
            freed += one.bytes;
            files += one.files;
            failed += one.failed;
        }
    }
    if (wants("instances", true)) {
        const cache::Sweep one { cache::sweep_instances(impl.workspaceDirectory_, now, impl.cache_grace(), dryRun) };
        freed += one.bytes;
        removed += one.instances;
        failed += one.failed;
    }
    if (wants("trash", true)) {
        const cache::Sweep one { cache::sweep_trash(impl.cacheDirectory, dryRun) };
        freed += one.bytes;
        failed += one.failed;
    }
    // C-2's job on the engine path, and only ever by explicit request, and only where no engine
    // lives: a command that stops for nothing must not take the command directories either.
    if (wants("staleCommands", false) && !engineLive) {
        for (const auto& context : cache::contexts_of(impl.cacheDirectory)) {
            for (const auto& build : engine::clangd::stale_module_builds(base::join_path(context, "cdb"), 2)) {
                const std::uint64_t bytes { cache::tree_bytes(build) };
                if (!dryRun) platform::fs::remove_all(build);
                freed += bytes;
                ++files;
            }
        }
    }
    if (wants("budget", true) && !dryRun) {
        const cache::Sweep one { cache::enforce_budget(base::join_path(platform::dirs::cache_directory(), "workspaces"), impl.cache_budget(),
                                                       base::file_name(impl.workspaceDirectory_), now) };
        freed += one.bytes;
        files += one.files;
        failed += one.failed;
    }
    {
        std::lock_guard lock(impl.cacheMutex_);
        impl.lastSweepAt_ = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        impl.lastSweepFreed_ = freed;
        impl.lastSweepFiles_ = files;
        impl.lastSweepFailed_ = failed;
        impl.cacheReportAt_.reset();   // the next report is computed at once, from what is left
    }
    impl.journal.add("cache-swept", Json { { "bytes", freed }, { "files", files }, { "instances", removed },
                                           { "failed", failed }, { "origin", "command" } });
    impl.update_status();
    return Json { { "ok", true }, { "freedBytes", freed }, { "files", files }, { "instances", removed }, { "roots", 1 }, { "dryRun", dryRun } };
}

bool Workspace::restart_core_engine() {
    if (impl_->coreEngine == nullptr) return false;
    impl_->journal.add("engine-restart-requested");
    return impl_->coreEngine->restart_on_request();
}

void Workspace::set_context(const Json& id, std::string_view context) {
    if (context != "default" && (!impl_->model || !spec::find_set(impl_->model->database, context))) {
        impl_->client.reply_error(id, lsp::INVALID_PARAMS, std::format("unknown context {}", context));
        return;
    }
    impl_->contextSet = context == "default" ? std::string {} : std::string { context };
    impl_->journal.add("context", Json { { "context", std::string { context } } });
    impl_->client.reply(id, nullptr);
    if (impl_->model) impl_->replan();
}

bool Workspace::start_review(const Json& arguments) {
    if (impl_->reviewProcess) return false;
    const auto& options = impl_->options;
    if (options.serverExecutable.empty()) {
        log::warning("cannot review {}: the server does not know its own executable", root_);
        return false;
    }
    platform::SpawnOptions spawn;
    spawn.program = options.serverExecutable;
    spawn.arguments = { "review", "--format", "lsp", "--root", root_ };
    const Json settings = arguments.is_array() && !arguments.empty() && arguments[0].is_object() ? arguments[0] : Json::object();
    if (auto base = lsp::string_at(settings, "base"); base && !base->empty()) spawn.arguments.insert(spawn.arguments.end(), { "--base", *base });
    if (!options.payloadDirectory.empty()) spawn.arguments.insert(spawn.arguments.end(), { "--payload", options.payloadDirectory });
    if (!options.clangd.empty()) spawn.arguments.insert(spawn.arguments.end(), { "--clangd", options.clangd });
    if (!options.kit.empty()) spawn.arguments.insert(spawn.arguments.end(), { "--kit", options.kit });
    if (!options.mcpp.empty()) spawn.arguments.insert(spawn.arguments.end(), { "--mcpp", options.mcpp });
    if (!options.database.empty()) spawn.arguments.insert(spawn.arguments.end(), { "--database", options.database });
    if (options.engine != "clangd") spawn.arguments.insert(spawn.arguments.end(), { "--engine", options.engine });
    if (!options.discoverCompilers) spawn.arguments.push_back("--no-discover");
    if (!options.trusted) spawn.arguments.push_back("--untrusted");
    spawn.workDirectory = root_;
    spawn.pipeInput = false;
    spawn.pipeOutput = true;
    // The review starts an engine and may start the build tool: ending it must end all of that,
    // which is what a unit of its own buys (design 4.2). shut_down() kills the unit, not one process.
    spawn.ownUnit = true;
    auto started = platform::Process::spawn(spawn);
    if (!started) {
        log::warning("cannot review {}: {}", root_, started.error().message);
        return false;
    }
    impl_->reviewProcess = std::make_shared<platform::Process>(std::move(*started));
    log::info("review of {} started", root_);
    // The review runs its own headless session; this one keeps serving the editor meanwhile.
    std::thread { [process = impl_->reviewProcess, events = impl_->events, rootKey = key_] {
        std::string output;
        while (true) {
            auto chunk = process->read_output();
            if (!chunk || chunk->empty()) break;
            output += *chunk;
        }
        const auto code = process->wait();
        events->push(Event { EventKind::review_finished, Json { { "output", std::move(output) }, { "exitCode", code ? *code : -1 } }, 0, {}, rootKey, {} });
    } }.detach();
    return true;
}

void Workspace::handle_review_finished(const Json& outcome) {
    impl_->reviewProcess.reset();
    const Json result = Json::parse(outcome.value("output", std::string {}), nullptr, false);
    if (result.is_discarded() || !result.is_object() || result.contains("error")) {
        const std::string why { result.is_object() && result.contains("error") ? lsp::dump(result["error"]) : std::string { "no result" } };
        log::warning("review of {} failed: {}", root_, why);
        impl_->client.notify("window/logMessage", Json { { "type", 2 }, { "message", std::format("mcppls review failed: {}", why) } });
        return;
    }
    std::set<std::string> touched;
    for (const auto& [uri, diagnostics] : impl_->reviewDiagnostics) touched.insert(uri);
    impl_->reviewDiagnostics.clear();
    const Json byUri = result.value("diagnostics", Json::object());
    for (auto entry = byUri.begin(); entry != byUri.end(); ++entry) {
        // The editor's own spelling of a document it has open.
        const std::string uri { impl_->client_uri(entry.key()) };
        impl_->reviewDiagnostics[uri] = entry.value();
        touched.insert(uri);
    }
    for (const auto& uri : touched) impl_->publish_diagnostics(uri, true);
    const Json counts = result.value("counts", Json::object());
    impl_->client.notify("window/logMessage", Json { { "type", 3 },
                                                     { "message", std::format("mcppls review against {}: {} error(s), {} warning(s), {} note(s){}", result.value("base", std::string { "HEAD" }),
                                                                              counts.value("error", 0), counts.value("warning", 0), counts.value("information", 0),
                                                                              result.value("complete", true) ? "" : "; incomplete") } });
}

void Workspace::clear_review() {
    std::set<std::string> touched;
    for (const auto& [uri, diagnostics] : impl_->reviewDiagnostics) touched.insert(uri);
    impl_->reviewDiagnostics.clear();
    for (const auto& uri : touched) impl_->publish_diagnostics(uri, true);
}

void Workspace::handle_engine_event(std::string_view engineId, const Json& event) {
    for (const auto& engine : impl_->engines) {
        if (engine->id() == engineId) engine->handle_event(event);
    }
}

void Workspace::handle_tool_run(const Json& record) {
    // The environment build tools are started in, journaled the first time a run reports it and
    // whenever it changes (design 4.6). Variable NAMES only: a value may be a credential.
    const std::string source { record.value("environment", std::string {}) };
    if (!source.empty() && source != impl_->journaledToolEnvironment) {
        impl_->journaledToolEnvironment = source;
        const auto environment = platform::toolenv::get();
        impl_->journal.add("tool-environment", Json { { "source", source }, { "durationMs", environment.duration.count() },
                                                      { "differing", environment.differing }, { "reason", environment.reason } });
    }
    impl_->journal.add("tool-run", record);
}

bool Workspace::describe_online() {
    if (impl_->needsDownload.empty() || impl_->describingOnline) return false;
    log::info("fetching what the build description of {} needs, as asked", impl_->root);
    impl_->onlineOnce = true;
    impl_->downloadRetryAt.reset();
    impl_->start_model_load();
    impl_->update_status();
    return true;
}

void Workspace::reload_build_description() {
    // Only when something is actually waiting on it. A window regaining focus is not news, and a
    // build tool run for every focus change is exactly the implicit work this design removed.
    if (impl_->needsDownload.empty() && impl_->staleModelReason.empty()) return;
    // And at most one every thirty seconds: the build tool is the user's, not this server's, to keep busy.
    const auto now = Clock::now();
    if (impl_->lastManualReloadAt && now - *impl_->lastManualReloadAt < std::chrono::seconds { 30 }) return;
    impl_->lastManualReloadAt = now;
    log::info("reading the build description of {} again", root_);
    impl_->start_model_load();
}

void Workspace::reload_with_options(const SessionOptions& options, const std::string& compilerOverride, bool kitEnabled) {
    impl_->options.buildTool = options.buildTool;
    impl_->options.discoverCompilers = options.discoverCompilers;
    impl_->options.database = options.database;
    impl_->options.mcpp = options.mcpp;
    impl_->options.producerTimeout = options.producerTimeout;
    impl_->options.buildDiscovery = options.buildDiscovery;
    impl_->options.buildDiscoveryProviders = options.buildDiscoveryProviders;
    impl_->options.buildDiscoveryAskBeforeDownload = options.buildDiscoveryAskBeforeDownload;
    impl_->compilerOverride = compilerOverride;
    impl_->kitEnabled = kitEnabled;
    impl_->schedule_reload();
}

void Workspace::apply_live_options(const SessionOptions& options) {
    impl_->options.buildDiscoveryAskBeforeDownload = options.buildDiscoveryAskBeforeDownload;
    impl_->update_status();
}

void Workspace::handle_model_loaded(int generation, std::shared_ptr<project::ProjectModel> model, bool fromProducer) {
    if (generation != impl_->modelGeneration) return;
    impl_->handle_model_loaded(std::move(model), fromProducer);
}

std::optional<Clock::time_point> Workspace::next_deadline() const { return impl_->next_deadline(); }

void Workspace::handle_timers() { impl_->handle_timers(); }

const index::ModuleIndex& Workspace::module_index() const { return impl_->index; }

std::shared_ptr<const project::ProjectModel> Workspace::project_model() const { return impl_->model; }

const normalize::EnginePlan& Workspace::engine_plan() const { return impl_->plan; }

std::string Workspace::canonical_path_of(std::string_view uri) const { return impl_->path_of_uri(uri); }

bool Workspace::model_loading() const { return impl_->loading; }

std::uint64_t Workspace::snapshot_generation() const { return impl_->snapshotGeneration; }

std::optional<std::string> Workspace::document_text(std::string_view path) const {
    const Document* document { impl_->documents_.find_by_path(path) };
    if (document == nullptr) return std::nullopt;
    return document->text;
}

std::optional<engine::EngineStatus> Workspace::core_engine_status() const {
    if (impl_->coreEngine == nullptr) return std::nullopt;
    return impl_->coreEngine->status();
}

const std::string& Workspace::cache_directory() const { return impl_->cacheDirectory; }

bool Workspace::trusted() const { return impl_->options.trusted; }

const std::string& Workspace::mcpp_executable() const { return impl_->options.mcpp; }

std::optional<std::int64_t> Workspace::core_diagnostics_version(std::string_view uri) const {
    const auto found = impl_->coreDiagnosticsVersions.find(uri);
    if (found == impl_->coreDiagnosticsVersions.end()) return std::nullopt;
    return found->second;
}

bool Workspace::core_engine_serves(std::string_view method, std::string_view path) const {
    if (impl_->coreEngine == nullptr) return false;
    const Json params = Json::object();
    return impl_->coreEngine->claims(engine::RequestView { method, &params, path, {} });
}

} // namespace mcppls::orchestrator
