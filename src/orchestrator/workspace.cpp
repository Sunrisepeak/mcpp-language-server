module mcppls.orchestrator.workspace;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.glob;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.sha256;
import mcppls.base.text;
import mcppls.base.uri;
import mcppls.base.version;
import mcppls.orchestrator.cache;
import mcppls.engine.clangd.process;
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
import mcppls.project.mcpp;
import mcppls.project.target;
import mcppls.project.modelcache;
import mcppls.normalize.plan;
import mcppls.bundle.writer;
import mcppls.engine;
import mcppls.engine.payload;
import mcppls.engine.native.index;
import mcppls.engine.native.keywords;
import mcppls.engine.native.templates;
import mcppls.orchestrator.completion;
import mcppls.orchestrator.answer;
import mcppls.orchestrator.answering;
import mcppls.orchestrator.incidents;
import mcppls.orchestrator.journal;
import mcppls.orchestrator.client;
import mcppls.orchestrator.documents;
import mcppls.orchestrator.instance;
import mcppls.orchestrator.routing;
import mcppls.orchestrator.tokens;
import mcppls.orchestrator.highlights;
import mcpplibs.os;
import mcppls.base.home;
import mcppls.base.payload;
import mcppls.toolrun;
import mcppls.toolenv;

namespace os = mcpplibs::os;

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

// A turn of a root's timers past this says which part of it took the time (0.0.13 W10).
constexpr std::chrono::milliseconds TIMERS_SLOW { 1000 };
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

std::optional<std::string> described_source_structure(std::string_view text, const project::ScanResult& scan) {
    // The scanner recognizes ordinary # directives. Alternate directive tokens are
    // deliberately unacknowledged instead of guessing how the compiler processes them.
    if (text.contains("%:") || text.contains("?" "?=") || text.contains("_Pragma") || text.contains("__pragma")
        || text.contains("__LINE__") || text.contains("__COUNTER__") || !scan.preprocessorComplete) return std::nullopt;
    if (!scan.importsComplete || !scan.declarationsComplete || std::ranges::any_of(scan.imports, [](const auto& import) { return import.isHeaderUnit; })) return std::nullopt;
    return structure_of(scan) + "\npreprocessor-inputs\n" + scan.preprocessorDirectives;
}

// The inputs a producer named in its database's watch list (S2 5), shared with the polling worker.
struct WatchPatterns {
    std::atomic<bool> stopped { false };
    std::mutex mutex;
    std::vector<std::string> entries;   // glob patterns relative to the root, or absolute paths
    int generation { 0 };               // advanced whenever `entries` is replaced
    std::map<std::string, std::optional<os::fs::stamp>> concreteBaseline;
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

std::map<std::string, os::fs::stamp> watched_files_snapshot_of(std::string_view root, std::span<const std::string> entries, bool allRootFiles = false) {
    std::map<std::string, os::fs::stamp> files;
    for (const auto& file : base::files_under(root, {}, WATCH_POLL_SKIP_DIRECTORIES)) {
        if (!allRootFiles && !watch_covers(entries, root, file)) continue;
        if (auto fileStamp = base::file_stamp(file)) files.emplace(file, *fileStamp);
    }
    // Explicit files are watched independently of traversal exclusions, including target/ and
    // files outside the root. Globs do not cause traversal into excluded directories.
    for (const auto& entry : entries) {
        if (entry.find_first_of("*?[{") != std::string::npos) continue;
        const std::string path { base::is_absolute_path(entry) ? base::normalize_path(entry) : base::join_path(root, entry) };
        if (auto fileStamp = base::file_stamp(path)) files.insert_or_assign(path, *fileStamp);
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

std::map<std::string, os::fs::stamp> watched_files_snapshot(std::string_view root) { return watched_files_snapshot_of(root, {}); }

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
    std::optional<bool> cacheOnDevDrive;   // Windows only (0.0.14 E1)
    // 0.0.14 G1: the files last worked in, most recent first, kept in the cache directory: the next session has the core
    // engine get them ready before the editor opens them again.
    static constexpr std::size_t RECENT_FILES { 5 };
    static constexpr std::chrono::seconds RECENT_FILES_WRITTEN { 15 };
    std::deque<std::string> recentFiles;
    bool recentFilesChanged { false };
    std::optional<Clock::time_point> recentFilesWrittenAt;

    void note_recent_file(const std::string& path) {
        if (path.empty() || !base::is_within(path, root) || !project::is_cxx_source_name(path)) return;
        if (!recentFiles.empty() && base::same_path(recentFiles.front(), path)) return;
        std::erase_if(recentFiles, [&](const std::string& recent) { return base::same_path(recent, path); });
        recentFiles.push_front(path);
        while (recentFiles.size() > RECENT_FILES) recentFiles.pop_back();
        recentFilesChanged = true;
    }

    // A file the person closed is not among the next session's: an editor that quits closes nothing, so what it
    // restores stays, and what was put away goes.
    void forget_recent_file(const std::string& path) {
        if (std::erase_if(recentFiles, [&](const std::string& recent) { return base::same_path(recent, path); }) > 0) recentFilesChanged = true;
    }

    void save_recent_files(bool now) {
        if (!recentFilesChanged) return;
        if (!now && recentFilesWrittenAt && Clock::now() - *recentFilesWrittenAt < RECENT_FILES_WRITTEN) return;
        Json list = Json::array();
        for (const auto& path : recentFiles) list.push_back(path);
        (void)os::fs::write_atomic(base::join_path(cacheDirectory, "recent-files.json"), list.dump());
        recentFilesChanged = false;
        recentFilesWrittenAt = Clock::now();
    }

    std::vector<std::string> read_recent_files() {
        std::error_code ignored;
        std::vector<std::string> paths;
        const auto text = os::fs::read(base::join_path(cacheDirectory, "recent-files.json"));
        if (!text) return paths;
        const Json list = Json::parse(*text, nullptr, false);
        if (!list.is_array()) return paths;
        for (const auto& item : list) {
            if (!item.is_string() || paths.size() >= RECENT_FILES) continue;
            const std::string path { item.get<std::string>() };
            if (base::is_within(path, root) && std::filesystem::is_regular_file(path, ignored)) paths.push_back(path);
        }
        recentFiles.assign(paths.begin(), paths.end());
        return paths;
    }
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
    // One cache pass at a time in this process, background or interactive alike (0.0.10 review:
    // startup, an engine start and a command used to race on the same tree). A pass asked for
    // while one runs is merged into `sweepPending_` and retriggered when the running one reports.
    std::atomic<bool> sweepRunning_ { false };
    bool sweepPending_ { false };                  // under cacheMutex_
    bool sweepPendingReportOnly_ { true };         // the coalesced pass's mode: report-only unless a removal asked too
    std::optional<Clock::time_point> leaseRenewAt;
    engine::PayloadPaths payload;
    bool payloadCorrupt { false };
    std::optional<spec::Kit> kit;
    std::string macosSdk;
    // S4 section 5 (ecosystem design §9.2): the kits this root chooses from, and the choice for its target. A kit that
    // stands in for the target's own (the openkal kit, or the host's for another target) names the one that would serve
    // it, which the status offers to install.
    std::vector<spec::Kit> kits;
    std::string kitTarget;
    std::string kitMissingTarget;
    bool kitInstalling { false };
    // How the last kit install the person asked for ended (S3 `kitInstall`): {outcome, message, target, at}; null before.
    Json kitInstall;
    DocumentStore documents_;
    mutable std::unordered_map<std::string, std::string> canonicalByUri;
    index::ModuleIndex index;
    spec::MetadataReader metadataReader { spec::caching_metadata_reader() };
    std::map<std::string, os::fs::stamp> watchBaseline;
    std::shared_ptr<WatchPatterns> watchPatterns { std::make_shared<WatchPatterns>() };
    bool dynamicWatch { false };             // the client registers watchers for us (else this root polls)
    int watchRegistration { 0 };             // the id suffix of the model's current watcher registration, 0 for none
    // S2 5: a model whose producer failed to answer again is not replaced by the fallback; this says why.
    std::string staleModelReason;

    Json clientParams;
    std::string startedAtUtc;   // when this root started serving (about_json)
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
    // mcpp design 2026-10-28 §11.1: what the compiler last answered for each open document, moved with the edits
    // since; a request is answered from it at once and refreshed when the compiler has caught up (H1).
    highlights::Store highlights;
    std::uint64_t tokensAsked { 0 };                    // for the ids of the requests the server asks of its own
    std::size_t tokensWritten { 0 };                    // H2: files written since the directory was last pruned
    // H2: when the compiler is asked to confirm tokens read from disk -- a moment after the open, so the request does not
    // compete with the file's first completion (the key cannot name the headers the file includes; the compiler can).
    static constexpr std::chrono::milliseconds TOKENS_CONFIRM_DELAY { 1500 };
    std::map<std::string, Clock::time_point, std::less<>> tokensConfirmAt;
    // Diagnostics published by engines other than mcppls's own, per engine and client URI.
    std::map<std::string, std::map<std::string, Json, std::less<>>, std::less<>> engineDiagnostics;
    std::map<std::string, std::string, std::less<>> publishedDiagnostics;
    // The document version the core engine's latest diagnostics were computed for, by client URI (-1: not said).
    std::map<std::string, std::int64_t, std::less<>> coreDiagnosticsVersions;
    // The review an editor asked for (overall design 7.7): its findings as LSP diagnostics, by client URI.
    std::map<std::string, Json, std::less<>> reviewDiagnostics;
    std::shared_ptr<os::process::process> reviewProcess;

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
    // UP-25 (issue #24): what the core engine has been answering per file, and how long it took -- the completion
    // budget of a file whose answers keep landing just past the flat one waits for them instead of cancelling each.
    std::map<std::string, completion::EnginePace, std::less<>> completionPace_;
    static constexpr std::chrono::milliseconds ADAPTIVE_COMPLETION_BUDGET { 2500 };
    // F9 (D4): what space-triggered completion costs. Most never pass the gate and cost one look at a line.
    struct SpaceTriggerStats {
        std::size_t count { 0 };
        std::size_t passed { 0 };
        std::int64_t maxMicros { 0 };
        std::int64_t totalMicros { 0 };
    };
    SpaceTriggerStats spaceTrigger;
    std::size_t keywordsWithoutEngine { 0 };   // F15: keyword completions answered before the core engine did
    std::size_t templatesWithoutEngine { 0 };  // S3-6.4: completions with code templates answered before the core engine did
    std::size_t wordsWithoutCore { 0 };        // M-2 (plan 0.0.8): completions the core engine did not answer, given the file's words
    // C-2 (plan 0.0.8 part 2): completions the core engine answered after their budget, and requests given such an answer.
    std::size_t lateCompletionsArrived { 0 };
    std::size_t lateCompletionsServed { 0 };
    // E-3 (plan 0.0.8): how much the person edited, so a report shows edits going on while a request stopped coming.
    std::size_t documentChanges { 0 };
    std::string lastDocumentChangeAt;
    bool vscodeLike { false };                 // the client runs VS Code's commands (completion::vscode_like)
    bool snippetSupport { false };             // textDocument.completion.completionItem.snippetSupport: templates are snippets

    // Requests in flight across engines.
    // A semantic-tokens request (mcpp design 2026-10-28 §11.1): what the client asked for, and what the engines gave.
    // Every engine is asked for the whole file; a range is cut from the answer here.
    struct TokensRequest {
        enum class Kind { full, delta, range };
        Kind kind { Kind::full };
        std::string uri;
        std::string previousResultId;                     // a delta's: the answer the client holds
        std::optional<base::Range> range;
        bool fromKept { false };                          // answered from what is kept: the core engine was not asked for it
        bool answered { false };                          // an engine gave a result
        std::vector<tokens::Token> native;                // mcppls's own module-syntax tokens (and any other merging engine's)
        std::optional<std::vector<tokens::Token>> core;   // the core engine's, when it was asked and answered
    };
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
        bool renamePreflight { false };
        engine::RequestView view;
        Json params;
        std::string path;
        std::string text;
        Json message;
        // F15: a completion's module-syntax keywords, merged into whatever the engines answer.
        Json keywords;
        // S3-6.4: its code templates, merged the same way.
        Json templates;
        // R-7 (plan 2026-09-30): when mcppls answers without the core engine (routing::answer_budget).
        std::optional<Clock::time_point> budgetAt;
        // M-2 (plan 0.0.8): the core engine gave an answer, even an empty one; a completion it did not answer
        // (not asked, unavailable, failed) gets the file's words in finish_job.
        bool coreAnswered { false };
        // C-2 (plan 0.0.8 part 2): the job asked nothing of its own and waits for a late completion of the same word.
        bool waitsForLate { false };
        // The answer layer: the request read, the best kept answer for it, and whether an agent asked (MCP tools).
        std::optional<answering::Asked> asked;
        std::optional<answering::Found> found;
        bool agent { false };
        bool withoutCore { false };   // answered by mcppls itself because the core engine did not answer in time
        // M-1 (plan 0.0.9): when the request went to the engine whose answer went out, and when that engine replied.
        // The difference is the engine's share of the request's time; the rest is mcppls's own.
        std::optional<Clock::time_point> engineSentAt;
        std::optional<Clock::time_point> engineRepliedAt;
        std::optional<TokensRequest> tokens;
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
        std::string path;
        completion::WordKey key;
        Json clientId;                          // the request the core engine is still working on
        Clock::time_point started;              // when the person asked (UP-25: the answer's latency feeds the file's pace)
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
    // 0.0.14 A1: the core engine's last complete answer in a word, with the document outside the word as it was then. A
    // client that asks again as the word is typed on (some ask at every key, whatever the list says) is given it at once:
    // the engine's candidates for the word are the same while nothing outside it changed. Only a complete list: one the
    // engine cut short may lack what the longer word matches. MCPPLS_COMPLETION_REFILTER=off turns it off.
    struct WordAnswer {
        completion::WordKey key;
        std::uint64_t outside { 0 };
        Json result;
        Clock::time_point at;
    };
    static constexpr std::chrono::seconds WORD_ANSWER_KEPT { 30 };
    std::map<std::string, WordAnswer, std::less<>> wordAnswers;   // document URI -> its last complete core answer
    const bool wordAnswersOn { os::env::get("MCPPLS_COMPLETION_REFILTER").value_or("") != "off" };
    std::size_t completionsFromWord { 0 };

    // The answer layer (answer-layer design 2026-10-13): what the core engine answered, kept by what it depends on, and
    // given at once the next time the same is asked while the engine is slower than the answer budget. The engine's
    // request goes on in the background and its answer replaces what was kept; a request the client cancels does too.
    std::unique_ptr<answering::Layer> answers_;
    std::unordered_map<std::string, std::uint64_t> answerInflight_;   // a place (K1) -> the job asking the core engine about it
    struct Background {
        answering::Asked asked;
        std::optional<answering::Served> served;   // what went out instead of the engine's answer, if anything did
        Json clientId;
        Clock::time_point requestStarted;          // the original client request, before any fallback
        Clock::time_point started;                 // detached at; eviction and notice freshness use this
        Clock::time_point until;
        std::int64_t version { 0 };                 // the document's when it was asked
        bool sparse { false };                      // what went out had (almost) nothing after a `.`, `->` or `::` (R3)
    };
    std::map<std::uint64_t, Background> background_;
    static constexpr std::size_t BACKGROUND_LIMIT { 2 };
    static constexpr std::chrono::seconds BACKGROUND_KEEP { 30 };
    static constexpr std::chrono::seconds READY_NOTICE_WITHIN { 5 };
    std::unordered_map<std::string, std::string> planCommands_;   // path key -> the plan's command for it, as one string
    std::uint64_t internalRequests_ { 0 };
    std::size_t learning_ { 0 };                                  // typeDefinition requests in flight
    std::optional<Clock::time_point> lastClientRequestAt_;
    std::string lastAskedUri_;
    base::Position lastAskedPosition_ {};
    std::optional<Clock::time_point> prefetchAt_;
    std::optional<Clock::time_point> prefetchExhaustedFor_;      // the request after which nothing near was left to ask
    bool prefetchInFlight_ { false };
    std::optional<std::size_t> prefetchRestartsAt_;               // the core engine's restarts when prefetching began
    bool prefetchStopped_ { false };
    std::deque<Clock::time_point> prefetched_;                    // the last minute's, at most PREFETCH_PER_MINUTE
    static constexpr std::size_t PREFETCH_PER_MINUTE { 30 };
    std::optional<Clock::time_point> answersSaveAt_;

    // Project model and plan.
    std::shared_ptr<project::ProjectModel> model;
    int modelGeneration { 0 };
    std::string modelDescription;            // describe_model of `model`, to recognize an unchanged reload
    bool loading { false };
    bool applyingPlan { false };
    bool reloadAfterLoad { false };
    bool producerStatesResetPending { false };   // XS-4: a reset asked for them to go before the next load
    // A lexical source structure can legitimately differ from the producer's active
    // dependency graph (#if 0, for example). Remember only inputs a valid answer covered.
    std::map<std::string, std::string> producerSourcePaths;
    std::map<std::string, std::string> loadingSourceStructures;
    std::map<std::string, os::fs::stamp> loadingSourceStamps;
    std::map<std::string, std::string> describedSourceStructures;
    int describedSourceGeneration { -1 };
    std::string loadingSourceProducerIdentity;
    std::string describedSourceProducerIdentity;
    project::TargetSelection targetSelection;
    bool targetReadPending { false };
    int targetReadRetries { 0 };
    std::optional<Clock::time_point> targetRetryAt;
    std::string targetSelectionInput;
    int targetSelectionGeneration { 0 };
    bool targetSelectionBusy { false };
    bool targetLoadRequested { false };
    bool targetCacheDeferred { false };
    std::shared_ptr<project::ProjectModel> targetDeferredModel;
    normalize::EnginePlan plan;
    bool firstPlanWritten { false };
    std::string contextSet;
    std::map<std::string, std::string, std::less<>> structures;   // path key -> module structure at planning time
    // robustness design C2: C++ sources of the workspace that the editor opened and no set describes. They join the engine
    // database with the nearest unit's arguments and stay for the session, so opening one again leaves the database as it is.
    std::set<std::string> openedOutsideModel;
    std::set<std::string> plannedFiles;      // path keys of the plan's units and of the files it left out
    // mcpp-community/mcpp#797: path keys of mcpp's engine interface, which mcpp writes once for each of its versions:
    // no edit is offered there, and none reaches it.
    std::set<std::string, std::less<>> readOnlySources;
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
    // 0.0.17 W4 (#59 U15): a structure reload that saves of a file still being typed in asked for waits until the file
    // is quiet (EDIT_SETTLE after its last change). With autosave, every pause while an import is typed is a save that
    // changes the file's imports; each was a run of the producer and a loading state of its own. Edits that keep coming
    // put it off no longer than EDIT_DEFER_LIMIT after the first save that waited, so the model is never left behind.
    static constexpr std::chrono::seconds EDIT_DEFER_LIMIT { 30 };
    std::set<std::string, std::less<>> reloadWaitsFor;   // path keys of the files being typed in
    std::optional<Clock::time_point> reloadDeferredSince;
    std::optional<Clock::time_point> loadGiveUpAt;       // the producer has not answered: take what there is (design 4.1)
    std::optional<Clock::time_point> lastResortAt;       // nothing at all came: serve without a database rather than nothing
    std::optional<Clock::time_point> sdkCheckAt;
    // While the kit of the target is missing: when to look again for it, installed by the editor, by hand, or by
    // another window (a kit appears in xlings' store without this server's own install).
    std::optional<Clock::time_point> kitCheckAt;

    // Model sources (design 4.1). The cache is per source and carries a fingerprint of everything
    // the producer read, so a session can start from it and confirm it in the background.
    project::SourceKind detectedSource { project::SourceKind::inferred };
    std::string detectedManifest;
    std::optional<project::CachedModel> staleCache;      // a cache whose fingerprint no longer matches
    std::string modelOrigin;                             // cache-fresh | cache-stale | cache-confirmed | producer | inferred
    std::string firstModelOrigin;                        // where the model this session started with came from
    std::string producerPath;                            // for the fingerprint of the next save
    std::string producerVersion;
    std::string producerObservedIdentity;
    std::optional<Clock::time_point> producerIdentityAt;
    std::optional<Clock::time_point> targetIdentityAt;
    std::string staleCacheWhy;   // which input made the cached model stale (0.0.13 W6)
    // 0.0.13 W6: this start's timeline, said in one line when the root is first ready and kept, with the last few starts,
    // in the report and in <cache>/startups.json: a warm start and a cold one are compared from those lines alone.
    struct Startup {
        Clock::time_point began { Clock::now() };
        std::string beganUtc;
        std::optional<double> model, engine, firstDiagnostics, firstCompletion, ready;
        std::string origin;
        std::string state;   // what it settled at: ready or degraded
        bool said { false };
    };
    Startup startup;
    std::optional<Clock::time_point> healthAt;
    std::size_t healthRequestsSeen { 0 };
    std::string keptModelNotice;   // why a newer answer was not taken in place of this model (0.0.13 W3)
    // 0.0.13 W3(a): a build program (build.mcpp) changed and the project is not described again yet. Describing it
    // compiles and runs the build program, so a change is applied once it has been saved and left alone for a moment,
    // and not while clangd says it does not compile; `buildProgramHeld` says why it waits.
    std::string buildProgramPending;
    std::string buildProgramHeld;
    std::optional<Clock::time_point> buildProgramChangedAt;
    std::optional<Clock::time_point> buildProgramCheckAt;
    std::string modelAdoptedAtUtc;   // when the model in hand was taken, for that notice
    struct ProducerDiagnostics {
        std::string path;   // as the root spells it, for its URI
        Json diagnostics;
    };
    std::map<std::string, ProducerDiagnostics, std::less<>> producerDiagnostics;   // path key -> mcpp's diagnostics on a build program
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
        std::error_code ignored;
        const std::string workspaceDirectory { base::join_path(base::cache_directory(), base::join_path("workspaces", project::workspace_key(root))) };
        lease = WorkspaceLease::acquire(workspaceDirectory, std::chrono::system_clock::now(), root);
        workspaceDirectory_ = workspaceDirectory;
        ownToken_ = lease->token();
        cacheDirectory = lease->directory();
        // The heartbeat tick is every instance's own (C-9): a guest keeps its `instance.json` fresh
        // with it, so a sweep can tell it from a dead one without touching the owner's lease.
        leaseRenewAt = Clock::now() + LEASE_RENEWAL;
        // Under its one name, like every file the engines are given (engine_uri): the prime units
        // and the database live here, and clangd answers for them under the name it was given.
        (void)std::filesystem::create_directories(cacheDirectory, ignored);
        cacheDirectory = base::canonical_path(cacheDirectory);
        // 0.0.14 E1: on Windows, a cache off a Dev Drive has every file clangd writes and reads there scanned by
        // Microsoft Defender as it happens. Said once, in the log and the overview; nothing is changed for the person.
        if constexpr (os::is_windows) cacheOnDevDrive = os::windows::on_dev_drive(std::filesystem::path { cacheDirectory });
        if (cacheOnDevDrive == false) {
            log::info("the cache {} is not on a Dev Drive: Microsoft Defender scans the module and preamble files clangd writes and reads "
                      "there as it does; a Dev Drive scans them asynchronously (docs/50-troubleshooting.md, \"Windows\")", cacheDirectory);
        }
        {
            answering::Settings settings { options.answerCache, options.answerBudget, options.answerPrefetch,
                                           answer::marks_of(options.completionProvenance).value_or(answer::Marks::subtle), options.answerAgent == "authoritative" };
            if (!wordAnswersOn) settings.cache = "off";   // MCPPLS_COMPLETION_REFILTER=off: as 0.0.14 with A1 off
            answers_ = std::make_unique<answering::Layer>(
                std::move(settings), base::join_path(workspaceDirectory_, "answers"), base::join_path(base::cache_directory(), "answers"),
                [this](std::string_view path) -> std::optional<std::string> {
                    if (const Document* document = documents_.find_by_path(path)) return document->text;
                    return std::nullopt;
                });
        }
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
        if (coreEngine != nullptr && engineId == coreEngine->id()) {
            coreDiagnosticsVersions[uri] = version.value_or(-1);
            if (!startup.firstDiagnostics) startup.firstDiagnostics = since_start_();
        }
        publish_diagnostics(uri, true);
        // A build program waiting on clangd's verdict gets it now.
        if (!buildProgramPending.empty() && coreEngine != nullptr && engineId == coreEngine->id() && !buildProgramCheckAt) {
            if (const Document* document = documents_.find(uri); document != nullptr && base::same_path(document->path, buildProgramPending)) {
                buildProgramCheckAt = Clock::now();
            }
        }
    }

    void forget_engine_diagnostics(std::string_view engineId) override {
        engineDiagnostics.erase(std::string { engineId });
        if (coreEngine != nullptr && engineId == coreEngine->id()) coreDiagnosticsVersions.clear();
    }

    void engine_settled(std::string_view engineId, const Json& serverCapabilities) override {
        if (coreEngine != nullptr && engineId != coreEngine->id()) return;
        if (!startup.engine) startup.engine = since_start_();
        if (coreEngine != nullptr) coreCapabilities = serverCapabilities;
        // Rebuilt from the very capabilities merge_capabilities is about to see (or already saw,
        // for the first root), so a request's core-engine tokens are always remapped by the same
        // legend the client was told about.
        tokensLegend = tokens::build_legend(coreCapabilities);
        highlights.unsettle_all();   // a core engine that (re)started has answered nothing yet
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
        // A setting that does not parse says so (review 2026-10-03): the default quietly holding is
        // how a typo'd budget used to look exactly like no budget at all.
        if (const std::string given { options.settings.string_value("cache.maxBytes") }; !given.empty()) {
            if (const auto bytes = cache::parse_bytes(given)) budget.perWorkspace = *bytes;
            else log::warning("setting cache.maxBytes \"{}\" does not parse (4G, 512M, 100K, bytes or unlimited); keeping the default", given);
        }
        if (const std::string given { options.settings.string_value("cache.totalBytes") }; !given.empty()) {
            if (const auto bytes = cache::parse_bytes(given)) budget.total = *bytes;
            else log::warning("setting cache.totalBytes \"{}\" does not parse (4G, 512M, 100K, bytes or unlimited); keeping the default", given);
        }
        return budget;
    }

    std::chrono::seconds cache_grace() const { return options.settings.seconds_value("cache.instanceGrace"); }

    // One background pass: dead instances first (they can free the most), then the copies, then the
    // budget across the workspaces nothing has open. `before` is the sweep's bound on the file
    // clock; the result arrives as a `cache_swept` event, so the journal, the status and the report
    // cache are all touched on the session loop, as everything else is. `reportOnly` skips every
    // removal and only refreshes the numbers (a stale `cxxModules/cache` answer). Single-flight:
    // while one pass runs, another asked-for pass is coalesced and retriggered by
    // `handle_cache_swept`, never queued behind it (review 2026-10-03: a crash-looping engine
    // starting one full pass per restart is many threads on one tree otherwise).
    void start_cache_task_(std::string_view origin, std::int64_t before = base::file_time_now(), bool reportOnly = false) {
        {
            std::lock_guard lock(cacheMutex_);
            if (sweepRunning_.load()) {
                sweepPending_ = true;
                sweepPendingReportOnly_ = sweepPendingReportOnly_ && reportOnly;   // a removal pass asked for later wins
                return;
            }
            sweepRunning_.store(true);
        }
        const cache::Budget budget { cache_budget() };
        const auto grace { cache_grace() };
        const std::string workspaceDirectory { workspaceDirectory_ };
        const std::string ownCacheDirectory { cacheDirectory };
        const std::string ownKey { base::file_name(workspaceDirectory_) };
        const std::string ownToken { ownToken_ };
        auto queue = events;
        const std::string rootKey { key };
        std::thread { [budget, grace, workspaceDirectory, ownCacheDirectory, ownKey, ownToken, origin = std::string { origin }, before, queue, rootKey, reportOnly]() mutable {
            const auto now { std::chrono::system_clock::now() };
            std::uint64_t bytes { 0 };
            std::size_t files { 0 }, instances { 0 }, failed { 0 };
            if (!reportOnly) {
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
                const cache::Sweep over { cache::enforce_budget(base::join_path(base::cache_directory(), "workspaces"), budget, ownKey, now) };
                bytes += over.bytes;
                files += over.files;
                failed += over.failed;
            }
            Json report;
            try {
                report = cache::report(ownCacheDirectory, budget, std::chrono::system_clock::now(), ownToken);
            } catch (...) {
                report = Json::object();   // a report is never worth a crashed sweeper thread
            }
            queue->push(Event { EventKind::cache_swept,
                                Json { { "origin", origin }, { "reportOnly", reportOnly }, { "bytes", bytes }, { "files", files },
                                       { "instances", instances }, { "failed", failed }, { "report", std::move(report) } },
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
                     { "os", std::string { os::name(os::host()) } },
                     { "arch", std::string_view { base::payload_arch() } },
                     { "logDirectory", base::parent_path(log::file_path()) },
                     { "bundlesDirectory", bundle::default_directory() } };
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
        // The compiler may answer differently for every file now: what is kept stands in until it has answered again.
        highlights.unsettle_all();
        request_tokens_refresh(std::chrono::milliseconds { 500 });
    }

    // A refresh within `within`, or sooner when one is due sooner already. A compiler answer the client does not have
    // asks for one within TOKENS_REFRESH_SOON: the person is looking at a stand-in.
    static constexpr std::chrono::milliseconds TOKENS_REFRESH_SOON { 20 };
    void request_tokens_refresh(std::chrono::milliseconds within) {
        if (!clientSupportsTokensRefresh || !initializeAnswered) return;
        const auto at = Clock::now() + within;
        if (!tokensRefreshAt || at < *tokensRefreshAt) tokensRefreshAt = at;
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
        if (os::host() == os::platform::windows) {
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
        std::string canonical { path ? base::canonical_path(*path) : std::string {} };
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
        consider(targetRetryAt);
        consider(producerIdentityAt);
        consider(targetIdentityAt);
        consider(buildProgramCheckAt);
        consider(healthAt);
        consider(replanAt);
        if (!coreWaitOver) consider(coreWaitUntil);
        consider(loadGiveUpAt);
        consider(downloadRetryAt);
        consider(lastResortAt);
        consider(producerSoftAt);
        consider(sdkCheckAt);
        consider(kitCheckAt);
        consider(statusFlushAt);
        consider(leaseRenewAt);
        consider(tokensRefreshAt);
        for (const auto& [uri, at] : tokensConfirmAt) consider(at);
        for (const auto& [id, job] : jobs) consider(job.budgetAt);
        for (const auto& [id, late] : lateCompletions) consider(late.until);
        for (const auto& [id, background] : background_) consider(background.until);
        consider(prefetchAt_);
        consider(answersSaveAt_);
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
            if (patterns->stopped.load()) return;
            // Build/source stamps captured at Workspace construction remain authoritative during
            // startup. Broaden the baseline off the LSP thread so later watch registration can
            // distinguish preexisting glob matches from files created after publication.
            auto initial = watched_files_snapshot_of(rootPath, {}, true);
            // A build/source file created since the startup snapshot must still be reported.
            std::erase_if(initial, [&](const auto& entry) { return watch_covers({}, rootPath, entry.first) && !known.contains(entry.first); });
            for (const auto& [path, stamp] : known) initial.insert_or_assign(path, stamp);
            known = std::move(initial);
            std::vector<std::string> knownEntries;
            int knownGeneration { 0 };
            while (!patterns->stopped.load()) {
                std::this_thread::sleep_for(std::chrono::seconds { 2 });
                if (patterns->stopped.load()) return;
                std::vector<std::string> entries;
                std::map<std::string, std::optional<os::fs::stamp>> concreteBaseline;
                int generation { 0 };
                {
                    const std::lock_guard lock { patterns->mutex };
                    entries = patterns->entries;
                    generation = patterns->generation;
                    if (generation != knownGeneration) concreteBaseline = patterns->concreteBaseline;
                }
                // Concrete inputs may live outside the root walk or in excluded directories.
                // Publication stamps catch deletion before the first poll of a newly declared watch.
                for (const auto& [path, stamp] : concreteBaseline) {
                    if (known.contains(path) && watch_covers(knownEntries, rootPath, path)) continue;
                    if (stamp) known.insert_or_assign(path, *stamp);
                    else known.erase(path); // absent at publication, even if the background walk saw it later
                }
                auto current = watched_files_snapshot_of(rootPath, entries, true);
                Json changes = Json::array();
                for (const auto& [file, fileStamp] : current) {
                    if (!watch_covers(entries, rootPath, file)) continue;
                    const auto previous = known.find(file);
                    if (previous == known.end()) changes.push_back(Json { { "uri", base::path_to_uri(file) }, { "type", 1 } });
                    else if (previous->second != fileStamp) changes.push_back(Json { { "uri", base::path_to_uri(file) }, { "type", 2 } });
                }
                for (const auto& [file, fileStamp] : known) {
                    if (current.contains(file) || !watch_covers(entries, rootPath, file)) continue;
                    changes.push_back(Json { { "uri", base::path_to_uri(file) }, { "type", 3 } });
                }
                known = std::move(current);
                knownEntries = std::move(entries);
                knownGeneration = generation;
                if (patterns->stopped.load()) return;
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
        job.params = job.message.contains("params") ? job.message["params"] : Json::object();
        const std::string uri { uri_of_params(job.params) };
        job.path = uri.empty() ? std::string {} : path_of_uri(uri);
        const Document* document { uri.empty() ? nullptr : documents_.find(uri) };
        if (document != nullptr) job.text = document->text;
        job.view = engine::RequestView { job.method, &job.params, job.path, job.text };
        // The MCP tools ask through the kernel, under ids of their own: their answers say where they came from.
        if (job.clientId.is_string()) {
            const std::string& id { job.clientId.get_ref<const std::string&>() };
            job.agent = id.starts_with("k:") || id.starts_with("ka:");
        }
        if (!job.path.empty() && readOnlySources.contains(base::path_key(job.path)) && refuse_edit(jobId)) return;
        if (is_tokens_method(job.method)) {
            route_tokens_request(jobId, uri);
            return;
        }
        if (job.method == lsp::method::TEXT_DOCUMENT_COMPLETION) {
            if (completion::is_space_trigger(job.params)) {
                route_space_triggered(jobId);
                return;
            }
            if (document != nullptr && !job.path.empty()) {
                if (const auto position = position_of(job.params)) {
                    const project::ScanResult* scan { index.scan_of(job.path) };
                    job.keywords = index::keyword_completion(job.text, *position, scan, index::KeywordOptions { .suggestModulesAfterImport = vscodeLike });
                    job.templates = index::template_completion(job.text, *position, scan, index::TemplateOptions { .snippets = snippetSupport });
                    if (answered_from_word(jobId, uri, *position)) return;
                    if (answered_late(jobId, uri, *position)) return;
                }
            }
        }
        if (answers_ && answers_->enabled() && coreEngine != nullptr && document != nullptr && !job.path.empty() && answer::kind_of(job.method)) {
            const std::string id { job.clientId.is_string() ? job.clientId.get<std::string>() : std::string {} };
            if (!job.agent) {
                lastClientRequestAt_ = Clock::now();
                lastAskedUri_ = uri;
                if (const auto position = position_of(job.params)) lastAskedPosition_ = *position;
            }
            // An agent that asked for the engine's own answer (fresh, or mcppls.answer.agent: authoritative) waits for it.
            const bool authoritativeOnly { job.agent && (id.starts_with("ka:") || answers_->settings().agentAuthoritative) };
            if (auto asked = answers_->read(job.method, uri, job.path, job.text, job.params, answer_fingerprint(job.path, job.text))) {
                job.asked = std::move(*asked);
                if (!authoritativeOnly) job.found = answers_->lookup(*job.asked);
                // The same place is being asked of the core engine already: what is kept goes out now, and the engine's
                // answer, when it comes, is kept for the next one.
                if (job.found && answerInflight_.contains(job.asked->k1) && serve_kept(jobId)) return;
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
            for (engine::Engine* merger : selection.mergers) {
                const std::string engineId { merger->id() };
                merger->request(job.view, job.message, [this, jobId, engineId](engine::Answer answer) { merge_answer(jobId, engineId, std::move(answer)); });
                if (!jobs.contains(jobId)) return;
            }
            return;
        }
        if (selection.answerers.empty()) {
            if (job.method == "textDocument/rename" && !readOnlySources.empty()) {
                fail_rename_ownership(jobId, "no compiler can resolve the symbol");
                return;
            }
            finish_job(jobId, Json(nullptr));
            return;
        }
        job.answerers = std::move(selection.answerers);
        // An agent that asked for the engine's own answers (S5-2.6-2: `fresh`, or mcppls.answer.agent: authoritative)
        // waits for the engine up to its own deadline: neither a kept answer nor mcppls's own stands in.
        const bool engineOnly { job.agent && job.clientId.is_string()
                                && (job.clientId.get_ref<const std::string&>().starts_with("ka:") || (answers_ && answers_->settings().agentAuthoritative)) };
        if (const auto budget = request_budget(job); budget && !engineOnly && coreEngine != nullptr
            && std::ranges::find(job.answerers, coreEngine) != job.answerers.end()) {
            job.budgetAt = job.started + *budget;
        }
        // A kept answer goes out at the answer budget (a jump's only when it is certain: one that may have moved waits),
        // or just after the engine's own if it has been answering this file within its regular budget: a kept list that
        // goes out a moment before the engine's complete one has the editor ask again at every key (ux-modules-windows).
        if (job.found && coreEngine != nullptr && std::ranges::find(job.answerers, coreEngine) != job.answerers.end()
            && (!answer::is_jump(job.asked->kind) || job.found->certainty == answer::Certainty::certain)) {
            const auto at = job.started + kept_answer_wait(job);
            job.budgetAt = job.budgetAt ? std::min(*job.budgetAt, at) : at;
        }
        if (job.method == "textDocument/rename" && !readOnlySources.empty()) {
            preflight_rename(jobId);
            return;
        }
        ask_next(jobId);
    }

    // ---- semantic tokens (mcpp design 2026-10-28 §11.1) -------------------------------------------------------------

    static bool is_tokens_method(std::string_view method) {
        return method == lsp::method::TEXT_DOCUMENT_SEMANTIC_TOKENS_FULL || method == lsp::method::TEXT_DOCUMENT_SEMANTIC_TOKENS_FULL_DELTA
               || method == lsp::method::TEXT_DOCUMENT_SEMANTIC_TOKENS_RANGE;
    }

    static std::optional<base::Range> range_of(const Json& params) {
        const Json* range { lsp::find(params, "range") };
        if (range == nullptr || !range->is_object()) return std::nullopt;
        const Json* start { lsp::find(*range, "start") };
        const Json* end { lsp::find(*range, "end") };
        if (start == nullptr || end == nullptr) return std::nullopt;
        const auto read = [](const Json& position) -> std::optional<base::Position> {
            const auto line = lsp::int_at(position, "line");
            const auto character = lsp::int_at(position, "character");
            if (!line || !character) return std::nullopt;
            return base::Position { static_cast<int>(*line), static_cast<int>(*character) };
        };
        const auto from = read(*start);
        const auto to = read(*end);
        if (!from || !to) return std::nullopt;
        return base::Range { *from, *to };
    }

    // H1. A request for an open document's tokens is answered at once from what the compiler last gave for it when that
    // is exact (its very text, nothing it reads changed since), or -- for a client that can be refreshed -- from what was
    // kept, moved with the edits since (or from mcppls's own module-syntax tokens alone, before the compiler gave any):
    // the compiler is asked meanwhile, and the client is told to ask again when its answer differs. A client that cannot
    // be refreshed waits for the compiler, as before.
    void route_tokens_request(std::uint64_t jobId, const std::string& uri) {
        Job& job = jobs.at(jobId);
        TokensRequest request;
        request.uri = uri;
        if (job.method == lsp::method::TEXT_DOCUMENT_SEMANTIC_TOKENS_FULL_DELTA) {
            request.kind = TokensRequest::Kind::delta;
            if (const Json* previous { lsp::find(job.params, "previousResultId") }; previous != nullptr && previous->is_string()) {
                request.previousResultId = previous->get<std::string>();
            }
        }
        if (job.method == lsp::method::TEXT_DOCUMENT_SEMANTIC_TOKENS_RANGE) {
            request.kind = TokensRequest::Kind::range;
            request.range = range_of(job.params);
        }
        // Every engine is asked for the whole file: a range is cut from it, and a delta made of it, here.
        job.method = std::string { lsp::method::TEXT_DOCUMENT_SEMANTIC_TOKENS_FULL };
        job.message["method"] = job.method;
        if (job.message.contains("params") && job.message["params"].is_object()) {
            job.message["params"].erase("previousResultId");
            job.message["params"].erase("range");
        }
        job.params = job.message.contains("params") ? job.message["params"] : Json::object();
        job.view = engine::RequestView { job.method, &job.params, job.path, job.text };
        const Document* document { uri.empty() ? nullptr : documents_.find(uri) };
        highlights::Kept* kept { document != nullptr && !job.path.empty() ? &highlights.at(uri) : nullptr };
        const bool coreCan { coreEngine != nullptr && !core_waits_for_producer() && coreEngine->claims(job.view) };
        if (kept != nullptr && !kept->known && !kept->cacheChecked) read_kept_tokens(*document, *kept);
        if (kept != nullptr && kept->known && kept->exact && kept->version == document->version) {
            request.fromKept = true;
            ++highlights.counters.exact;
            // H2: what the disk held is confirmed by the compiler once, in the background, a moment later.
            if (kept->fromCache && coreCan && clientSupportsTokensRefresh) tokensConfirmAt.try_emplace(uri, Clock::now() + TOKENS_CONFIRM_DELAY);
        } else if (kept != nullptr && coreCan && clientSupportsTokensRefresh) {
            request.fromKept = true;
            ++highlights.counters.standIn;
            ask_core_tokens(uri);
        } else if (coreCan) {
            ++highlights.counters.waited;
        }
        job.tokens = std::move(request);
        std::vector<engine::Engine*> candidates;
        for (const auto& engine : engines) {
            if (engine.get() == coreEngine && (job.tokens->fromKept || !coreCan)) continue;
            candidates.push_back(engine.get());
        }
        Selection selection { select_engines(candidates, job.view) };
        if (selection.mergers.empty()) {
            finish_tokens_job(jobId);
            return;
        }
        job.merging = true;
        job.awaiting = selection.mergers.size();
        job.engineSentAt = Clock::now();
        const std::int64_t version { document != nullptr ? document->version : 0 };
        for (engine::Engine* merger : selection.mergers) {
            const std::string engineId { merger->id() };
            engine::Reply reply { [this, jobId, engineId](engine::Answer answer) { tokens_answer(jobId, engineId, std::move(answer)); } };
            if (merger == coreEngine) {
                if (kept != nullptr) kept->asking = version;
                reply = [this, jobId, engineId, uri, version](engine::Answer answer) {
                    if (answer.kind == engine::Answer::Kind::result) {
                        auto core = tokens::remap(answer.value, tokensLegend);
                        record_core_tokens(uri, answer.version.value_or(version), core);
                        if (const auto it = jobs.find(jobId); it != jobs.end() && it->second.tokens) it->second.tokens->core = std::move(core);
                        answer.value = nullptr;
                    } else if (highlights::Kept* asked = highlights.find(uri)) {
                        asked->asking.reset();
                    }
                    tokens_answer(jobId, engineId, std::move(answer));
                };
            }
            merger->request(job.view, job.message, std::move(reply));
            if (!jobs.contains(jobId)) return;
        }
    }

    void tokens_answer(std::uint64_t jobId, const std::string& engineId, engine::Answer answer) {
        auto it = jobs.find(jobId);
        if (it == jobs.end()) return;
        Job& job = it->second;
        TokensRequest& request { *job.tokens };
        switch (answer.kind) {
        case engine::Answer::Kind::result:
            if (!answer.value.is_null()) request.answered = true;
            if (coreEngine == nullptr || engineId != coreEngine->id()) {
                auto native = tokens::decode(answer.value);
                tokens::ensure_sorted(native);
                if (request.native.empty()) request.native = std::move(native);
                else request.native = tokens::merge_tokens(request.native, native);
            }
            break;
        case engine::Answer::Kind::unavailable: break;
        case engine::Answer::Kind::error:
            if (!job.error) job.error = std::move(answer.value);
            break;
        case engine::Answer::Kind::cancelled: job.cancelled = true; break;
        }
        if (--job.awaiting > 0) return;
        job.engineRepliedAt = Clock::now();
        if (job.cancelled) {
            finish_job_cancelled(jobId);
        } else if (job.error && !request.fromKept && !request.core) {
            finish_job_with_error(jobId, std::move(*job.error));
        } else {
            finish_tokens_job(jobId);
        }
    }

    // The answer: the compiler's tokens (what is kept, moved to the document's current version, which is what the client
    // shows; else this request's own answer) with mcppls's own in the gaps, cut to the range asked for.
    void finish_tokens_job(std::uint64_t jobId) {
        auto it = jobs.find(jobId);
        if (it == jobs.end()) return;
        Job& job = it->second;
        TokensRequest& request { *job.tokens };
        const auto began = Clock::now();
        highlights::Kept* kept { highlights.find(request.uri) };
        const bool fromKept { kept != nullptr && kept->known };
        if (!fromKept && !request.core && !request.answered) {
            job.answeredBy = "merged";
            finish_job(jobId, Json(nullptr));
            return;
        }
        const std::vector<tokens::Token>& core { fromKept ? kept->core : request.core ? *request.core : request.native };
        std::vector<tokens::Token> merged { &core == &request.native ? request.native : tokens::merge_tokens(core, request.native) };
        if (request.kind == TokensRequest::Kind::range && request.range) merged = highlights::within(merged, *request.range);
        std::vector<std::uint32_t> data { tokens::encode_data(merged) };
        Json result;
        if (request.kind == TokensRequest::Kind::range || kept == nullptr) {
            result = tokens::result_of(data);
        } else if (request.kind == TokensRequest::Kind::delta && !request.previousResultId.empty() && request.previousResultId == kept->resultId) {
            // H3: the edits of the client's last answer, under a new resultId.
            Json edits = highlights::edits_between(kept->sent, data);
            ++highlights.counters.deltas;
            result = Json { { "resultId", highlights.remember(request.uri, std::move(data)) }, { "edits", std::move(edits) } };
        } else {
            const std::string resultId { highlights.remember(request.uri, data) };
            result = tokens::result_of(kept->sent, resultId);
        }
        if (const Document* document { documents_.find(request.uri) }; kept != nullptr && document != nullptr && request.kind != TokensRequest::Kind::range) {
            kept->givenVersion = document->version;
            kept->givenDigest = highlights::digest(fromKept && kept->version == document->version ? kept->core : std::span<const tokens::Token> {});
            kept->givenExact = fromKept && kept->exact;
        }
        highlights.composed(std::chrono::duration<double, std::milli>(Clock::now() - began).count());
        job.answeredBy = request.fromKept ? "kept" : "merged";
        finish_job(jobId, std::move(result));
    }

    // The core engine's tokens for `uri`, answered for `answered` (the version it had): kept, moved to the document's
    // version now. A client given something else for that version is told to ask again; one that holds a stand-in for
    // a newer text has the compiler asked again, so what it is shown converges on the compiler's once typing pauses.
    void record_core_tokens(const std::string& uri, std::int64_t answered, std::vector<tokens::Token> core) {
        const Document* document { documents_.find(uri) };
        highlights::Kept* kept { highlights.find(uri) };
        if (document == nullptr || kept == nullptr) return;
        kept->asking.reset();
        const bool settled { coreEngine == nullptr || !coreEngine->busy_with(document->path) };
        const std::size_t count { core.size() };
        if (!highlights.engine_answered(uri, answered, document->version, std::move(core), settled)) {
            log::debug("semantic tokens of {}: the core engine's answer for version {} is dropped (now {}, its edits are no longer known)",
                       document->path, answered, document->version);
            return;
        }
        log::debug("semantic tokens of {}: the core engine answered {} tokens for version {} (now {}; {})", document->path, count, answered,
                   document->version, kept->exact ? "exact" : settled ? "moved" : "still preparing");
        if (kept->givenVersion && *kept->givenVersion == kept->version && kept->givenDigest != highlights::digest(kept->core)) {
            ++highlights.counters.refreshes;
            request_tokens_refresh(TOKENS_REFRESH_SOON);
        }
        if (kept->exact && !kept->edited) write_kept_tokens(*document, *kept);
        if (!kept->exact && answered != document->version && clientSupportsTokensRefresh) ask_core_tokens(uri);
    }

    // H2: where a document's tokens are kept on disk, and the key they are kept under -- a digest of everything the
    // compiler's answer depends on: this server and the compiler, the semantic profile, the plan's command for the file,
    // the module interface units it reads (transitively; an open one by its text, the rest by size and time), and the text.
    // Without a plan entry for the file there is no command to name, so no key yet.
    std::string tokens_directory() const { return base::join_path(workspaceDirectory_, "tokens"); }

    std::optional<std::string> tokens_cache_key(const Document& document) const {
        const auto command = planCommands_.find(base::path_key(document.path));
        if (command == planCommands_.end()) return std::nullopt;
        base::Sha256 hash;
        hash.update(std::format("mcppls-tokens 1\n{}\n{}\n{}\n", base::VERSION, payload.clangdVersion, profile_json().dump()));
        hash.update(command->second);
        hash.update("\n");
        // The module interface units the file reads, and theirs: each by name and identity, in name order.
        std::map<std::string, std::string, std::less<>> modules;
        std::vector<std::pair<std::string, std::string>> pending;   // (importer, module)
        for (const auto& name : imports_of(document.path)) pending.emplace_back(document.path, name);
        while (!pending.empty() && modules.size() < 1024) {
            auto [importer, name] = std::move(pending.back());
            pending.pop_back();
            if (modules.contains(name)) continue;
            std::string identity;
            const auto units = index.providers_for(importer, name);
            for (const auto& unit : units) {
                if (const Document* open { documents_.find_by_path(unit.path) }) identity += std::format("{} text {}\n", unit.path, base::sha256_hex(open->text));
                else if (const auto stamp = base::file_stamp(unit.path)) identity += std::format("{} {} {}\n", unit.path, stamp->size, static_cast<std::int64_t>(stamp->modified_ns));
                for (const auto& imported : imports_of(unit.path)) pending.emplace_back(unit.path, imported);
            }
            if (units.empty()) {
                if (const auto* external = index.external(name)) {
                    const auto stamp = base::file_stamp(external->path);
                    identity = std::format("{} {} {}\n", external->path, stamp ? stamp->size : 0, stamp ? static_cast<std::int64_t>(stamp->modified_ns) : 0);
                }
            }
            modules.emplace(std::move(name), std::move(identity));
        }
        for (const auto& [name, identity] : modules) hash.update(std::format("module {}\n{}", name, identity));
        hash.update(base::sha256_hex(document.text));
        return hash.finish();
    }

    // H2: the tokens the disk keeps for this document's text, read once per open. A hit is exact: the key says the
    // compiler would answer the same; it is asked once all the same, to confirm.
    void read_kept_tokens(const Document& document, highlights::Kept& kept) {
        const auto key = tokens_cache_key(document);
        if (!key) return;   // no plan entry yet: asked again at the next request
        kept.cacheChecked = true;
        const std::string file { base::join_path(tokens_directory(), highlights::cache_file_name(base::path_key(document.path))) };
        const auto bytes = os::fs::read(file);
        const auto cached = bytes ? highlights::decode_cache(*bytes) : std::nullopt;
        if (!cached || cached->key != *key) {
            ++highlights.counters.cacheMisses;
            return;
        }
        ++highlights.counters.cacheHits;
        kept.core = std::move(cached->tokens);
        kept.known = true;
        kept.exact = true;
        kept.fromCache = true;
        kept.version = document.version;
        kept.writtenKey = *key;
        log::debug("semantic tokens of {}: {} tokens kept for this text were read", document.path, kept.core.size());
    }

    // H2: the compiler's exact tokens of a text the disk has (opened or saved, not edited since), written once per key.
    void write_kept_tokens(const Document& document, highlights::Kept& kept) {
        std::error_code ignored;
        const auto key = tokens_cache_key(document);
        if (!key || *key == kept.writtenKey) return;
        const std::string directory { tokens_directory() };
        (void)std::filesystem::create_directories(directory, ignored);
        const std::string file { base::join_path(directory, highlights::cache_file_name(base::path_key(document.path))) };
        if (auto written = os::fs::write_atomic(file, highlights::encode_cache(highlights::CacheFile { *key, kept.core })); !written) {
            log::debug("semantic tokens of {} were not kept on disk: {}", document.path, written.error().message());
            return;
        }
        kept.writtenKey = *key;
        ++highlights.counters.cacheWrites;
        if (++tokensWritten % 32 == 1) (void)highlights::prune_cache(directory, highlights::CACHE_FILES, highlights::CACHE_BYTES);
    }

    // The core engine asked for `uri`'s tokens on the server's own behalf (H1): one request at a time per document.
    void ask_core_tokens(const std::string& uri) {
        const Document* document { documents_.find(uri) };
        highlights::Kept* kept { highlights.find(uri) };
        if (coreEngine == nullptr || document == nullptr || kept == nullptr || kept->asking || core_waits_for_producer()) return;
        const std::string path { path_of_uri(uri) };
        Json message = lsp::make_request(std::format("t:{}", ++tokensAsked), lsp::method::TEXT_DOCUMENT_SEMANTIC_TOKENS_FULL,
                                         Json { { "textDocument", Json { { "uri", uri } } } });
        const engine::RequestView view { lsp::method::TEXT_DOCUMENT_SEMANTIC_TOKENS_FULL, &message["params"], path, document->text };
        if (!coreEngine->claims(view)) return;
        const std::int64_t version { document->version };
        kept->asking = version;
        log::debug("semantic tokens of {}: the core engine is asked at version {}", path, version);
        coreEngine->request(view, message, [this, uri, version](engine::Answer answer) {
            if (answer.kind == engine::Answer::Kind::result) {
                record_core_tokens(uri, answer.version.value_or(version), tokens::remap(answer.value, tokensLegend));
                return;
            }
            highlights::Kept* asked { highlights.find(uri) };
            if (asked == nullptr) return;
            asked->asking.reset();
            // The text changed while it was asked (clangd answers such a request "content modified"): asked again for the
            // text now. Otherwise (set aside, restarted, not accepting yet) the engine's next change refreshes and asks again.
            if (const Document* now { documents_.find(uri) }; now != nullptr && now->version != version) ask_core_tokens(uri);
        });
    }

    // ---- the answer layer -----------------------------------------------------------------------------------------

    // How long a request with a kept answer waits for the core engine (answer-layer design 4.6): the answer budget,
    // unless the engine has lately answered this file's completions within their regular budget -- then a little past
    // the slowest of those, so its complete list is what goes out.
    std::chrono::milliseconds kept_answer_wait(const Job& job) const {
        const std::chrono::milliseconds budget { answers_->settings().budget };
        if (job.method != lsp::method::TEXT_DOCUMENT_COMPLETION && job.method != lsp::method::TEXT_DOCUMENT_SIGNATURE_HELP) return budget;
        const auto pace = completionPace_.find(job.path);
        const auto regular = request_budget(job);
        if (pace == completionPace_.end() || !regular) return budget;
        const auto usual = pace->second.usual(Clock::now());
        if (!usual || *usual + KEPT_ANSWER_MARGIN > *regular) return budget;
        return std::max(budget, *usual + KEPT_ANSWER_MARGIN);
    }
    static constexpr std::chrono::milliseconds KEPT_ANSWER_MARGIN { 150 };

    // A file's visibility fingerprint (answer-layer design 4.3): the engine and the profile, the plan's command for it,
    // what it imports, and its preamble.
    std::string answer_fingerprint(const std::string& path, std::string_view text) const {
        const std::string toolchain { std::format("{}|{}", payload.clangdVersion, profile_json().dump()) };
        std::vector<std::string> command;
        if (const auto it = planCommands_.find(base::path_key(path)); it != planCommands_.end()) command.push_back(it->second);
        return answering::fingerprint(toolchain, command, imports_of(path), text);
    }

    // The job's kept answer, composed and sent, with the core engine's request (if one was sent) going on in the
    // background. False when there is nothing to send (a jump that is not certain and was not found again).
    bool serve_kept(std::uint64_t jobId) {
        Job& job = jobs.at(jobId);
        if (!job.asked || !job.found) return false;
        auto served = answers_->serve(*job.asked, *job.found);
        if (!served) return false;
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - job.started);
        answers_->count_served(*job.asked, *served, took);
        const std::string line { answering::Layer::log_line(*job.asked, *served, took) };
        if (served->provenance.certainty == answer::Certainty::uncertain || served->doubted > 0) log::info("{}", line);
        else log::debug("{}", line);
        const bool coreAsked { !job.waitsForLate && job.next > 0 && job.answerers[job.next - 1] == coreEngine };
        if (coreAsked) {
            keep_in_background(jobId, *job.asked, *served, job.clientId);
            coreEngine->detach(job.clientId);
            for (const auto& engine : engines) {
                if (engine.get() != coreEngine) engine->cancel(job.clientId);
            }
        }
        job.answeredBy = "cache";
        job.coreAnswered = true;   // the file's words are not added to a kept answer
        finish_job(jobId, std::move(served->result), served->provenance);
        return true;
    }

    // The core engine's request for `jobId` goes on after the client has its answer (or cancelled): its answer is kept.
    void keep_in_background(std::uint64_t jobId, const answering::Asked& asked, std::optional<answering::Served> served, const Json& clientId) {
        while (background_.size() >= BACKGROUND_LIMIT) {
            const auto oldest = std::ranges::min_element(background_, {}, [](const auto& entry) { return entry.second.started; });
            if (coreEngine != nullptr && !lateCompletions.contains(oldest->first)) coreEngine->cancel(oldest->second.clientId);
            if (const auto inflight = answerInflight_.find(oldest->second.asked.k1); inflight != answerInflight_.end() && inflight->second == oldest->first) {
                answerInflight_.erase(inflight);
            }
            answers_->count_background("dropped");
            background_.erase(oldest);
        }
        const Document* document { documents_.find(asked.uri) };
        const bool trigger { asked.context.place == answer::Context::Place::member || asked.context.place == answer::Context::Place::arrow
                             || asked.context.place == answer::Context::Place::qualified };
        const std::size_t items { served && served->result.is_object() && served->result.contains("items") && served->result["items"].is_array()
                                      ? served->result["items"].size() : 0 };
        // (Almost) nothing went out: no kept answer, only the file's words, and fewer than three of them.
        const bool sparse { asked.kind == answer::Kind::completion && trigger && items < 3 && (!served || served->provenance.source == answer::Source::native) };
        background_[jobId] = Background { asked, std::move(served), clientId, jobs.at(jobId).started, Clock::now(), Clock::now() + BACKGROUND_KEEP,
                                          document != nullptr ? document->version : 0, sparse };
        answerInflight_[asked.k1] = jobId;
        answers_->count_background("detached");
    }

    // The core engine answered a request that went on in the background.
    void background_answered(std::uint64_t jobId, const engine::Answer& answer) {
        const auto it = background_.find(jobId);
        if (it == background_.end()) return;
        Background background { std::move(it->second) };
        background_.erase(it);
        if (const auto inflight = answerInflight_.find(background.asked.k1); inflight != answerInflight_.end() && inflight->second == jobId) {
            answerInflight_.erase(inflight);
        }
        if (answer.kind != engine::Answer::Kind::result || !keepable()) {
            answers_->count_background("dropped");
            return;
        }
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - background.requestStarted);
        // A kept answer does not finish the engine's work. Its eventual full latency must
        // teach the same pace as a foreground answer, rather than relearning only its tail.
        if (background.asked.kind == answer::Kind::completion || background.asked.kind == answer::Kind::signature_help) {
            note_core_completion(background.asked.path, background.requestStarted);
        }
        answers_->keep(background.asked, answer.value, background.served, took);
        answers_->count_engine(background.asked, took);
        answers_->count_background("answered");
        if (background.asked.kind == answer::Kind::completion) {
            note_word_answer_at(background.asked.uri, background.asked.text, background.asked.position, answer.value);
        }
        learn_receiver_type(background.asked);
        // R3 (S3-5.9-1): the person was given (almost) nothing after a trigger and has not moved: the list appears now,
        // for a client that said it asks again on this; others ask again as the person types.
        const Document* document { documents_.find(background.asked.uri) };
        const Json* ready { lsp::find_path(clientParams, { "capabilities", "experimental", "cxxModules", "completionReady" }) };
        if (background.sparse && ready != nullptr && ready->is_boolean() && ready->get<bool>() && document != nullptr && document->version == background.version
            && !completion::is_empty(answer.value) && Clock::now() - background.started <= READY_NOTICE_WITHIN) {
            client.notify("cxxModules/completionReady", Json { { "uri", background.asked.uri }, { "version", background.version },
                                                            { "position", Json { { "line", background.asked.position.line },
                                                                                 { "character", background.asked.position.character } } } });
        }
    }

    // Whether an answer the core engine gives now is one to keep (design 4.10): not while it cannot serve as it should.
    bool keepable() const {
        if (coreEngine == nullptr) return false;
        const engine::EngineStatus status { coreEngine->status() };
        return !status.failed && status.state != "unavailable";
    }

    // The engine answered a request a person waits for: what it said is kept.
    void keep_answer(const Job& job, const Json& result) {
        if (!job.asked || !keepable()) return;
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - job.started);
        answers_->keep(*job.asked, result, std::nullopt, took);
        answers_->count_engine(*job.asked, took);
        learn_receiver_type(*job.asked);
    }

    // A request mcppls makes of the core engine itself (learning, prefetching): its answer goes to `done`, never to a client.
    void ask_core_internally(std::string method, Json params, const std::string& path, const std::string& text,
                             std::function<void(std::optional<Json>)> done) {
        if (coreEngine == nullptr) {
            done(std::nullopt);
            return;
        }
        const Json message = lsp::make_request(std::format("mcppls:answer:{}", ++internalRequests_), method, std::move(params));
        const engine::RequestView view { method, &message["params"], path, text };
        coreEngine->request(view, message, [done = std::move(done)](engine::Answer answer) {
            done(answer.kind == engine::Answer::Kind::result ? std::optional<Json> { std::move(answer.value) } : std::nullopt);
        });
    }

    // After a completion at `receiver.`: what type the receiver has, asked of the engine once, so the answer is kept for
    // the type and every file that has a receiver of it gets it (design 4.2, K2).
    void learn_receiver_type(const answering::Asked& asked) {
        if (asked.kind != answer::Kind::completion || learning_ >= 2) return;
        if (asked.context.place != answer::Context::Place::member && asked.context.place != answer::Context::Place::arrow) return;
        if (answers_->knows_type(asked)) return;
        // The receiver's last character, before the `.` or `->`.
        std::size_t at { asked.context.wordStart };
        while (at > 0 && (asked.text[at - 1] == ' ' || asked.text[at - 1] == '\t')) --at;
        if (at == 0) return;
        at -= asked.context.place == answer::Context::Place::arrow ? 2 : 1;
        while (at > 0 && (asked.text[at - 1] == ' ' || asked.text[at - 1] == '\t')) --at;
        if (at == 0 || !base::is_identifier_char(asked.text[at - 1])) return;   // `f().`, `a[0].`: no name to ask about
        const base::Position position { base::position_at(asked.text, at - 1) };
        ++learning_;
        ask_core_internally("textDocument/typeDefinition",
                            Json { { "textDocument", Json { { "uri", asked.uri } } }, { "position", Json { { "line", position.line }, { "character", position.character } } } },
                            asked.path, asked.text, [this, asked](std::optional<Json> result) {
                                --learning_;
                                if (!result) return;
                                const Json* location { result->is_array() ? (result->empty() ? nullptr : &(*result)[0]) : result->is_object() ? &*result : nullptr };
                                if (location == nullptr) return;
                                const std::string uri { location->contains("targetUri") ? location->value("targetUri", std::string {}) : location->value("uri", std::string {}) };
                                const Json* range { location->contains("targetSelectionRange") ? &(*location)["targetSelectionRange"]
                                                                                              : location->contains("range") ? &(*location)["range"] : nullptr };
                                if (uri.empty() || range == nullptr || !range->contains("start")) return;
                                const std::string path { path_of_uri(uri) };
                                if (path.empty()) return;
                                std::string text;
                                if (const Document* open = documents_.find_by_path(path)) text = open->text;
                                else if (auto disk = os::fs::read(path)) text = std::move(*disk);
                                const base::Position start { (*range)["start"].value("line", 0), (*range)["start"].value("character", 0) };
                                const auto offset = base::offset_at(text, start);
                                if (!offset) return;
                                std::size_t end { *offset };
                                while (end < text.size() && base::is_identifier_char(text[end])) ++end;
                                if (end == *offset) return;
                                answers_->learn_type(asked, std::string_view { text }.substr(*offset, end - *offset), path, start.line);
                            });
    }

    // While the engine is idle (design 4.8): the completion at one of the `.`, `->`, `::` the file being edited has,
    // whose answer is not kept yet.
    void prefetch_due(Clock::time_point now) {
        prefetchAt_.reset();
        if (!answers_ || !answers_->enabled() || !answers_->settings().prefetch || coreEngine == nullptr || prefetchInFlight_ || prefetchStopped_) return;
        if (!lastClientRequestAt_ || lastAskedUri_.empty()) return;
        if (now - *lastClientRequestAt_ < std::chrono::seconds { 2 } || !jobs.empty() || !background_.empty() || learning_ > 0) {
            prefetchAt_ = now + std::chrono::seconds { 2 };
            return;
        }
        while (!prefetched_.empty() && now - prefetched_.front() > std::chrono::minutes { 1 }) prefetched_.pop_front();
        if (prefetched_.size() >= PREFETCH_PER_MINUTE) {
            prefetchAt_ = prefetched_.front() + std::chrono::minutes { 1 };
            return;
        }
        const engine::EngineStatus status { coreEngine->status() };
        // A core engine that restarted since prefetching began may have stopped on a question nobody else asks: none
        // more this session (the person's own requests still keep and give answers).
        if (!prefetchRestartsAt_) prefetchRestartsAt_ = status.restarts;
        if (status.restarts != *prefetchRestartsAt_) {
            log::info("answers: no more prefetching in {} this session: clangd restarted since it began", root);
            prefetchStopped_ = true;
            return;
        }
        if (status.state != "ready" || status.preparing) {
            prefetchAt_ = now + std::chrono::seconds { 5 };
            return;
        }
        const Document* document { documents_.find(lastAskedUri_) };
        if (document == nullptr || document->path.empty()) return;
        const std::string fingerprint { answer_fingerprint(document->path, document->text) };
        const auto place = answers_->prefetch_place(document->uri, document->text, lastAskedPosition_, fingerprint);
        if (!place) {
            prefetchExhaustedFor_ = lastClientRequestAt_;   // everything near is kept: nothing more until the person asks again
            return;
        }
        const Json params { { "textDocument", Json { { "uri", document->uri } } }, { "position", Json { { "line", place->line }, { "character", place->character } } },
                            { "context", Json { { "triggerKind", 1 } } } };
        auto asked = answers_->read("textDocument/completion", document->uri, document->path, document->text, params, fingerprint);
        if (!asked) return;
        prefetchInFlight_ = true;
        prefetched_.push_back(now);
        answers_->count_background("prefetched");
        ask_core_internally("textDocument/completion", params, document->path, document->text, [this, asked = std::move(*asked)](std::optional<Json> result) {
            prefetchInFlight_ = false;
            prefetchAt_ = Clock::now() + std::chrono::milliseconds { 500 };
            if (!result || !answer::trim(answer::Kind::completion, *result)) {
                answers_->prefetch_came_back_empty(asked);
                return;
            }
            if (!keepable()) return;
            answers_->keep(asked, *result, std::nullopt, std::chrono::milliseconds { 0 });
            learn_receiver_type(asked);
        });
    }

    // A client's $/cancelRequest for a request the answer layer can still use: the client is answered `cancelled`, and
    // the core engine's request goes on so its answer is kept ("typed, then deleted": the next time, at once).
    bool cancel_into_background(const Json& clientId) {
        if (!answers_ || !answers_->enabled() || coreEngine == nullptr) return false;
        for (auto& [jobId, job] : jobs) {
            if (job.clientId != clientId) continue;
            if (!job.asked || job.merging || job.waitsForLate || job.next == 0 || job.answerers[job.next - 1] != coreEngine) return false;
            keep_in_background(jobId, *job.asked, std::nullopt, clientId);
            coreEngine->detach(clientId);
            for (const auto& engine : engines) {
                if (engine.get() != coreEngine) engine->cancel(clientId);
            }
            finish_job_cancelled(jobId);
            return true;
        }
        return false;
    }

    // mcpp-community/mcpp#797: mcpp's engine interface is written by mcpp, once for each of its versions; a request
    // that would edit it is answered without an edit. Whether `jobId`, a request about such a file, was answered so.
    bool refuse_edit(std::uint64_t jobId) {
        const std::string& method { jobs.at(jobId).method };
        if (method == "textDocument/formatting" || method == "textDocument/rangeFormatting" || method == "textDocument/rangesFormatting"
            || method == "textDocument/onTypeFormatting" || method == "textDocument/codeAction") {
            finish_job(jobId, Json::array());
            return true;
        }
        if (method == "textDocument/prepareRename") {
            finish_job(jobId, Json(nullptr));
            return true;
        }
        if (method == "textDocument/rename") {
            finish_job_with_error(jobId, read_only_error(jobs.at(jobId).path));
            return true;
        }
        return false;
    }

    void fail_rename_ownership(std::uint64_t jobId, std::string_view reason) {
        finish_job_with_error(jobId, Json { { "code", lsp::REQUEST_FAILED },
            { "message", std::format("Cannot confirm rename ownership: {}", reason) } });
    }

    // A compiler can return only the editable references of an imported symbol. Resolve its
    // definition and declaration owners before asking for edits, rather than treating
    // that partial edit as proof of safety. Both lookups share the original deadline.
    void preflight_rename(std::uint64_t jobId, bool declaration = false,
                          std::optional<os::fs::stamp> closedStamp = std::nullopt) {
        Job& job = jobs.at(jobId);
        const std::string uri { uri_of_params(job.params) };
        const Document* document { documents_.find(uri) };
        if (coreEngine == nullptr || !position_of(job.params)) {
            fail_rename_ownership(jobId, "the symbol's document or compiler is unavailable");
            return;
        }
        if (!declaration && document == nullptr) closedStamp = base::file_stamp(job.path);
        if (declaration && closedStamp && document != nullptr) {
            fail_rename_ownership(jobId, "the closed document was opened while resolving the symbol");
            return;
        }
        if (document == nullptr && !closedStamp) {
            fail_rename_ownership(jobId, "the closed document has no stable file stamp");
            return;
        }
        const std::optional<std::int64_t> version = document != nullptr
            ? std::optional<std::int64_t> { document->version } : std::nullopt;
        const auto generation = modelGeneration;
        const auto ownerModel = model;
        job.renamePreflight = true;
        if (!declaration) job.budgetAt = job.started + *answer_budget("textDocument/definition");
        const std::string method { declaration ? "textDocument/declaration" : "textDocument/definition" };
        Json params = job.params;
        params.erase("newName");
        const Json request = lsp::make_request(job.clientId, method, params);
        const engine::RequestView view { method, &params, job.path, job.text };
        coreEngine->request(view, request, [this, jobId, uri, version, generation, ownerModel, declaration, closedStamp](engine::Answer answer) {
            const auto current = jobs.find(jobId);
            if (current == jobs.end() || !current->second.renamePreflight) return;
            if (!current->second.budgetAt || Clock::now() >= *current->second.budgetAt) {
                fail_rename_ownership(jobId, "symbol ownership lookup timed out");
                return;
            }
            if (answer.kind == engine::Answer::Kind::cancelled) {
                finish_job_cancelled(jobId);
                return;
            }
            const Document* now = documents_.find(uri);
            const bool sameDocument { version ? now != nullptr && now->version == *version && now->text == current->second.text
                : now == nullptr && closedStamp && base::file_stamp(current->second.path) == closedStamp };
            if (generation != modelGeneration || ownerModel != model || !sameDocument) {
                fail_rename_ownership(jobId, "the document or project changed while resolving the symbol");
                return;
            }
            if (answer.kind != engine::Answer::Kind::result) {
                fail_rename_ownership(jobId, "the compiler did not resolve the symbol");
                return;
            }
            const auto range_valid = [](const Json* range) {
                if (range == nullptr || !range->is_object()) return false;
                const auto start = range->find("start"), end = range->find("end");
                if (start == range->end() || end == range->end() || !start->is_object() || !end->is_object()) return false;
                const auto sl = lsp::int_at(*start, "line"), sc = lsp::int_at(*start, "character");
                const auto el = lsp::int_at(*end, "line"), ec = lsp::int_at(*end, "character");
                return sl && sc && el && ec && *sl >= 0 && *sc >= 0 && *el >= 0 && *ec >= 0
                    && std::pair { *sl, *sc } <= std::pair { *el, *ec };
            };
            std::vector<std::string> owners;
            const auto location = [&](const Json& entry) {
                if (!entry.is_object()) return false;
                const bool link { entry.contains("targetUri") };
                const Json* target = lsp::find(entry, link ? "targetUri" : "uri");
                if (target == nullptr || !target->is_string() || !range_valid(lsp::find(entry, link ? "targetRange" : "range"))
                    || (link && !range_valid(lsp::find(entry, "targetSelectionRange")))) return false;
                if (link) {
                    const auto point = [](const Json& position) {
                        return std::pair { *lsp::int_at(position, "line"), *lsp::int_at(position, "character") };
                    };
                    const Json& range { entry["targetRange"] };
                    const Json& selection { entry["targetSelectionRange"] };
                    if (point(selection["start"]) < point(range["start"]) || point(range["end"]) < point(selection["end"])) return false;
                }
                const std::string path { path_of_uri(target->get<std::string>()) };
                if (path.empty()) return false;
                owners.push_back(path);
                return true;
            };
            bool resolved { false };
            if (answer.value.is_array()) {
                resolved = !answer.value.empty();
                for (const auto& entry : answer.value) if (!location(entry)) resolved = false;
            } else {
                resolved = location(answer.value);
            }
            if (!resolved) {
                fail_rename_ownership(jobId, "the compiler returned no valid definition location");
                return;
            }
            for (const auto& path : owners) {
                if (readOnlySources.contains(base::path_key(path))) {
                    finish_job_with_error(jobId, read_only_error(path));
                    return;
                }
            }
            if (!declaration) {
                preflight_rename(jobId, true, closedStamp);
                return;
            }
            current->second.renamePreflight = false;
            current->second.budgetAt.reset();
            ask_next(jobId);
        });
    }

    Json read_only_error(std::string_view path) const {
        return Json { { "code", lsp::REQUEST_FAILED },
                      { "message", std::format("{} is mcpp's engine interface, which mcpp writes for each of its versions and which is not edited",
                                               base::file_name(path)) } };
    }

    // The first file of mcpp's engine interface a workspace edit changes, if any.
    std::optional<std::string> read_only_target(const Json& edit) const {
        if (!edit.is_object() || readOnlySources.empty()) return std::nullopt;
        const auto check = [&](const std::string& uri) -> std::optional<std::string> {
            const std::string path { path_of_uri(uri) };
            if (!path.empty() && readOnlySources.contains(base::path_key(path))) return path;
            return std::nullopt;
        };
        if (const auto changes = edit.find("changes"); changes != edit.end() && changes->is_object()) {
            for (auto change = changes->begin(); change != changes->end(); ++change) {
                if (auto path = check(change.key())) return path;
            }
        }
        if (const auto changes = edit.find("documentChanges"); changes != edit.end() && changes->is_array()) {
            for (const auto& change : *changes) {
                const Json* document { lsp::find(change, "textDocument") };
                const Json* uri { document != nullptr ? lsp::find(*document, "uri") : lsp::find(change, "uri") };
                if (uri != nullptr && uri->is_string()) {
                    if (auto path = check(uri->get<std::string>())) return path;
                }
            }
        }
        return std::nullopt;
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

    // R-7 (plan 2026-09-30), with UP-25's amendment: the flat budget of the method -- except that a file whose
    // core-engine completion answers keep landing just past it waits longer, since there the flat budget cancelled
    // every answer the engine was about to give, and after `.`, `->` or `::` the fallback is empty by design.
    std::optional<std::chrono::milliseconds> request_budget(const Job& job) const {
        const auto flat = answer_budget(job.method);
        if (!flat) return std::nullopt;
        if (job.method != lsp::method::TEXT_DOCUMENT_COMPLETION && job.method != lsp::method::TEXT_DOCUMENT_SIGNATURE_HELP) {
            return flat;
        }
        const auto pace = completionPace_.find(job.path);
        if (pace == completionPace_.end()) return flat;
        return pace->second.budget(Clock::now(), *flat, ADAPTIVE_COMPLETION_BUDGET);
    }

    // The person asked at `job.started` and the budget is counted from there, so the pace measures the same span.
    void note_core_completion(const std::string& path, Clock::time_point started) {
        if (path.empty()) return;
        if (completionPace_.size() >= FILE_STATS_LIMIT && !completionPace_.contains(path)) return;
        completionPace_[path].answered(Clock::now(),
                                       std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started),
                                       ADAPTIVE_COMPLETION_BUDGET);
    }

    // R-7 (plan 2026-09-30): the core engine has not answered in the request's budget (answer_budget). mcppls answers
    // with what it has -- a completion the file's words as an incomplete list, with the keywords (F15) merged in by
    // finish_job; a hover what explain_if_preparing says -- and the core engine's request is cancelled. The job is
    // finished first, so the engines' answers to the cancellation find nothing to finish. The core engine's own
    // timeouts and watchdogs still see the request until clangd lets go of it.
    void answer_without_core(std::uint64_t jobId) {
        if (jobs.at(jobId).renamePreflight) {
            const Json id = jobs.at(jobId).clientId;
            fail_rename_ownership(jobId, "symbol ownership lookup timed out");
            if (coreEngine != nullptr) coreEngine->cancel(id);
            return;
        }
        if (jobs.at(jobId).found && serve_kept(jobId)) return;
        Job& job = jobs.at(jobId);
        const Json clientId = job.clientId;   // `=`: braces make an array holding it (CWG 2137)
        if (job.keywords.is_array() && !job.keywords.empty()) ++keywordsWithoutEngine;
        if (job.templates.is_array() && !job.templates.empty()) ++templatesWithoutEngine;
        job.answeredBy = "mcppls";
        job.withoutCore = true;
        Json result;
        bool keepCore { false };
        if (job.method == lsp::method::TEXT_DOCUMENT_COMPLETION) {
            const auto position = position_of(job.params);
            result = completion::without_engine(position ? completion::document_words(job.text, *position) : Json::array());
            // C-2: the core engine's request stays, for the requests the same word makes next.
            const bool coreAsked { !job.waitsForLate && job.next > 0 && job.answerers[job.next - 1] == coreEngine };
            if (coreAsked && position) {
                if (auto key = completion::word_key(job.text, *position)) {
                    lateCompletions[jobId] = LateCompletion { uri_of_params(job.params), job.path, std::move(*key), clientId,
                                                              job.started, Clock::now() + LATE_COMPLETION_KEEP, {} };
                    keepCore = true;
                }
            }
        }
        // The answer layer: the engine's answer, late, is kept for the next request of the same place, whatever it was.
        const bool coreAsked { !job.waitsForLate && job.next > 0 && job.answerers[job.next - 1] == coreEngine };
        if (job.asked && coreAsked) {
            answering::Served words;
            words.result = result;
            words.provenance = answer::Provenance { answer::Source::native, answer::Certainty::uncertain, 0, false, true, {} };
            keep_in_background(jobId, *job.asked, std::move(words), clientId);
            keepCore = true;
        }
        finish_job(jobId, std::move(result));
        for (const auto& engine : engines) {
            if (keepCore && engine.get() == coreEngine) engine->detach(clientId);
            else engine->cancel(clientId);
        }
    }

    // 0.0.14 A1: a complete core answer, kept for its word.
    void note_word_answer(const Job& job, const Json& result) {
        if (const auto position = position_of(job.params)) note_word_answer_at(uri_of_params(job.params), job.text, *position, result);
    }

    // The same for a completion asked of `text` at `position` (also one answered in the background: the next letter of the
    // word is given the engine's complete list, as a late answer was in 0.0.14).
    void note_word_answer_at(const std::string& uri, std::string_view text, base::Position position, const Json& result) {
        if (!wordAnswersOn || !completion::is_complete(result) || completion::is_empty(result)) return;
        auto key = completion::word_key(text, position);
        const auto outside = completion::outside_word(text, position);
        if (!key || !outside) {
            wordAnswers.erase(uri);
            return;
        }
        wordAnswers[uri] = WordAnswer { std::move(*key), *outside, result, Clock::now() };
    }

    // 0.0.14 A1: a request typed on in the word of a complete core answer, with the document outside the word as it
    // was, is given that answer for its position.
    bool answered_from_word(std::uint64_t jobId, const std::string& uri, base::Position position) {
        if (!wordAnswersOn || coreEngine == nullptr) return false;
        const auto answer = wordAnswers.find(uri);
        if (answer == wordAnswers.end()) return false;
        Job& job = jobs.at(jobId);
        const auto key = completion::word_key(job.text, position);
        const auto outside = completion::outside_word(job.text, position);
        if (!key || !outside || !completion::typed_on(answer->second.key, *key) || *outside != answer->second.outside
            || Clock::now() - answer->second.at > WORD_ANSWER_KEPT) {
            wordAnswers.erase(answer);
            return false;
        }
        job.coreAnswered = true;
        job.answeredBy = std::string { coreEngine->id() };
        ++completionsFromWord;
        finish_job(jobId, completion::retarget(answer->second.result, position));
        return true;
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
                if (const auto budget = request_budget(job)) job.budgetAt = job.started + *budget;
                return true;
            } else {
                if (!background_.contains(it->first)) coreEngine->cancel(it->second.clientId);
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
        // The answer layer may also retain this C-2 request. Its background callback below
        // records that result once; with the layer off, C-2 still owns its pace sample.
        if (answer.kind == engine::Answer::Kind::result && !background_.contains(jobId)) note_core_completion(late.path, late.started);
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
        if (answerer == coreEngine && job.asked) answerInflight_.try_emplace(job.asked->k1, jobId);
        answerer->request(job.view, job.message, [this, jobId, engineId = std::string { answerer->id() }](engine::Answer answer) {
            auto current = jobs.find(jobId);
            if (current == jobs.end()) {
                late_completion_answered(jobId, answer);
                background_answered(jobId, answer);
                return;
            }
            if (current->second.asked) {
                if (const auto inflight = answerInflight_.find(current->second.asked->k1); inflight != answerInflight_.end() && inflight->second == jobId) {
                    answerInflight_.erase(inflight);
                }
            }
            switch (answer.kind) {
            case engine::Answer::Kind::result:
                if (coreEngine != nullptr && engineId == coreEngine->id()) {
                    current->second.coreAnswered = true;
                    const bool typing { current->second.method == lsp::method::TEXT_DOCUMENT_COMPLETION
                                        || current->second.method == lsp::method::TEXT_DOCUMENT_SIGNATURE_HELP };
                    if (typing) note_core_completion(current->second.path, current->second.started);
                    if (current->second.method == lsp::method::TEXT_DOCUMENT_COMPLETION) note_word_answer(current->second, answer.value);
                    keep_answer(current->second, answer.value);
                }
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
        if (!startup.firstCompletion && job.method == "textDocument/completion" && outcome == "result" && coreEngine != nullptr
            && job.answeredBy == coreEngine->id()) {
            startup.firstCompletion = since_start_();
        }
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
        // A completion a person notices is slow from two seconds on (0.0.17 M3, #59 U5/U6: a typing session's slowest answers were
        // 6-8 s, and nothing said where the time went); anything else from five. Where it went: waiting before an engine was asked
        // (queueMs), the engine (engineMs), and what came after its answer -- merging, the answer layer, the reply (answerMs).
        const bool completion { job.method == lsp::method::TEXT_DOCUMENT_COMPLETION };
        if (ms >= (completion ? 2000 : 5000)) {
            Json detail { { "method", job.method }, { "file", job.path }, { "ms", static_cast<std::int64_t>(ms) },
                          { "outcome", std::string { outcome } }, { "answeredBy", job.answeredBy } };
            const auto milliseconds = [](Clock::duration span) { return static_cast<std::int64_t>(std::chrono::duration<double, std::milli>(span).count()); };
            if (job.engineSentAt) detail["queueMs"] = milliseconds(*job.engineSentAt - job.started);
            if (job.engineSentAt && job.engineRepliedAt) {
                detail["engineMs"] = milliseconds(*job.engineRepliedAt - *job.engineSentAt);
                detail["answerMs"] = milliseconds(Clock::now() - *job.engineRepliedAt);
            }
            journal.add("slow-request", std::move(detail));
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

    void finish_job(std::uint64_t jobId, Json result, std::optional<answer::Provenance> provenance = std::nullopt) {
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
        // F15, S3-6.4: the module-syntax keywords and the code templates, with the engine's items, or alone
        // (and incomplete, so the client asks again) when the engine gave none.
        for (const Json* own : { &it->second.keywords, &it->second.templates }) {
            if (own->is_array() && !own->empty()) result = result.is_null() ? completion::keywords_only(*own) : completion::merge(result, *own);
        }
        result = explain_if_preparing(it->second, std::move(result));
        // A rename from a file of the project that reaches mcpp's engine interface (mcpp#797) is not made in part.
        if (it->second.method == "textDocument/rename") {
            if (const auto path = read_only_target(result)) {
                finish_job_with_error(jobId, read_only_error(*path));
                return;
            }
        }
        note_request(it->second, result.is_null() ? "empty" : "result");
        const Json id = it->second.clientId;
        const bool agent { it->second.agent };
        const bool withoutCore { it->second.withoutCore };
        const bool layered { answer::kind_of(it->second.method).has_value() };
        jobs.erase(it);
        // An agent (the MCP tools) is told where an answer came from (answer-layer design 6.2): one the answer layer
        // could have given, and one mcppls gave itself because the core engine did not answer in time.
        if (!agent || (!provenance && !layered && !withoutCore)) {
            client.reply(id, std::move(result));
            return;
        }
        if (!provenance) {
            provenance = answer::Provenance { withoutCore ? answer::Source::native : answer::Source::engine,
                                              withoutCore ? answer::Certainty::uncertain : answer::Certainty::authoritative, 0, false, false, {} };
            if (withoutCore) provenance->note = "The compiler did not answer in time; this comes from mcppls's own reading of the files.";
        }
        Json message = lsp::make_result(id, std::move(result));
        message["provenance"] = answer::to_json(*provenance);
        client.send(message);
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

    // A body save reaches the engine but does not change its database. F13 still needs a
    // new plan for a deferred stand-in, an import now on disk, or a conditional import the
    // producer omitted and the editing-source union would add. Compare fresh disk text;
    // a matching draft alone does not establish what clangd will read after an autosave.
    bool saved_structure_is_planned(std::string_view path) const {
        if (!model || !firstPlanWritten || plan.standInsDeferred) return false;
        const std::string key { base::path_key(path) };
        const auto known = structures.find(key);
        const auto* scan = index.scan_of(path);
        if (known == structures.end() || scan == nullptr || structure_of(*scan) != known->second) return false;
        const auto disk = os::fs::read(path);
        if (!disk || structure_of(project::scan_source(*disk)) != known->second) return false;
        const auto required = project::required_names(*scan);
        bool represented { false };
        for (const auto& entry : plan.entries) {
            if (base::path_key(entry.file) != key) continue;
            represented = true;
            if (std::ranges::any_of(required, [&](const std::string& name) {
                return std::ranges::find(entry.imports, name) == entry.imports.end();
            })) return false;
        }
        return represented;
    }

    // A C++ source of the workspace the plan has no unit for is planned at once (robustness design C2); the core engine
    // holds it until then. A new source of an inferred model is in the model reloaded for it.
    void note_opened(std::string_view path) {
        // A build program (mcpp-community/mcpp#797) is planned in any model: one no set describes is left unread.
        const bool program { project::is_build_program_source(path) };
        if (!model || (model->source == project::SourceKind::inferred && !program)) return;
        if (!(project::is_cxx_source_name(path) || program) || !base::is_within(path, root) || plannedFiles.contains(base::path_key(path))) return;
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
    std::string target_input_signature() const {
        std::string signature { options.mcppTarget };
        if (options.mcppTarget == "auto") {
            const auto path = base::join_path(root, "compile_commands.json");
            signature += "\n" + base::canonical_path(path);
            if (const auto stamp = base::file_stamp(path)) signature += std::format("\n{}:{}", stamp->size, static_cast<std::int64_t>(stamp->modified_ns));
            else signature += "\nabsent";
            if (const auto identity = os::fs::identity_of(path)) signature += std::format("\n{}:{}", identity->volume, identity->index);
        }
        return signature;
    }

    std::string producer_file_identity() const {
        if (producerPath.empty()) return {};
        std::string identity { base::canonical_path(producerPath) };
        if (const auto stamp = base::file_stamp(producerPath)) identity += std::format("\n{}:{}", stamp->size, static_cast<std::int64_t>(stamp->modified_ns));
        else identity += "\nabsent";
        if (const auto file = os::fs::identity_of(producerPath)) identity += std::format("\n{}:{}", file->volume, file->index);
        return identity;
    }

    bool refresh_target_selection() {
        const auto providers = allowed_providers();
        const auto detection = project::detect_project(root, options.database, providers, options.buildDiscovery != "off");
        if (!options.trusted || detection.kind != project::SourceKind::mcpp) {
            targetSelection.mode.clear();
            targetReadPending = false;
            targetRetryAt.reset();
            if (targetSelectionBusy) ++targetSelectionGeneration;
            targetSelectionBusy = false;
            targetSelectionInput.clear();
            targetLoadRequested = false;
            targetCacheDeferred = false;
            targetReadRetries = 0;
            targetIdentityAt.reset();
            return true;
        }
        if (options.mcppTarget == "auto") targetIdentityAt = Clock::now() + std::chrono::seconds { 30 };
        else targetIdentityAt.reset();
        const std::string input { target_input_signature() };
        if (input == targetSelectionInput) {
            targetReadPending = false;
            targetRetryAt.reset();
            return true;
        }
        if (targetSelectionBusy) return false; // one bounded parse at a time; its completion checks the latest stamp
        targetReadPending = true;
        targetSelectionBusy = true;
        const int generation { ++targetSelectionGeneration };
        const auto rootCopy = root;
        const auto selector = options.mcppTarget;
        const auto rootKey = key;
        const auto queue = events;
        // CDB parsing and directory-boundary checks can be substantial. Only cheap stamps run on the LSP loop.
        std::thread { [queue, rootCopy, selector, rootKey, input, generation]() {
            auto selected = std::make_shared<project::ProjectModel>();
            selected->targetSelection = project::select_mcpp_target(rootCopy, selector);
            queue->push(Event { EventKind::target_selected, Json { { "input", input } }, generation, std::move(selected), rootKey });
        } }.detach();
        return false;
    }

    void target_selected(int generation, const Json& message, const project::TargetSelection& selected) {
        if (generation != targetSelectionGeneration) return;
        targetSelectionBusy = false;
        if (message.value("input", std::string {}) != target_input_signature()) {
            if (refresh_target_selection()) {
                const bool requested { std::exchange(targetLoadRequested, false) };
                if (targetDeferredModel) {
                    reloadAfterLoad = reloadAfterLoad || requested;
                    handle_model_loaded(std::exchange(targetDeferredModel, {}));
                }
                else if (requested) start_model_load();
                else update_status();
            }
            return;
        }
        if (selected.retryable && targetReadRetries < 5) {
            ++targetReadRetries;
            targetRetryAt = Clock::now() + std::chrono::seconds { 1 };
            update_status();
            return;
        }
        if (!selected.retryable) targetReadRetries = 0;
        const auto previous = project::target_selection_key(targetSelection);
        targetSelection = selected;
        // S4 section 5: the kit follows the target; the load below reads with the kit of the new one.
        if (previous != project::target_selection_key(targetSelection)) select_kit();
        targetSelectionInput = message.value("input", std::string {});
        targetReadPending = false;
        targetRetryAt.reset();
        if (targetCacheDeferred && !model) {
            targetCacheDeferred = false;
            adopt_cached_model();
        }
        const bool requested { targetLoadRequested };
        const bool load { requested || !model || previous != project::target_selection_key(targetSelection) };
        targetLoadRequested = false;
        if (targetDeferredModel) {
            reloadAfterLoad = reloadAfterLoad || requested;
            handle_model_loaded(std::exchange(targetDeferredModel, {}));
        }
        else if (load) start_model_load();
        else update_status();
    }

    Json target_selection_json() const {
        if (targetSelection.mode.empty()) return nullptr;
        const bool applied { model && model->source == project::SourceKind::mcpp
            && project::target_selection_key(model->targetSelection) == project::target_selection_key(targetSelection)
            && (targetSelection.requestedTarget.empty() || project::mcpp_target_matches(targetSelection.requestedTarget, model->profile.target))
            && std::ranges::none_of(model->issues, [](const auto& issue) { return issue.code.starts_with("mcpp-target-"); }) };
        return Json { { "mode", targetSelection.mode }, { "requestedTarget", targetReadPending ? std::string {} : targetSelection.requestedTarget },
                      { "source", targetSelection.source }, { "reason", targetReadPending ? "the compilation database is being read again" : targetSelection.reason },
                      { "evidencePath", targetSelection.evidencePath }, { "pending", targetReadPending || !applied } };
    }

    void adopt_cached_model() {
        const auto providers = allowed_providers();
        const project::Detection detection { project::detect_project(root, options.database, providers, options.buildDiscovery != "off") };
        detectedSource = detection.kind;
        detectedManifest = detection.manifest;
        const bool selectionReady { refresh_target_selection() };
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
        if (detection.kind == project::SourceKind::mcpp) {
            // The old producer and version cannot certify the executable currently selected by PATH or a launcher.
            // Compatible models are provisional until this session's asynchronous producer confirms them.
            if (!selectionReady) {
                targetCacheDeferred = true;
                loadGiveUpAt = Clock::now() + FIRST_MODEL_WAIT;
                return;
            }
            if (cached->model.targetSelection.mode.empty()
                || project::target_selection_key(cached->model.targetSelection) != project::target_selection_key(targetSelection)
                || (!targetSelection.requestedTarget.empty() && !project::mcpp_target_matches(targetSelection.requestedTarget, cached->model.profile.target))
                || std::ranges::any_of(cached->model.issues, [](const auto& issue) { return issue.code.starts_with("mcpp-target-"); })) {
                log::info("the cached mcpp model of {} does not establish the current analysis target; asking the producer", root);
                loadGiveUpAt = Clock::now() + FIRST_MODEL_WAIT;
                return;
            }
            producerPath = cached->producer;
            producerVersion = cached->producerVersion;
            lastProducerMs = cached->producerMs;
            staleModelReason = "the cached mcpp model is in use while this session confirms the producer and its inputs";
            adopt_model(std::make_shared<project::ProjectModel>(std::move(cached->model)), "cache-pending");
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
        // 0.0.13 W5, W6: which input moved, and whether only its time did. A checkout or a branch switch rewrites files with
        // the bytes they had; the model they describe is the cached one, and the cache is fresh.
        std::string staleWhy { "an input changed (this cache predates the record of which)" };
        if (!cached->inputs.empty()) {
            const auto verdict = project::compare_inputs(cached->inputs, project::input_stamps(root, cached->model.watch, detection.manifest, cached->producer));
            if (verdict.unchanged) {
                log::info("the cached {} model for {} matches its inputs ({} rewritten with the same content); planning with it while the producer confirms it",
                          project::to_string(detection.kind), root, verdict.touched);
                adopt_model(std::make_shared<project::ProjectModel>(std::move(cached->model)), "cache-fresh");
                return;
            }
            staleWhy = verdict.changed;
        }
        staleCacheWhy = staleWhy;
        log::info("the cached {} model for {} is older than its inputs ({}); the producer has three seconds before it is used anyway",
                  project::to_string(detection.kind), root, staleWhy);
        staleCache = std::move(cached);
        loadGiveUpAt = Clock::now() + std::chrono::seconds { 3 };
    }

    // The producer has not answered within its wait. Use what there is --- never nothing.
    void use_what_there_is() {
        if (model) return;
        if (staleCache) {
            auto cached = std::move(*staleCache);
            staleCache.reset();
            staleModelReason = std::format("{} has not answered yet; the model from the last session is in use and may be stale ({})",
                                           project::to_string(cached.model.source), staleCacheWhy);
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
            { "targetSelection", candidate.source == project::SourceKind::mcpp ? project::target_selection_key(candidate.targetSelection) : std::string {} },
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
            const auto text = os::fs::read(producer_timing_path(directory));
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
        const auto text = os::fs::read(path);
        Json timing = text ? Json::parse(*text, nullptr, false) : Json::object();
        if (!timing.is_object()) timing = Json::object();
        timing[std::string { project::to_string(kind) }] = milliseconds;
        (void)os::fs::write_atomic(path, timing.dump());
    }

    void save_model_cache() const {
        if (!model) return;
        project::CachedModel cached;
        cached.model = *model;
        cached.producer = producerPath;
        cached.producerVersion = producerVersion;
        cached.producerMs = lastProducerMs;
        cached.fingerprint = project::inputs_fingerprint(root, model->watch, detectedManifest, producerPath, producerVersion);
        cached.inputs = project::input_stamps(root, model->watch, detectedManifest, producerPath);
        cached.savedAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (auto saved = project::save_model(cacheDirectory, cached); !saved) {
            log::info("the {} model of {} could not be cached: {}", project::to_string(model->source), root, saved.error().message);
        }
    }

    double since_start_() const { return std::chrono::duration<double>(Clock::now() - startup.began).count(); }

    void note_model_source(std::string origin) {
        if (!startup.model) {
            startup.model = since_start_();
            startup.origin = origin;
        }
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

    // A1 (plan 2026-10-14): what each build file and each named input of the producer held when it was last
    // described, by content. mcpp rewrites a file it lists as an input on every run with the same bytes (the
    // lock of a build program's synthesised package, mcpp#807); the watcher reports the new mtime, the model
    // was loaded again, the run rewrote it again -- the producer ran every two seconds of an idle session.
    // A change event whose file holds what it held is no change -- nor is a "created" one (a write through a rename is
    // reported so) for a file known to hold those bytes; a file never seen before is new whatever it holds.
    std::map<std::string, std::string> describedInputs;   // path key -> SHA-256 of the content

    // Whether `path` holds what it held when last seen; remembers what it holds now.
    bool input_unchanged(const std::string& path) {
        const auto text = os::fs::read(path);
        if (!text) {
            describedInputs.erase(base::path_key(path));
            return false;
        }
        std::string digest { base::sha256_hex(*text) };
        const auto [at, inserted] = describedInputs.try_emplace(base::path_key(path), digest);
        if (inserted) return false;
        const bool same { at->second == digest };
        at->second = std::move(digest);
        return same;
    }

    // The files a description reads that are named outright (no glob): the build files at the root and the
    // model's concrete watch entries. Sources go through the index and the structure check instead (G-5).
    std::vector<std::string> described_files() const {
        std::vector<std::string> files;
        for (const auto name : BUILD_FILES) files.push_back(base::join_path(root, name));
        if (model && model->source != project::SourceKind::inferred) {
            for (const auto& entry : model->watch) {
                if (entry.find_first_of("*?[{") != std::string::npos) continue;
                files.push_back(base::is_absolute_path(entry) ? base::normalize_path(entry) : base::join_path(root, entry));
            }
        }
        return files;
    }

    // Before a run: what the producer is about to read. An edit made while it runs then differs from this, and is
    // seen. After a model is adopted, only files not yet known are added, for the same reason.
    void remember_described_inputs(bool overwrite) {
        for (const auto& file : described_files()) {
            if (overwrite) input_unchanged(file);
            else if (!describedInputs.contains(base::path_key(file))) {
                if (const auto text = os::fs::read(file)) describedInputs.emplace(base::path_key(file), base::sha256_hex(*text));
            }
        }
    }

    void capture_source_structures() {
        loadingSourceStructures.clear();
        loadingSourceStamps.clear();
        loadingSourceProducerIdentity = producer_file_identity();
        // Only sources reported by the producer's watcher, never a recursive scan.
        // Capture before dispatch: a later disk edit is not evidence this run read it.
        std::size_t remaining { 32 * 1024 * 1024 };
        for (const auto& [key, path] : producerSourcePaths) {
            if (remaining == 0) break;
            const auto before = base::file_stamp(path);
            if (!before || before->size > remaining) continue;
            const auto text = os::fs::read(path, remaining);
            if (!text) break; // a failed bounded read may already have consumed the remaining budget
            remaining -= text->size();
            if (base::file_stamp(path) == before) {
                if (auto structure = described_source_structure(*text, project::scan_source(*text, true))) {
                    loadingSourceStructures.emplace(key, std::move(*structure));
                    loadingSourceStamps.emplace(key, *before);
                }
            }
        }
    }

    void confirm_source_structures(const project::ProjectModel& loaded) {
        if (loaded.source == project::SourceKind::inferred || !loaded.issues.empty() || reloadAfterLoad
            || loadingSourceProducerIdentity != producer_file_identity()
            || (!loaded.producer.empty() && !producerPath.empty() && !base::same_path(loaded.producer, producerPath))) return;
        std::set<std::string> units;
        for (const auto& set : loaded.database.sets) {
            for (const auto& unit : set.units) units.insert(base::path_key(spec::absolute_source(unit)));
        }
        describedSourceStructures.clear();
        for (const auto& [key, structure] : loadingSourceStructures) {
            const auto path = producerSourcePaths.find(key);
            const auto stamp = loadingSourceStamps.find(key);
            if (units.contains(key) && path != producerSourcePaths.end() && stamp != loadingSourceStamps.end()
                && base::file_stamp(path->second) == std::optional { stamp->second }
                && matches_watch_entries(loaded.watch, root, path->second)) {
                describedSourceStructures.emplace(key, structure);
            }
        }
        describedSourceGeneration = modelGeneration;
        describedSourceProducerIdentity = loadingSourceProducerIdentity;
    }

    // 0.0.17 XS-4 (#68): what a build tool made for itself in this cache (a provider's private configure directory, its id under
    // the cache) goes with a reset too, or a state xmake no longer recovers from outlives the reset that was to clear it. Removed
    // only while no load runs in it: a reset during a load leaves it to the start of the next one.
    void remove_producer_states() {
        std::error_code ignored;
        for (const auto* provider : project::registered_providers()) {
            const std::string directory { base::join_path(cacheDirectory, std::string { provider->id() }) };
            if (!std::filesystem::is_directory(directory, ignored)) continue;
            std::filesystem::remove_all(directory, ignored);
            log::info("removed {}'s private state for {} ({})", provider->id(), root, directory);
        }
    }

    void start_model_load() {
        if (loading) {
            reloadAfterLoad = true;
            return;
        }
        if (!refresh_target_selection()) {
            targetLoadRequested = true;
            update_status();
            return;
        }
        if (std::exchange(producerStatesResetPending, false)) remove_producer_states();
        loading = true;
        remember_described_inputs(true);
        const int generation { ++modelGeneration };
        capture_source_structures();
        project::LoadOptions load;
        load.trusted = options.trusted;
        load.cacheDirectory = cacheDirectory;
        load.compilerOverride = compilerOverride;
        load.mcppExecutable = options.mcpp;
        load.mcppTarget = options.mcppTarget;
        if (!targetSelection.mode.empty()) load.targetSelection = targetSelection;
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
        // This wait is on the worker, including warm starts. A cached model keeps serving while the
        // login environment resolves; the editor's provisional PATH must not confirm another producer.
        load.environmentWait = std::chrono::seconds { 10 };
        load.rootKey = key;
        producerSlowMs->store(0);
        loadStartedAt = Clock::now();
        load.onSlow = [slow = producerSlowMs](std::chrono::milliseconds elapsed) { slow->store(elapsed.count()); };
        producerSoftAt = Clock::now() + load.producerSoft + std::chrono::milliseconds { 200 };
        std::shared_ptr<const spec::Kit> kitCopy = kit ? std::make_shared<const spec::Kit>(*kit) : nullptr;
        const std::string rootCopy { root };
        const std::string probeCachePath { base::join_path(base::cache_directory(), "toolchains/probe.json") };
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
            const auto stamp { base::file_stamp(path) };
            return stamp && now - static_cast<std::int64_t>(stamp->modified_ns) < std::chrono::nanoseconds { 10s }.count();
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
        if (options.trusted && loadedModel->detected == project::SourceKind::mcpp) {
            // Untrusted roots deliberately select no producer target and scan sources instead.
            // A watched selection can change while a producer is running, including before its file event arrives.
            // Such a result never becomes the active model, even briefly.
            if (!refresh_target_selection()) {
                targetDeferredModel = std::move(loadedModel);
                reloadAfterLoad = false;
                loadGiveUpAt.reset();
                producerElapsed.reset();
                update_status();
                return;
            }
            if (project::target_selection_key(loadedModel->targetSelection) != project::target_selection_key(targetSelection)) {
                reloadAfterLoad = false;
                loadGiveUpAt.reset();
                producerElapsed.reset();
                loadStartedAt.reset();
                log::info("discarding the mcpp model of {}: its analysis target was replaced while it ran", root);
                start_model_load();
                return;
            }
            const auto targetProblem = std::ranges::find_if(loadedModel->issues, [](const auto& issue) { return issue.code.starts_with("mcpp-target-"); });
            // With buildTool=off a cold session may still use its existing CDB, explicitly
            // marked pending. It must not replace an established model with another platform.
            const bool rejectTarget { targetProblem != loadedModel->issues.end()
                && loadedModel->source != project::SourceKind::inferred
                && (targetProblem->code != "mcpp-target-unapplied" || (model && model->source != project::SourceKind::inferred)) };
            if (rejectTarget) {
                staleModelReason = std::format("the requested mcpp analysis target was not applied: {}; {}", targetProblem->message,
                    model ? "the previous model remains in use" : "sources are scanned until the target can be applied");
                const bool requestedAgain { std::exchange(reloadAfterLoad, false) };
                loadGiveUpAt.reset();
                producerElapsed.reset();
                loadStartedAt.reset();
                coreWaitOver = true; // the producer answered; a rejected target is not pending work
                if (!model) start_inferred_load();
                else if (modelOrigin == "inferred") replanAt = Clock::now();
                journal.add("model-stale", Json { { "reason", staleModelReason } });
                if (requestedAgain) start_model_load();
                else update_status();
                return;
            }
        }
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
        // 0.0.13 W3: a build program (build.mcpp) that did not compile or run leaves out of the answer what it gives the
        // targets -- their flags, the units its rules generate. A complete model in hand is kept: targets keep their Qt
        // headers while build.mcpp is being typed, and clangd is not restarted twice per half-typed save. mcpp's errors go
        // onto the build program either way; the next complete answer replaces the model.
        note_build_program_failures(*loadedModel);
        if (model && model->source == loadedModel->source && build_program_failed(*loadedModel) && !build_program_failed(*model)
            && std::ranges::none_of(model->issues, [](const project::ModelIssue& issue) { return issue.code == "producer-partial"; })) {
            const auto failed = std::ranges::find_if(loadedModel->issues, [](const project::ModelIssue& issue) { return issue.code == spec::BUILD_PROGRAM_FAILED_ISSUE; });
            keptModelNotice = std::format("{} does not compile; the project keeps the description it had at {}", failed->path.empty() ? std::string { "a build program" } : failed->path,
                                          modelAdoptedAtUtc.empty() ? std::string { "start" } : modelAdoptedAtUtc);
            log::warning("model reload ({}): {}", root, keptModelNotice);
            journal.add("model-kept", Json { { "reason", keptModelNotice }, { "why", std::string { spec::BUILD_PROGRAM_FAILED_ISSUE } } });
            if (reloadAfterLoad) {
                reloadAfterLoad = false;
                start_model_load();
            }
            update_status();
            return;
        }
        keptModelNotice.clear();
        staleModelReason.clear();
        confirm_source_structures(*loadedModel);
        adopt_model(std::move(loadedModel), "producer");
    }

    static bool build_program_failed(const project::ProjectModel& candidate) {
        return std::ranges::any_of(candidate.issues, [](const project::ModelIssue& issue) { return issue.code == spec::BUILD_PROGRAM_FAILED_ISSUE; });
    }

    // mcpp's own words about a build program that failed, on that file: the compiler's "file:line:col: error: ..." lines
    // where they name it, else the message at its first line. The files that had some and have none now are cleared.
    void note_build_program_failures(const project::ProjectModel& loaded) {
        std::map<std::string, ProducerDiagnostics, std::less<>> next;
        for (const auto& issue : loaded.issues) {
            if (issue.code != spec::BUILD_PROGRAM_FAILED_ISSUE || issue.path.empty()) continue;
            const std::string file { base::is_absolute_path(issue.path) ? issue.path : base::join_path(root, issue.path) };
            next[base::path_key(file)] = ProducerDiagnostics { file, build_program_diagnostics(file, issue.message) };
        }
        std::vector<std::string> touched;   // the paths whose diagnostics change, as the root spells them
        for (const auto& [key, entry] : producerDiagnostics) {
            if (!next.contains(key)) touched.push_back(entry.path);
        }
        for (const auto& [key, entry] : next) touched.push_back(entry.path);
        producerDiagnostics = std::move(next);
        for (const auto& path : touched) {
            const Document* document { documents_.find_by_path(path) };
            if (document != nullptr) {
                publish_diagnostics(document->uri);
                continue;
            }
            const auto found = producerDiagnostics.find(base::path_key(path));
            client.notify("textDocument/publishDiagnostics",
                          Json { { "uri", base::path_to_uri(path) }, { "diagnostics", found == producerDiagnostics.end() ? Json::array() : found->second.diagnostics } });
        }
    }

    static Json build_program_diagnostics(std::string_view file, std::string_view message) {
        Json diagnostics = Json::array();
        const std::string_view name { base::file_name(file) };
        std::string first;
        for (const auto rawLine : base::split(message, '\n')) {
            const std::string_view line { base::trim(rawLine) };
            if (line.empty()) continue;
            if (first.empty()) first = std::string { line };
            // "<path>:<line>:<column>: error: <text>" (GCC, Clang; MSVC's "<path>(<line>): error" has no column and is left as text)
            const std::size_t error { line.find(": error: ") };
            const std::size_t fatal { line.find(": fatal error: ") };
            const std::size_t at { fatal != std::string_view::npos ? fatal : error };
            if (at == std::string_view::npos) continue;
            const std::string_view where { line.substr(0, at) };
            const auto parts = base::split(where, ':');
            if (parts.size() < 3) continue;
            const std::string_view columnText { parts[parts.size() - 1] };
            const std::string_view lineText { parts[parts.size() - 2] };
            std::string_view path { where.substr(0, where.size() - columnText.size() - lineText.size() - 2) };
            int lineNumber { 0 };
            int column { 0 };
            if (std::from_chars(lineText.data(), lineText.data() + lineText.size(), lineNumber).ec != std::errc {}
                || std::from_chars(columnText.data(), columnText.data() + columnText.size(), column).ec != std::errc {})
                continue;
            if (base::file_name(path) != name) continue;
            const std::string text { line.substr(at + (fatal != std::string_view::npos ? 15 : 9)) };
            const Json position { { "line", std::max(0, lineNumber - 1) }, { "character", std::max(0, column - 1) } };
            diagnostics.push_back(Json { { "range", Json { { "start", position }, { "end", position } } }, { "severity", 1 }, { "source", "mcpp" },
                                         { "code", std::string { spec::BUILD_PROGRAM_FAILED_ISSUE } }, { "message", text } });
            if (diagnostics.size() >= 20) break;
        }
        if (diagnostics.empty()) {
            const Json position { { "line", 0 }, { "character", 0 } };
            diagnostics.push_back(Json { { "range", Json { { "start", position }, { "end", position } } }, { "severity", 1 }, { "source", "mcpp" },
                                         { "code", std::string { spec::BUILD_PROGRAM_FAILED_ISSUE } },
                                         { "message", std::format("mcpp could not build this build program: {}", first.empty() ? std::string { "no reason given" } : first) } });
        }
        return diagnostics;
    }

    // Everything that makes a model this workspace's model: the watch registration, the index, the
    // plan, the cache. One path, whether the model came from the producer, from the cache, or from
    // scanned sources; `origin` is only recorded.
    void adopt_model(std::shared_ptr<project::ProjectModel> loadedModel, std::string origin) {
        if (!loadedModel->producer.empty()) {
            producerPath = loadedModel->producer;
            producerVersion = loadedModel->producerVersion;
        }
        if (loadedModel->source == project::SourceKind::mcpp && options.trusted && options.buildTool != "off" && !producerPath.empty()) {
            producerObservedIdentity = producer_file_identity();
            producerIdentityAt = Clock::now() + std::chrono::seconds { 30 };
        } else producerIdentityAt.reset();
        const bool watchChanged { !model || model->watch != loadedModel->watch };
        std::string description { describe_model(*loadedModel) };
        const bool unchanged { model && description == modelDescription };
        model = std::move(loadedModel);
        remember_described_inputs(false);
        if (watchChanged) {
            {
                const std::lock_guard lock { watchPatterns->mutex };
                watchPatterns->entries = model->watch;
                watchPatterns->concreteBaseline.clear();
                for (const auto& entry : model->watch) {
                    if (entry.find_first_of("*?[{") != std::string::npos) continue;
                    const std::string path { base::is_absolute_path(entry) ? base::normalize_path(entry) : base::join_path(root, entry) };
                    watchPatterns->concreteBaseline.emplace(path, base::file_stamp(path));
                }
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
        modelAdoptedAtUtc = std::format("{:%H:%M}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
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
                } else if (auto text = os::fs::read(path)) {
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
        // mcpp-community/mcpp#797: the configurations that compile each unit, and mcpp's engine interface.
        std::map<std::string, unsigned, std::less<>> configurations;
        readOnlySources.clear();
        for (const auto& set : model->database.sets) {
            const unsigned configuration { spec::is_build_program_set(set) ? index::BUILD_PROGRAMS : index::TARGETS };
            for (const auto& unit : set.units) {
                const std::string key { base::path_key(spec::absolute_source(unit)) };
                configurations[key] |= configuration;
                if (set.name == spec::ENGINE_INTERFACE_SET) readOnlySources.insert(key);
            }
        }
        index.set_configurations(std::move(configurations), readOnlySources);
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
        std::error_code ignored;
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
            auto text = os::fs::read(path);
            return text ? project::scan_source(*text) : project::ScanResult {};
        };
        input.metadataReader = metadataReader;
        std::set<std::string> openSources;
        for (const auto& path : openedOutsideModel) {
            if (std::filesystem::is_regular_file(path, ignored)) openSources.insert(path);
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
            if (auto disk = os::fs::read(document->path)) input.editingDiskImports[document->path] = project::required_names(project::scan_source(*disk));
            if (!lastEdit || edited->second > *lastEdit) lastEdit = edited->second;
        }
        input.modelOrigin = modelOrigin;
        if (coreEngine != nullptr) coreEngine->configure_plan(input);
        normalize::EnginePlan newPlan { normalize::plan_engine(input) };
        // An open build program no set describes is not read (mcpp-community/mcpp#797); one another context reads is
        // already in otherContextSources.
        for (const Document* document : documents_.all()) {
            if (document->path.empty() || !project::is_build_program_source(document->path)) continue;
            const auto same = [&](const std::string& file) { return base::same_path(file, document->path); };
            if (std::ranges::any_of(newPlan.entries, same, &normalize::EngineEntry::file)) continue;
            if (std::ranges::any_of(newPlan.otherContextSources, same, &std::pair<std::string, std::string>::first)) continue;
            newPlan.unreadSources.push_back(document->path);
        }
        // A stand-in held back for a file being edited is planned once the file has been quiet for EDITING_WINDOW.
        if (newPlan.standInsDeferred && lastEdit) {
            const auto quiet = *lastEdit + EDITING_WINDOW + std::chrono::milliseconds { 100 };
            if (!replanAt || quiet < *replanAt) replanAt = quiet;
        }
        plan = std::move(newPlan);
        openedOutsideModel = { plan.openSources.begin(), plan.openSources.end() };
        plannedFiles.clear();
        for (const auto& entry : plan.entries) plannedFiles.insert(base::path_key(entry.file));
        // The answer layer's fingerprint of a file: its command, but for the file itself (the last argument). A plan
        // whose commands changed can change what any file sees (another unit's flags, a provider moved): every answer
        // kept before is uncertain from now on.
        std::unordered_map<std::string, std::string> commands;
        for (const auto& entry : plan.entries) {
            std::string command;
            for (std::size_t i { 0 }; i + 1 < entry.arguments.size(); ++i) command += entry.arguments[i] + '\n';
            commands[base::path_key(entry.file)] = std::move(command);
        }
        if (answers_ && !planCommands_.empty() && commands != planCommands_) answers_->everything_changed();
        if (commands != planCommands_) highlights.unsettle_all();
        planCommands_ = std::move(commands);
        // H2: a document asked for before its command was known is looked up on disk now; a hit is told to the client.
        for (const Document* document : documents_.all()) {
            highlights::Kept* kept { highlights.find(document->uri) };
            if (kept == nullptr || kept->known || kept->cacheChecked || document->path.empty()) continue;
            read_kept_tokens(*document, *kept);
            if (kept->known && kept->givenVersion) request_tokens_refresh(TOKENS_REFRESH_SOON);
        }
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
        {
            // apply may synchronously report status before its new plan is installed.
            // Publish only after every engine has applied this same workspace plan.
            struct StatusBatch {
                bool& active;
                bool previous;
                ~StatusBatch() { active = previous; }
            } batch { applyingPlan, std::exchange(applyingPlan, true) };
            for (const auto& engine : engines) {
                if (coreWaits && engine.get() == coreEngine) continue;
                engine->apply(&plan);
            }
            for (const Document* document : documents_.all()) publish_diagnostics(document->uri);
        }
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
    // `typing`: of the files whose changes asked for it, those still being typed in (W4); the reload waits for them.
    void schedule_reload(bool fromEdits = false, const std::vector<std::string>& typing = {}) {
        using namespace std::chrono_literals;
        if (fromEdits && reloadAt && !reloadMovable) return;
        const auto now { Clock::now() };
        const auto wait = fromEdits ? std::clamp<std::chrono::milliseconds>(std::chrono::milliseconds { lastProducerMs }, 1500ms, 60s) : 1500ms;
        reloadAt = now + wait;
        reloadMovable = fromEdits;
        if (!fromEdits) {
            reloadWaitsFor.clear();
            reloadDeferredSince.reset();
            return;
        }
        reloadWaitsFor.insert(typing.begin(), typing.end());
        defer_reload_while_typing(now);
    }

    // W4: the pending reload put off until the files it waits for are quiet, within EDIT_DEFER_LIMIT of the first wait.
    void defer_reload_while_typing(Clock::time_point now) {
        if (!reloadAt || !reloadMovable) return;
        std::optional<Clock::time_point> quiet;
        for (const auto& key : reloadWaitsFor) {
            const auto edited = editedAt.find(key);
            if (edited != editedAt.end() && edited->second + EDIT_SETTLE > now) quiet = std::max(quiet.value_or(now), edited->second + EDIT_SETTLE);
        }
        if (!quiet) return;
        if (!reloadDeferredSince) reloadDeferredSince = now;
        reloadAt = std::max(*reloadAt, std::min(*quiet, *reloadDeferredSince + EDIT_DEFER_LIMIT));
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

    // mcpp-community/mcpp#797: a build program, or a unit only build programs compile, that the context does not read
    // says which context does; a `.mcpp` file mcpp's build database does not describe says which mcpp describes one.
    std::optional<Json> configuration_diagnostic(std::string_view path) const {
        if (path.empty() || !model || plannedFiles.contains(base::path_key(path))) return std::nullopt;
        const auto said = [](std::string message) {
            return Json { { "range", index::to_json(base::Range {}) }, { "severity", 3 }, { "code", "build-program-context" },
                          { "source", "mcppls" }, { "message", std::move(message) } };
        };
        const auto elsewhere = std::ranges::find_if(plan.otherContextSources, [&](const auto& each) { return base::same_path(each.first, path); });
        if (elsewhere != plan.otherContextSources.end()) {
            const auto set = spec::find_set(model->database, elsewhere->second);
            const std::string toolchain { set ? model->database.sets[*set].toolchain : std::string {} };
            return said(std::format("{} is compiled for the build programs{}, which this context does not read; the context {} does "
                                    "(mcppls: Select Context)",
                                    base::file_name(path), toolchain.empty() ? std::string {} : std::format(", with {}", toolchain), elsewhere->second));
        }
        if (!project::is_build_program_source(path) || model->source != project::SourceKind::mcpp) return std::nullopt;
        const bool described { std::ranges::any_of(model->database.sets, [&](const spec::Set& set) {
            return std::ranges::any_of(set.units, [&](const spec::TranslationUnit& unit) { return base::same_path(spec::absolute_source(unit), path); });
        }) };
        if (described) return std::nullopt;
        const std::string& version { model->producerVersion };
        if (version.empty() || project::mcpp_version_less(version, base::BUILD_PROGRAMS_MCPP_VERSION)) {
            return said(std::format("{} is not read: {} describes the targets only, and mcpp {} and later describe build programs in the "
                                    "build database", base::file_name(path), version.empty() ? std::string { "this mcpp" } : std::format("mcpp {}", version),
                                    base::BUILD_PROGRAMS_MCPP_VERSION));
        }
        return said(std::format("{} is not read: mcpp's build database does not describe it (mcpp compiles the build.mcpp beside a "
                                "package's mcpp.toml)", base::file_name(path)));
    }

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
        Json moduleDiagnostics = document->path.empty() ? Json::array() : index.diagnostics(document->path);
        if (const auto produced = producerDiagnostics.find(base::path_key(document->path)); produced != producerDiagnostics.end()) {
            for (const auto& diagnostic : produced->second.diagnostics) moduleDiagnostics.push_back(diagnostic);
        }
        if (auto said = configuration_diagnostic(document->path)) {
            // A build program no set describes is not read at all: what it imports is not judged either.
            if (std::ranges::any_of(plan.unreadSources, [&](const std::string& file) { return base::same_path(file, document->path); })) {
                moduleDiagnostics = Json::array();
            }
            moduleDiagnostics.push_back(std::move(*said));
        }
        Json fromEngines = Json::array();
        for (const auto& [engineId, byUri] : engineDiagnostics) {
            const auto entry = byUri.find(uri);
            if (entry == byUri.end() || !entry->second.is_array()) continue;
            for (const auto& diagnostic : entry->second) fromEngines.push_back(diagnostic);
        }
        // 0.0.13 W3(d): an include of a file the build generates and has not generated yet says so, and what to do,
        // where clangd says only that the file is not found.
        // 0.0.13 W3(d): an include the build has not generated yet says so, and what to do, where clangd says only that the file
        // is not found. A missing generated file is matched by its name; a missing generated directory (a rule's output
        // directory) by any include not found in a file of the project.
        if (model && !model->generatedMissing.empty()) {
            for (auto& diagnostic : fromEngines) {
                if (!diagnostic.is_object() || diagnostic.value("code", std::string {}) != "pp_file_not_found") continue;
                const std::string message { diagnostic.value("message", std::string {}) };
                const std::size_t open { message.find('\'') };
                const std::size_t close { open == std::string::npos ? std::string::npos : message.find('\'', open + 1) };
                if (close == std::string::npos) continue;
                const std::string included { message.substr(open + 1, close - open - 1) };
                for (const auto& missing : model->generatedMissing) {
                    const std::string_view name { base::file_name(missing) };
                    const bool directory { base::extension(missing).empty() };
                    if (!directory && name != base::file_name(included)) continue;
                    diagnostic["message"] = directory
                        ? std::format("'{}' is not found, and the build has not generated {} yet; build the project once (C++ Modules: Build in Terminal) "
                                      "if the build makes it", included, name)
                        : std::format("'{}' is generated by the build and does not exist yet; build the project once (C++ Modules: Build in Terminal) "
                                      "and this file has its semantics", included);
                    diagnostic["code"] = "generated-file-missing";
                    break;
                }
            }
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

    // 0.0.13 W7: which mcppls, engine and mcpp are running, in one place: the status, the report and the
    // overview read it, and the log's first lines say the same. No paths (it is copied into issues), and
    // nothing that changes with each request, so carrying it in every status costs no extra notification.
    std::string extension_version() const {
        const Json* version { lsp::find_path(clientParams, { "initializationOptions", "extensionVersion" }) };
        return version != nullptr && version->is_string() ? version->get<std::string>() : std::string {};
    }

    // An editor package that started a server other than its own (an older payload left on PATH, an
    // MCPPLS_SERVER from another install): a report then describes a version nobody thinks is running.
    std::string version_mismatch() const {
        const std::string extension { extension_version() };
        if (extension.empty() || extension == base::VERSION) return {};
        return std::format("the editor extension is {} but the server it started is mcppls {}; reinstall the extension or remove the other server",
                           extension, base::VERSION);
    }

    Json about_json() const {
        Json about { { "server", std::string { base::VERSION } }, { "platform", std::string { base::payload_platform() } }, { "startedAt", startedAtUtc } };
        if (const std::string extension { extension_version() }; !extension.empty()) about["extension"] = extension;
        if (const Json* info = lsp::find(clientParams, "clientInfo"); info != nullptr && info->is_object()) {
            about["editor"] = Json { { "name", info->value("name", std::string {}) }, { "version", info->value("version", std::string {}) } };
        }
        if (coreEngine != nullptr) {
            const engine::EngineStatus core { coreEngine->status() };
            Json engineAbout { { "name", core.name }, { "version", core.version }, { "restarts", core.restarts } };
            if (payload.engineIdentity && payload.identityProblem.empty()) {
                engineAbout["build"] = payload.engineIdentity->version;
                engineAbout["features"] = payload.engineIdentity->features.size();
            }
            about["engine"] = std::move(engineAbout);
        } else {
            about["engine"] = Json { { "name", "none" } };
        }
        if (kit) about["kit"] = kit->name;
        if (detectedSource == project::SourceKind::mcpp) about["mcpp"] = producerVersion;
        if (const std::string mismatch { version_mismatch() }; !mismatch.empty()) about["mismatch"] = mismatch;
        if (cacheOnDevDrive) about["cacheDevDrive"] = *cacheOnDevDrive;
        return about;
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
            profile = Json { { "kind", "semantic-kit" }, { "stdlib", "unknown" }, { "target", std::string { base::payload_platform() } } };
        }
        return profile;
    }

    // ---- the kit of the target (S4 section 5) -----------------------------------------------

    // The kits as they are now, read again after an install.
    void read_kits() {
        kits.clear();
        if (payloadCorrupt || !kitEnabled) return;
        payload.kits = engine::kit_roots(payload);
        kits = engine::load_kits(payload);
    }

    // The kit for this root's target: the one it asks mcpp for, else this host's own.
    void select_kit() {
        const std::string before { kit ? std::format("{} {}", kit->name, kit->target) : std::string {} };
        if (payloadCorrupt || !kitEnabled) {
            kit.reset();
            kitTarget.clear();
            kitMissingTarget.clear();
            return;
        }
        auto choice = engine::choose_kit(kits, payload.explicitKit, targetSelection.requestedTarget, engine::host_kit_target());
        kit = std::move(choice.kit);
        kitTarget = std::move(choice.target);
        kitMissingTarget = std::move(choice.missingTarget);
        if (kit && spec::requires_macos_sdk(*kit) && macosSdk.empty()) {
            macosSdk = engine::macos_sdk_path();
            // usable plan W5.4 / U7: keep looking every 30s so installing the Command Line Tools
            // while the server runs is picked up without a restart.
            if (macosSdk.empty()) sdkCheckAt = Clock::now() + std::chrono::seconds { 30 };
        }
        kitCheckAt.reset();
        if (!kitMissingTarget.empty()) kitCheckAt = Clock::now() + std::chrono::seconds { 30 };
        const std::string after { kit ? std::format("{} {}", kit->name, kit->target) : std::string {} };
        if (after == before) return;
        if (!kit) log::warning("no semantic kit serves {} ({})", kitTarget, root);
        else log::info("semantic kit {} ({}) for {} at {}{} ({})", kit->name, kit->target, kitTarget, kit->root,
                       kitMissingTarget.empty() ? std::string {} : std::format(", standing in for mcppls-kit-{}", kitMissingTarget), root);
    }

    std::string kit_install_command() const {
        if (kitMissingTarget.empty()) return {};
        return payload.kitVersion.empty() ? std::format("xlings install mcppls-kit-{}", kitMissingTarget)
                                          : std::format("xlings install mcppls-kit-{}@{}", kitMissingTarget, payload.kitVersion);
    }

    // What reading with the stand-in leaves out (§9.2, said as it is): with the openkal kit, `import std` and the
    // standard library work and this system's own headers and macros do not.
    std::string kit_missing_message() const {
        const std::string command { kit_install_command() };
        if (!kit) return std::format("no semantic kit serves {}; install the kit for it: {}", kitTarget, command);
        if (!kit->target.ends_with("-none-elf")) {
            return std::format("the semantic kit for {} is not installed: files are read with the kit for {}; install it: {}", kitMissingTarget,
                               engine::kit_target_name(kit->target), command);
        }
        std::string_view system { "this system's own headers and macros" };
        if (kitMissingTarget.ends_with("-w64-mingw32")) system = "windows.h and the other Windows headers, and _WIN32";
        else if (kitMissingTarget.ends_with("-apple-darwin")) system = "the macOS SDK's headers, and __APPLE__";
        else if (kitMissingTarget.ends_with("-linux-gnu")) system = "glibc's and the Linux kernel's own headers, and __linux__";
        return std::format("the semantic kit for {} is not installed: files are read with the openkal kit, so import std and the standard "
                           "library work, while {} are not available; install it: {}",
                           kitMissingTarget, system, command);
    }

    static std::optional<std::string> find_xlings() {
        if (auto found = toolrun::find_program("xlings")) return found;
        std::error_code ignored;
        const std::string installed { base::join_path(base::home_directory(),
                                                      std::format(".xlings/subos/current/bin/xlings{}", os::conventions().exe_suffix)) };
        if (std::filesystem::is_regular_file(installed, ignored)) return installed;
        return std::nullopt;
    }

    bool install_kit(std::string_view wanted) {
        const std::string target { wanted.empty() ? kitMissingTarget : std::string { wanted } };
        if (target.empty() || kitInstalling) return false;
        const auto at = std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
        const auto xlings = find_xlings();
        if (!xlings) {
            kitInstall = Json { { "outcome", "failed" }, { "target", target }, { "at", at },
                                { "message", std::format("xlings is not installed, so mcppls-kit-{} cannot be installed here; install xlings, or unpack the "
                                                         "release's mcppls-kit-{}-<version>.tar.gz into its store", target, target) } };
            update_status();
            return false;
        }
        const std::string package { payload.kitVersion.empty() ? std::format("mcppls-kit-{}", target)
                                                               : std::format("mcppls-kit-{}@{}", target, payload.kitVersion) };
        log::info("installing the semantic kit {} with {}, as asked ({})", package, *xlings, root);
        kitInstalling = true;
        update_status();
        auto queue = events;
        const std::string rootKey { key };
        std::thread { [queue, rootKey, program = *xlings, package, target]() {
            auto result = toolrun::run({ .program = program,
                                         .arguments = { "install", package, "-y" },
                                         .purpose = "kit-install",
                                         .network = toolrun::Network::allowed,
                                         .bounds = os::process::bounds { .hard = std::chrono::minutes { 15 } } });
            const bool ok { result && result->status.success() && !result->timed_out };
            std::string message { ok ? std::format("{} is installed", package) : std::string {} };
            if (!result) message = std::format("{} could not be started: {}", program, result.error().message);
            else if (!ok) message = std::format("xlings install {} failed ({}): {}", package, result->timed_out ? "timed out" : "exit status",
                                                base::last_lines(result->err.empty() ? result->out : result->err, 6));
            queue->push(Event { EventKind::kit_installed, Json { { "ok", ok }, { "message", message }, { "target", target } }, 0, {}, rootKey });
        } }.detach();
        return true;
    }

    // The kits installed since they were read: the target's kit, installed by the editor itself or by hand, is taken
    // without a restart; the project is read again with it.
    void look_for_kits() {
        if (kitInstalling || payloadCorrupt || !kitEnabled) {
            if (!kitMissingTarget.empty()) kitCheckAt = Clock::now() + std::chrono::seconds { 30 };
            return;
        }
        if (engine::kit_roots(payload) == payload.kits) {
            if (!kitMissingTarget.empty()) kitCheckAt = Clock::now() + std::chrono::seconds { 30 };
            return;
        }
        const std::string before { kit ? kit->root : std::string {} };
        read_kits();
        select_kit();
        if ((kit ? kit->root : std::string {}) == before) return;
        log::info("a semantic kit was installed; reading the project with {} ({})", kit ? kit->name : std::string { "none" }, root);
        schedule_reload();
        update_status();
    }

    void kit_installed(const Json& outcome) {
        kitInstalling = false;
        const bool ok { outcome.value("ok", false) };
        kitInstall = Json { { "outcome", ok ? "installed" : "failed" }, { "target", outcome.value("target", std::string {}) },
                            { "message", outcome.value("message", std::string {}) },
                            { "at", std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())) } };
        journal.add("kit-install", kitInstall);
        if (!ok) {
            log::warning("the semantic kit install failed ({}): {}", root, kitInstall.value("message", std::string {}));
            update_status();
            return;
        }
        log::info("{}; reading the project with it ({})", kitInstall.value("message", std::string {}), root);
        read_kits();
        select_kit();
        schedule_reload();
        update_status();
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
        if (applyingPlan) return;          // replan publishes the committed snapshot after all synchronous apply calls
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
        // A start that ends degraded is the one most worth reading: it is said too, with the state it reached.
        if ((state == State::ready || state == State::degraded) && !startup.said) say_startup_(state);
        // Past here the status is built for every client: one that does not understand
        // `cxxModules/status` is not sent it, and `mcppls status` (S3 section 8) still reads it.
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
        // S4 section 5 (ecosystem design §9.2): the kit of the target is not installed and another stands in. Said with
        // what is missing and the command that installs it; a client that knows `askInstall` may offer to install it
        // in the background (mcppls.installKit).
        if (!kitMissingTarget.empty() && model && model->usesKit) {
            add("kit-missing", kit_missing_message(), "mcppls.installKit", "Install Kit", "environment");
            if (!issues.empty() && issues.back().value("code", std::string {}) == "kit-missing") {
                issues.back()["kit"] = Json { { "target", kitMissingTarget }, { "package", std::format("mcppls-kit-{}", kitMissingTarget) },
                                              { "version", payload.kitVersion }, { "command", kit_install_command() } };
                issues.back()["askInstall"] = !kitInstalling;
                if (kitInstalling) issues.back()["installing"] = true;
            }
        }
        Json notices = Json::array();
        if (model) {
            for (const auto& notice : model->notices) notices.push_back(Json { { "code", notice.code }, { "message", notice.message } });
        }
        if (!payload.kitNotice.empty()) notices.push_back(Json { { "code", "kit-version-mismatch" }, { "message", payload.kitNotice } });
        if (const std::string mismatch { version_mismatch() }; !mismatch.empty()) notices.push_back(Json { { "code", "version-mismatch" }, { "message", mismatch } });
        if (!keptModelNotice.empty()) notices.push_back(Json { { "code", std::string { spec::BUILD_PROGRAM_FAILED_ISSUE } }, { "message", keptModelNotice } });
        if (!buildProgramHeld.empty()) notices.push_back(Json { { "code", "build-program-held" }, { "message", buildProgramHeld } });
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
            { "about", about_json() },
        };
        if (const Json selection = target_selection_json(); selection.is_object()) params["targetSelection"] = selection;
        if (!notices.empty()) params["notices"] = std::move(notices);
        if (!onlineRun.is_null()) params["onlineRun"] = onlineRun;   // D-5, S3-4-26
        if (!kitInstall.is_null()) params["kitInstall"] = kitInstall;
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
        state_status_file_(params);
        if (clientSupportsStatus) client.notify("cxxModules/status", std::move(params));
    }

    // What `mcppls status` reads of this instance (S3 section 8): the status it last sent, and the
    // mcpp it ran for the root, written where its `instance.json` is. A reader that finds the
    // instance gone ignores the file; the server removes it when it releases the workspace.
    void state_status_file_(const Json& params) const {
        if (!lease) return;
        Json document { { "schema", 1 }, { "status", params } };
        if (const auto self = os::process::self()) document["pid"] = static_cast<std::int64_t>(self->id);
        if (detectedSource == project::SourceKind::mcpp)
            document["mcpp"] = Json { { "version", producerVersion }, { "program", producerPath } };
        (void)os::fs::write_atomic(lease->status_path(), document.dump());
    }

    // ---- timers -----------------------------------------------------------------------

    static std::string seconds_text_(const std::optional<double>& value) {
        return value ? std::format("{:.1f} s", *value) : std::string { "-" };
    }

    Json startup_json_() const {
        Json json { { "began", startup.beganUtc }, { "origin", startup.origin }, { "state", startup.state } };
        const auto put = [&](std::string_view key, const std::optional<double>& value) {
            if (value) json[std::string { key }] = std::round(*value * 100) / 100;
        };
        put("modelSeconds", startup.model);
        put("engineSeconds", startup.engine);
        put("firstDiagnosticsSeconds", startup.firstDiagnostics);
        put("firstCompletionSeconds", startup.firstCompletion);
        put("readySeconds", startup.ready);
        if (coreEngine != nullptr) {
            const engine::EngineStatus core { coreEngine->status() };
            std::size_t built { 0 };
            for (const auto& [why, count] : core.rebuilds) built += count;
            json["modulesBuilt"] = built;
            json["rebuilds"] = Json(core.rebuilds);
            json["restarts"] = core.restarts;
        }
        return json;
    }

    void say_startup_(State state) {
        startup.said = true;
        startup.ready = since_start_();
        startup.state = std::string { to_string(state) };
        const Json json = startup_json_();
        std::string rebuilt { "?" };
        if (json.contains("modulesBuilt")) {
            rebuilt = std::to_string(json["modulesBuilt"].get<std::size_t>());
            std::string reasons;
            for (const auto& entry : json["rebuilds"].items()) {
                reasons += std::format("{}{} {}", reasons.empty() ? "" : ", ", entry.value().get<std::size_t>(), entry.key());
            }
            if (!reasons.empty()) rebuilt += std::format(" ({})", reasons);
        }
        log::info("startup ({}): model {} {} · engine {} · first diagnostics {} · first completion {} · modules built {} · restarts {} · {} {}", root,
                  startup.origin.empty() ? std::string { "none" } : startup.origin, seconds_text_(startup.model), seconds_text_(startup.engine),
                  seconds_text_(startup.firstDiagnostics), seconds_text_(startup.firstCompletion), rebuilt,
                  json.value("restarts", std::size_t { 0 }), startup.state, seconds_text_(startup.ready));
        journal.add("startup", json);
        // The last five starts of this workspace, newest last.
        const std::string path { base::join_path(cacheDirectory, "startups.json") };
        Json history = Json::array();
        if (auto text = os::fs::read(path)) {
            history = Json::parse(*text, nullptr, false);
            if (!history.is_array()) history = Json::array();
        }
        history.push_back(json);
        while (history.size() > 5) history.erase(history.begin());
        (void)os::fs::write_atomic(path, history.dump());
        healthAt = Clock::now() + HEALTH_EVERY;
    }

    static constexpr std::chrono::minutes HEALTH_EVERY { 10 };

    // 0.0.13 W6: one line every ten minutes of use: how requests fared and what the engine did meanwhile.
    void say_health_() {
        healthAt = Clock::now() + HEALTH_EVERY;
        std::size_t requests { 0 };
        for (const auto& [method, stats] : requestStats) requests += stats.count;
        if (requests == healthRequestsSeen) return;   // nothing was asked: nothing to say
        healthRequestsSeen = requests;
        std::string methods;
        for (const std::string_view method : { "textDocument/completion", "textDocument/hover", "textDocument/definition" }) {
            const auto found = requestStats.find(method);
            if (found == requestStats.end() || found->second.recentMs.empty()) continue;
            std::vector<double> sorted { found->second.recentMs.begin(), found->second.recentMs.end() };
            std::ranges::sort(sorted);
            const auto at = [&](double p) { return sorted[std::min(sorted.size() - 1, static_cast<std::size_t>(p * static_cast<double>(sorted.size())))]; };
            std::size_t byCore { 0 };
            if (coreEngine != nullptr) {
                if (const auto by = found->second.answeredBy.find(coreEngine->id()); by != found->second.answeredBy.end()) byCore = by->second;
            }
            methods += std::format("{}{} {} p50 {:.0f} ms p95 {:.0f} ms, {}% by the core engine", methods.empty() ? "" : " · ", method.substr(method.find('/') + 1),
                                   found->second.count, at(0.5), at(0.95), found->second.count == 0 ? 0 : byCore * 100 / found->second.count);
        }
        std::string engineText;
        if (coreEngine != nullptr) {
            const engine::EngineStatus core { coreEngine->status() };
            std::size_t built { 0 };
            for (const auto& [why, count] : core.rebuilds) built += count;
            engineText = std::format(" · engine {} ({} restarts, {} modules built)", core.state, core.restarts, built);
        }
        log::info("health ({}): {}{}", root, methods.empty() ? std::string { "no timed requests" } : methods, engineText);
    }

    static constexpr std::chrono::milliseconds BUILD_PROGRAM_QUIET { 2000 };
    static constexpr std::chrono::seconds BUILD_PROGRAM_VERDICT_WAIT { 10 };

    void note_build_program_changed(const std::string& path) {
        const auto now = Clock::now();
        buildProgramPending = path;
        buildProgramChangedAt = now;
        if (options.buildProgramApply == "manual") {
            buildProgramCheckAt.reset();
            buildProgramHeld = std::format("{} changed; it is applied when you ask (C++ Modules: Apply build.mcpp, mcppls.applyBuildProgram)",
                                           base::file_name(path));
            update_status();
            return;
        }
        buildProgramCheckAt = now + BUILD_PROGRAM_QUIET;   // each change starts the quiet period again
    }

    // Whether the build program can be applied now: saved as the editor has it, and not a file clangd says does not
    // compile. clangd reads it with its own set's arguments (mcpp 2026.10.11.1 and later); with an older mcpp, or a
    // file not open, or no verdict within BUILD_PROGRAM_VERDICT_WAIT, mcpp decides, and a failure keeps the model (W3(b)).
    void check_build_program() {
        buildProgramCheckAt.reset();
        if (buildProgramPending.empty()) return;
        const auto now = Clock::now();
        if (const Document* document = documents_.find_by_path(buildProgramPending)) {
            if (auto disk = os::fs::read(buildProgramPending)) {
                std::string_view saved { *disk };
                if (saved.starts_with("\xEF\xBB\xBF")) saved.remove_prefix(3);   // the editor's text has no BOM
                if (saved != document->text) {
                    // What mcpp would read is not what is being edited: wait for the next save, but not for ever -- after a
                    // while the saved text is applied (it is what the person saved; clangd's verdict is on the edits since).
                    if (buildProgramChangedAt && now - *buildProgramChangedAt < BUILD_PROGRAM_VERDICT_WAIT) {
                        buildProgramCheckAt = now + BUILD_PROGRAM_QUIET;
                        return;
                    }
                    apply_build_program("saved; the editor has edits since");
                    return;
                }
            }
            // Only a file clangd is given has a verdict to wait for: an older mcpp describes no build program.
            const bool engineReads { std::ranges::any_of(plan.entries, [&](const auto& entry) { return base::same_path(entry.file, document->path); }) };
            if (coreEngine != nullptr && engineReads) {
                const auto version = coreDiagnosticsVersions.find(document->uri);
                const bool current { version != coreDiagnosticsVersions.end() && version->second == document->version };
                if (!current && buildProgramChangedAt && now - *buildProgramChangedAt < BUILD_PROGRAM_VERDICT_WAIT) {
                    buildProgramCheckAt = now + std::chrono::milliseconds { 500 };
                    return;
                }
                std::size_t errors { 0 };
                if (current) {
                    if (const auto byEngine = engineDiagnostics.find(std::string { coreEngine->id() }); byEngine != engineDiagnostics.end()) {
                        if (const auto entry = byEngine->second.find(document->uri); entry != byEngine->second.end() && entry->second.is_array()) {
                            for (const auto& diagnostic : entry->second) errors += diagnostic.value("severity", 1) == 1 ? 1 : 0;
                        }
                    }
                }
                if (errors > 0) {
                    const std::string held { std::format("{} has {} error{} in the editor; the project is described again once it compiles",
                                                         base::file_name(buildProgramPending), errors, errors == 1 ? "" : "s") };
                    if (held != buildProgramHeld) {
                        buildProgramHeld = held;
                        log::info("not describing {} yet: {}", root, buildProgramHeld);
                        journal.add("build-program-held", Json { { "file", buildProgramPending }, { "errors", errors } });
                        update_status();
                    }
                    return;   // the next diagnostics of the file check again
                }
            }
        }
        apply_build_program("it was saved and is quiet");
    }

    void apply_build_program(std::string_view why) {
        if (buildProgramPending.empty() && buildProgramHeld.empty()) return;
        log::info("applying {} ({}): describing {} again", buildProgramPending.empty() ? std::string { "the build programs" } : base::file_name(buildProgramPending), why, root);
        journal.add("build-program-applied", Json { { "file", buildProgramPending }, { "why", std::string { why } } });
        buildProgramPending.clear();
        buildProgramHeld.clear();
        buildProgramCheckAt.reset();
        for (const auto& engine : engines) engine->sources_changed();
        start_model_load();
        update_status();
    }

    void handle_timers() {
        save_recent_files(false);
        // 0.0.13 W10 (#52-8): a turn of the timers that holds the loop says which part held it; one once took 15 minutes
        // and the log could say only "its timers".
        const auto timersBegan = Clock::now();
        auto phaseStart = timersBegan;
        std::string slowestPhase;
        Clock::duration slowestTook {};
        const auto phase = [&](std::string_view name) {
            const auto at = Clock::now();
            if (at - phaseStart > slowestTook) {
                slowestTook = at - phaseStart;
                slowestPhase = std::string { name };
            }
            phaseStart = at;
        };
        const auto sayPhases = [&] {
            const auto took = Clock::now() - timersBegan;
            if (took < TIMERS_SLOW) return;
            log::warning("the timers of {} took {} ms, {} ms of them in {}", root, std::chrono::duration_cast<std::chrono::milliseconds>(took).count(),
                         std::chrono::duration_cast<std::chrono::milliseconds>(slowestTook).count(), slowestPhase);
        };
        for (const auto& engine : engines) {
            const auto before = Clock::now();
            engine->handle_timers();
            // Plan 2026-09-30 §13: which engine held the event loop (the session says that it was held).
            if (const auto took = Clock::now() - before; took >= std::chrono::milliseconds { 250 }) {
                journal.add("timers-slow", Json { { "engine", std::string { engine->id() } },
                                                  { "ms", std::chrono::duration_cast<std::chrono::milliseconds>(took).count() } });
            }
            phase(std::format("the {} engine's timers", engine->id()));
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
            for (auto it = background_.begin(); it != background_.end();) {
                if (it->second.until > now) {
                    ++it;
                    continue;
                }
                if (coreEngine != nullptr && !lateCompletions.contains(it->first)) coreEngine->cancel(it->second.clientId);
                if (const auto inflight = answerInflight_.find(it->second.asked.k1); inflight != answerInflight_.end() && inflight->second == it->first) {
                    answerInflight_.erase(inflight);
                }
                answers_->count_background("dropped");
                it = background_.erase(it);
            }
            if (answers_ && answers_->enabled()) {
                if (!prefetchAt_ && !prefetchStopped_ && lastClientRequestAt_ && answers_->settings().prefetch && lastClientRequestAt_ != prefetchExhaustedFor_) {
                    prefetchAt_ = now + std::chrono::seconds { 2 };
                }
                if (prefetchAt_ && *prefetchAt_ <= now) prefetch_due(now);
                if (!answersSaveAt_) answersSaveAt_ = now + std::chrono::minutes { 1 };
                if (*answersSaveAt_ <= now) {
                    answers_->save();
                    answersSaveAt_ = now + std::chrono::minutes { 1 };
                }
            }
            for (auto it = lateCompletions.begin(); it != lateCompletions.end();) {
                if (it->second.until > now) {
                    ++it;
                    continue;
                }
                if (coreEngine != nullptr && !background_.contains(it->first)) coreEngine->cancel(it->second.clientId);
                it = lateCompletions.erase(it);
            }
        }
        phase("answers past their budget");
        if (leaseRenewAt && *leaseRenewAt <= now) {
            lease->renew(std::chrono::system_clock::now());
            // C-9: the tick's cheap part -- stat the few `instance.json` files and rename the dead
            // directories aside; their removal (however large) runs in the background, so the tick
            // stays at milliseconds. A live guest's own heartbeat protects it here, owner or not,
            // and a directory that says nothing at all gets the same grace the sweep gives it.
            const auto heartbeat { std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()) };
            if (cache::rename_dead_instances(workspaceDirectory_, heartbeat, ownToken_, cache_grace()) > 0) start_cache_task_("tick", base::file_time_now());
            leaseRenewAt = now + LEASE_RENEWAL;
        }
        phase("the workspace lease and dead instances");
        if (reloadAt && *reloadAt <= now) {
            reloadAt.reset();
            reloadWaitsFor.clear();
            reloadDeferredSince.reset();
            start_model_load();
        }
        if (targetRetryAt && *targetRetryAt <= now) {
            targetRetryAt.reset();
            start_model_load();
        }
        if (producerIdentityAt && *producerIdentityAt <= now) {
            producerIdentityAt = now + std::chrono::seconds { 30 };
            if (options.trusted && options.buildTool != "off" && detectedSource == project::SourceKind::mcpp) {
                const auto identity = producer_file_identity();
                if (identity != producerObservedIdentity) {
                    producerObservedIdentity = identity;
                    log::info("the mcpp executable of {} changed; describing the project again", root);
                    schedule_reload();
                }
            } else producerIdentityAt.reset();
        }
        if (targetIdentityAt && *targetIdentityAt <= now) {
            targetIdentityAt = now + std::chrono::seconds { 30 };
            // Polling FileStamp alone cannot see a symlink swap with the same size and mtime.
            // Compare the small identity tuple; only a changed tuple starts the bounded background parse.
            if (options.trusted && options.mcppTarget == "auto" && !targetSelectionInput.empty()
                && target_input_signature() != targetSelectionInput) refresh_target_selection();
        }
        if (downloadRetryAt && *downloadRetryAt <= now) {
            downloadRetryAt.reset();
            log::info("asking {} again, offline, whether it can describe {} now", project::to_string(detectedSource), root);
            start_model_load();
        }
        phase("starting a model load");
        if (replanAt && *replanAt <= now) replan();
        if (buildProgramCheckAt && *buildProgramCheckAt <= now) check_build_program();
        if (healthAt && *healthAt <= now) say_health_();
        phase("a replan");
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
        phase("the engine wait and the status");
        for (auto it = tokensConfirmAt.begin(); it != tokensConfirmAt.end();) {
            if (it->second > now) {
                ++it;
                continue;
            }
            const std::string uri { it->first };
            it = tokensConfirmAt.erase(it);
            if (const highlights::Kept* kept { highlights.find(uri) }; kept != nullptr && kept->fromCache) ask_core_tokens(uri);
        }
        if (tokensRefreshAt && *tokensRefreshAt <= now) {
            tokensRefreshAt.reset();
            // A request, not a notification (LSP 3.16): its answer carries nothing, and an id the session cannot
            // parse as an engine's is dropped, like the watcher registrations' answers.
            client.send(lsp::make_request(std::format("w:{}:t{}", key, ++tokensRefreshes), lsp::method::WORKSPACE_SEMANTIC_TOKENS_REFRESH, nullptr));
        }
        if (kitCheckAt && *kitCheckAt <= now) {
            kitCheckAt.reset();
            look_for_kits();
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
        phase("the rest");
        sayPhases();
    }
};

// ---- Workspace --------------------------------------------------------------------------

Workspace::Workspace(std::string root, std::string key, SessionOptions options, engine::PayloadPaths payload, bool payloadCorrupt, bool kitEnabled,
                     std::string compilerOverride, std::shared_ptr<EventChannel> events, ClientSink& client)
    : root_ { root }, key_ { key },
      impl_ { std::make_unique<Impl>(std::move(root), std::move(key), std::move(options), std::move(payload), payloadCorrupt, kitEnabled,
                                    std::move(compilerOverride), std::move(events), client) } {}

Workspace::~Workspace() { impl_->watchPatterns->stopped.store(true); }

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
    if (const Json* supported = lsp::find_path(impl_->clientParams, { "capabilities", "textDocument", "completion", "completionItem", "snippetSupport" });
        supported != nullptr && supported->is_boolean()) {
        impl_->snippetSupport = supported->get<bool>();
    }
    impl_->dynamicWatch = !usePolling;
    if (usePolling) impl_->start_watch_polling();
    impl_->startedAtUtc = std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    impl_->startup = Impl::Startup {};
    impl_->startup.beganUtc = impl_->startedAtUtc;
    log::info("mcppls {} ({}) root {}", base::VERSION, base::payload_platform(), root_);
    {
        // 0.0.13 W7: the versions in the log's first lines, so a log read alone says what ran.
        const Json about = impl_->about_json();
        std::string editor;
        if (about.contains("editor")) editor = std::format(" · editor {} {}", about["editor"].value("name", std::string {}), about["editor"].value("version", std::string {}));
        log::info("versions ({}): server {}{}{}", root_, base::VERSION,
                  about.contains("extension") ? std::format(" · extension {}", about["extension"].get<std::string>()) : std::string {}, editor);
        if (about.contains("mismatch")) log::warning("version mismatch ({}): {}", root_, about["mismatch"].get<std::string>());
    }
    // 0.0.14 G1: the files of the last session, ready before the editor opens them again.
    if (impl_->coreEngine != nullptr) {
        if (const auto recent = impl_->read_recent_files(); !recent.empty()) {
            log::info("the last files in use ({}): {} of them, opened in clangd ahead of the editor once it takes files", root_, recent.size());
            impl_->coreEngine->warm(recent);
        }
    }
    // S4 section 5: the kits this root chooses from, and the one for its target; a target that changes, or a kit
    // installed meanwhile, chooses again.
    impl_->read_kits();
    impl_->select_kit();
    if (impl_->coreEngine != nullptr && impl_->answers_) impl_->answers_->load();
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
    impl_->watchPatterns->stopped.store(true);
    impl_->save_recent_files(true);
    if (impl_->answers_) impl_->answers_->save(true);
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
    // A6 (plan 2026-10-14): a `.mcpp` file is C++ to clangd whatever language id the editor gave
    // it -- a companion extension may name build.mcpp a language of its own.
    std::string languageId { item->value("languageId", std::string { "cpp" }) };
    if (std::string_view { uri }.ends_with(".mcpp")) languageId = "cpp";
    const Document& document = impl_->documents_.open(uri, path, std::move(languageId),
                                                      item->value("version", std::int64_t { 0 }), item->value("text", std::string {}));
    if (!path.empty()) {
        impl_->note_recent_file(path);
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
    // mcpp design 2026-10-28 §11.1: what is kept of the document's tokens moves with the edits (read from the text
    // they apply to); every other document's tokens are no longer known to be exact -- it may import this one.
    std::vector<highlights::Edit> edits;
    std::int64_t before { 0 };
    if (const Document* previous { impl_->documents_.find(uri) }; previous != nullptr) {
        before = previous->version;
        if (impl_->highlights.find(uri) != nullptr) edits = highlights::edits_of(previous->text, params.value("contentChanges", Json::array()));
    }
    if (!impl_->documents_.change(uri, version, params.value("contentChanges", Json::array()))) return;
    impl_->highlights.edited(uri, before, version, edits);
    impl_->highlights.unsettle_except(uri);
    ++impl_->snapshotGeneration;
    ++impl_->documentChanges;
    impl_->lastDocumentChangeAt = std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now()));
    const Document* document { impl_->documents_.find(uri) };
    if (!document->path.empty()) {
        if (impl_->answers_) impl_->answers_->changed(document->path);
        impl_->editedAt[base::path_key(document->path)] = Clock::now();
        if (impl_->reloadWaitsFor.contains(base::path_key(document->path))) impl_->defer_reload_while_typing(Clock::now());
        impl_->note_recent_file(document->path);
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
    impl_->highlights.forget(uri);
    impl_->tokensConfirmAt.erase(uri);
    if (!document.path.empty()) {
        if (impl_->answers_) impl_->answers_->changed(document.path);   // what it has is the disk's again
        impl_->forget_recent_file(document.path);
        impl_->editedAt.erase(base::path_key(document.path));
        impl_->completionPace_.erase(document.path);   // the file's pace says how its engine answered while it was open
    }
    ++impl_->snapshotGeneration;
    if (!document.path.empty()) {
        if (auto text = os::fs::read(document.path)) impl_->index.update(document.path, *text);
    }
    impl_->document_event(engine::DocumentChange::closed, document, &message);
    // Diagnostics of a closed file are cleared; an engine may send its own empty set too.
    impl_->publishedDiagnostics.erase(uri);
    impl_->coreDiagnosticsVersions.erase(uri);
    for (auto& [engineId, byUri] : impl_->engineDiagnostics) byUri.erase(uri);
    impl_->client.notify("textDocument/publishDiagnostics", Json { { "uri", uri }, { "diagnostics", Json::array() } });
    if (impl_->reviewDiagnostics.contains(uri)) impl_->publish_diagnostics(uri);
    // A build program held on the editor's errors has no editor left to wait on: mcpp decides (0.0.13 W3).
    // (In manual mode it waits for the command whatever the editor has.)
    if (!impl_->buildProgramPending.empty() && !impl_->buildProgramHeld.empty() && impl_->options.buildProgramApply != "manual"
        && base::same_path(document.path, impl_->buildProgramPending)) {
        impl_->buildProgramCheckAt = Clock::now();
    }
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
    if (!saved.path.empty() && impl_->answers_) impl_->answers_->changed(saved.path);
    // H2: the saved text is the disk's; the compiler's exact tokens of it are worth keeping for the next open.
    if (highlights::Kept* kept { impl_->highlights.find(uri) }; kept != nullptr && !saved.path.empty()) {
        kept->edited = false;
        if (kept->known && kept->exact && kept->version == saved.version) impl_->write_kept_tokens(saved, *kept);
    }
    impl_->document_event(engine::DocumentChange::saved, saved, &message);
    // Fix plan F13: an import of a file being edited that the save just put on disk gets its stand-in now, not once the
    // file is quiet (clangd builds with what is on disk).
    if (!saved.path.empty()) {
        const auto edited = impl_->editedAt.find(base::path_key(saved.path));
        if (edited != impl_->editedAt.end() && Clock::now() - edited->second < Impl::EDITING_WINDOW
            && !impl_->saved_structure_is_planned(saved.path)) impl_->schedule_replan();
    }
}

void Workspace::handle_watched_files(const Json& changes) {
    ++impl_->snapshotGeneration;
    bool reload { false };
    bool describedAgain { false };   // a build file or another input of the producer's, not a source: reloaded without the edit delay
    bool replan { false };
    std::vector<std::string> typing;   // W4: changed sources whose structure differs and that are being typed in
    for (const auto& change : changes) {
        const std::string path { impl_->path_of_uri(change.value("uri", std::string {})) };
        if (path.empty()) continue;
        if (impl_->answers_) impl_->answers_->changed(path);
        const std::string_view name { base::file_name(path) };
        const int type { change.value("type", 2) };
        // S2 5: an input the producer named changes what it would answer, so the model is loaded again.
        const bool producerInput { impl_->model && impl_->model->source != project::SourceKind::inferred && name != "compile_commands.json"
                                   && matches_watch_entries(impl_->model->watch, impl_->root, path) };
        if (type == 3) impl_->describedInputs.erase(base::path_key(path));
        if (is_build_file(name)) {
            if (name == "compile_commands.json" && impl_->detectedSource == project::SourceKind::mcpp
                && base::same_path(path, base::join_path(impl_->root, "compile_commands.json"))) {
                // The CDB chooses only the target; arbitrary formatting or flags do not rerun mcpp.
                // project/explicit modes never consult it for selection.
                if (impl_->options.mcppTarget != "auto") continue;
                const auto before = project::target_selection_key(impl_->targetSelection);
                impl_->targetReadRetries = 0;
                impl_->targetSelectionInput.clear();
                if (!impl_->refresh_target_selection()) {
                    impl_->update_status();
                    continue;
                }
                if (before != project::target_selection_key(impl_->targetSelection)) reload = describedAgain = true;
                impl_->update_status();
                continue;
            }
            if (type != 3 && impl_->input_unchanged(path)) continue;   // A1: written again with the same bytes
            reload = describedAgain = true;
            continue;
        }
        if (producerInput && project::is_build_program_source(path) && impl_->detectedSource == project::SourceKind::mcpp) {
            impl_->note_build_program_changed(path);   // applied once it is quiet and compiles (0.0.13 W3(a))
            continue;
        }
        if (!project::is_cxx_source_name(path)) {
            // An input the producer named that is not a source: what it reads changed, unless only its mtime did (A1).
            if (producerInput && !(type != 3 && impl_->input_unchanged(path))) reload = describedAgain = true;
            continue;
        }
        const bool open { impl_->documents_.find_by_path(path) != nullptr };
        std::optional<std::string> diskText;
        if (type != 3) {
            if (auto text = os::fs::read(path)) diskText = std::move(*text);
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
        if (producerInput) {
            const std::string key { base::path_key(path) };
            if (type == 3) {
                impl_->producerSourcePaths.erase(key);
                impl_->describedSourceStructures.erase(key);
                impl_->loadingSourceStructures.erase(key);
                impl_->loadingSourceStamps.erase(key);
            } else {
                impl_->producerSourcePaths.insert_or_assign(key, path);
            }
            bool differs { true };
            if (type == 2 && diskText) {
                const auto scan { project::scan_source(*diskText, true) };
                const auto structure { described_source_structure(*diskText, scan) };
                const auto known = impl_->describedSourceStructures.find(key);
                if (impl_->describedSourceGeneration == impl_->modelGeneration && known != impl_->describedSourceStructures.end()) {
                    differs = !structure || known->second != *structure
                        || impl_->describedSourceProducerIdentity != impl_->producer_file_identity();
                } else {
                    // Conditional imports need one actual answer even when the old model
                    // happens to list the same lexical names (for example #if 1 -> #if 0).
                    differs = !structure || scan.uncertain || impl_->model_structure_differs(path, scan);
                    const auto started = impl_->loadingSourceStructures.find(key);
                    if (started != impl_->loadingSourceStructures.end()
                        && (!structure || started->second != *structure || !impl_->loading || !impl_->staleModelReason.empty())) differs = true;
                }
            }
            if (differs) {
                reload = true;
                const auto edited = impl_->editedAt.find(key);
                if (open && edited != impl_->editedAt.end() && Clock::now() - edited->second < Impl::EDIT_SETTLE) typing.push_back(key);
            }
        }
        if (open) continue;
        if (impl_->model && impl_->model->source == project::SourceKind::inferred && type != 2) reload = true;
        else replan = true;
    }
    if (reload || replan) {
        for (const auto& engine : impl_->engines) engine->sources_changed();
    }
    if (reload) impl_->schedule_reload(!describedAgain, typing);
    else if (replan) impl_->schedule_replan();
    // 0.0.13 W10 (#53): clangd refuses the whole notification for one change that is not a file ("clangd only supports
    // 'file' URI scheme for workspace files"), and the files beside it went unseen; only file: URIs go on.
    Json forwarded = Json::array();
    for (const auto& change : changes) {
        if (change.is_object() && change.value("uri", std::string {}).starts_with("file:")) forwarded.push_back(change);
    }
    if (!forwarded.empty()) {
        const Json message = lsp::make_notification("workspace/didChangeWatchedFiles", Json { { "changes", std::move(forwarded) } });
        for (const auto& engine : impl_->engines) engine->notify(message);
    }
    for (const Document* document : impl_->documents_.all()) impl_->publish_diagnostics(document->uri);
}

void Workspace::cancel(const Json& id) {
    for (const auto& [jobId, job] : impl_->jobs) {
        if (!job.renamePreflight || job.clientId != id) continue;
        const Json engineId = job.clientId;
        impl_->finish_job_cancelled(jobId);
        // Cancellation is advisory to a compiler: retire the client job first so a
        // later successful lookup cannot advance to declaration or rename.
        if (impl_->coreEngine != nullptr) impl_->coreEngine->cancel(engineId);
        return;
    }
    if (impl_->cancel_into_background(id)) return;
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
            const bool program { spec::is_build_program_set(set) };
            if (!set.kind.empty() && set.kind != "other") label += std::format(" ({}{})", program && set.kind == "executable" ? "build program" : set.kind,
                                                                               program && set.kind != "executable" ? ", for build programs" : "");
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
        if (const Json selection = impl.target_selection_json(); selection.is_object()) project["targetSelection"] = selection;
        if (!impl.needsDownload.empty()) project["needsDownload"] = impl.needsDownload;
        if (impl.staleCache) project["cacheAwaitingProducer"] = true;
    }
    // The environment build tools are started in: where it came from, and the NAMES of the variables
    // that differ from this process's. Never the values --- a proxy variable can carry a credential.
    const auto toolEnvironment = mcppls::toolenv::get();
    Json environment { { "source", toolEnvironment.source }, { "resolved", toolEnvironment.resolved },
                       { "durationMs", toolEnvironment.duration.count() }, { "differing", toolEnvironment.differing },
                       { "mode", std::string { mcppls::toolenv::mode_name(mcppls::toolenv::mode()) } } };
    if (!toolEnvironment.reason.empty()) environment["reason"] = toolEnvironment.reason;
    // The one run a report about a wrong or missing build description is almost always about. It is
    // looked for over everything kept, not over the twenty listed: a load that probes a dozen
    // compilers afterwards would otherwise push the producer out of sight.
    Json lastProducerRun = nullptr;
    const auto kept = mcppls::toolrun::recent(mcppls::toolrun::KEPT);
    for (const auto& record : kept) {
        if (!record.root.empty() && record.root != key_) continue;
        if (record.purpose == "producer" || record.purpose.starts_with("configure")) lastProducerRun = mcppls::toolrun::to_json(record);
    }
    if (project.is_object()) project["producerRun"] = std::move(lastProducerRun);
    Json toolRuns = Json::array();
    for (const auto& record : kept | std::views::drop(kept.size() > 20 ? kept.size() - 20 : 0)) {
        if (!record.root.empty() && record.root != key_) continue;
        toolRuns.push_back(mcppls::toolrun::to_json(record));
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
    // F9, F15, S3-6.4: what space-triggered completion cost, and how often keywords and templates answered without the core engine.
    Json completionCosts { { "spaceTrigger", Json { { "count", impl.spaceTrigger.count }, { "passed", impl.spaceTrigger.passed },
                                                     { "maxMicros", impl.spaceTrigger.maxMicros }, { "totalMicros", impl.spaceTrigger.totalMicros } } },
                           { "keywordsWithoutEngine", impl.keywordsWithoutEngine }, { "templatesWithoutEngine", impl.templatesWithoutEngine },
                           { "wordsWithoutCore", impl.wordsWithoutCore },
                           { "late", Json { { "arrived", impl.lateCompletionsArrived }, { "served", impl.lateCompletionsServed } } },
                           { "fromWord", impl.completionsFromWord } };
    Json startups = Json::array();
    if (auto text = os::fs::read(base::join_path(impl.cacheDirectory, "startups.json"))) {
        startups = Json::parse(*text, nullptr, false);
        if (!startups.is_array()) startups = Json::array();
    }
    return Json { { "root", root_ }, { "key", key_ }, { "about", impl.about_json() }, { "startup", impl.startup_json_() }, { "startups", std::move(startups) }, { "cacheDirectory", impl.cacheDirectory },
                  { "trusted", impl.options.trusted }, { "state", std::string { to_string(impl.compute_state()) } }, { "project", std::move(project) },
                  { "toolEnvironment", std::move(environment) }, { "toolRuns", std::move(toolRuns) },
                  { "plan", std::move(plan) }, { "engines", std::move(engines) }, { "requests", std::move(requests) },
                  { "answers", impl.answers_ ? impl.answers_->report() : Json() },
                  { "semanticTokens", impl.highlights.report() },
                  { "slowestFiles", std::move(slowestFiles) },
                  { "completion", std::move(completionCosts) },
                  { "documents", Json { { "changes", impl.documentChanges }, { "lastChangeAt", impl.lastDocumentChangeAt } } },
                  { "eventTotals", impl.journal.totals() }, { "events", impl.journal.recent(300) } };
}

std::uint64_t Workspace::reset_cache() {
    std::error_code ignored;
    auto& impl = *impl_;
    std::uint64_t freed { 0 };
    for (const auto& engine : impl.engines) freed += engine->clear_cache_on_request();
    freed += cache::remove_tree(impl.tokens_directory()).bytes;
    for (const auto& entry : base::children(impl.cacheDirectory)) {
        const std::string_view name { base::file_name(entry) };
        // The cached models go; the instance's own `instance.json` stays (review 2026-10-03). It is
        // not cache -- it is the heartbeat that says this directory is alive. Removing it here made
        // a guest look dead to the reapers for up to a renewal interval while it was rebuilding
        // (the old comment claimed the opposite); like `owner.lease`, it survives a reset.
        if (!name.starts_with("model.") || !name.ends_with(".json")) continue;
        if (const auto stamp = base::file_stamp(entry)) freed += stamp->size;
        std::filesystem::remove_all(entry, ignored);
    }
    impl.producerStatesResetPending = true;   // removed as the load below starts, or the next one if one runs (XS-4)
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
    const bool dryRun { outcome.value("dryRun", false) };
    bool retrigger { false };
    bool pendingReportOnly { false };
    {
        std::lock_guard lock(impl.cacheMutex_);
        // The single-flight token is released here for every origin (a dry-run command posts its
        // event too, with nothing to remember); a pass coalesced while this one ran starts now.
        impl.sweepRunning_.store(false);
        if (impl.sweepPending_) {
            impl.sweepPending_ = false;
            retrigger = true;
            pendingReportOnly = impl.sweepPendingReportOnly_;
            impl.sweepPendingReportOnly_ = true;   // the next batch starts from the neutral mode again
        }
        // A preview changed nothing: no numbers are remembered and nothing is journalled as swept.
        if (!dryRun) {
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
    }
    if (!dryRun) {
        impl.journal.add("cache-swept", Json { { "bytes", bytes }, { "files", files }, { "instances", instances },
                                               { "failed", failed }, { "origin", outcome.value("origin", std::string {}) } });
        impl.update_status();
    }
    if (retrigger) impl.start_cache_task_("coalesced", base::file_time_now(), pendingReportOnly);
}

Json Workspace::cache_report() const {
    auto& impl = *impl_;
    Json numbers;
    bool refresh { false };
    {
        std::lock_guard lock(impl.cacheMutex_);
        // At most 30 s old (C-13.1: the server is the only source, and it answers from its cache).
        // A stale snapshot is answered at once and refreshed off the loop (review 2026-10-03):
        // walking a full cache tree on the event loop held every request behind it. The one
        // synchronous walk left is a server's very first answer, before any background pass has
        // reported -- small by definition, and never repeated.
        if (!impl.cacheReportAt_ || Clock::now() - *impl.cacheReportAt_ > std::chrono::seconds { 30 }) {
            if (impl.cacheReport_.is_null()) {
                impl.cacheReport_ = cache::report(impl.cacheDirectory, impl.cache_budget(), std::chrono::system_clock::now(), impl.ownToken_);
                impl.cacheReportAt_ = Clock::now();
            } else {
                refresh = true;
            }
        }
        numbers = impl.cacheReport_;
    }
    if (refresh) impl.start_cache_task_("report", base::file_time_now(), true);
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
    // The two prompts share one facts object: it was rendered twice per report (review 2026-10-03).
    // Copy-initialisation on purpose: `Json facts { json }` is nlohmann's initializer-list trap and
    // wraps the object in a one-element array, which the prompt renderers then reject.
    const Json promptFacts = impl.cache_prompt_facts_();
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
                    { "paths", Json { { "cacheRoot", impl.workspaceDirectory_ }, { "logDirectory", base::parent_path(log::file_path()) },
                                      { "bundlesDirectory", bundle::default_directory() } } },
                    { "cli", Json { { "cacheQuery", "mcppls cache --format json" }, { "sweep", "mcppls cache --prune --dry-run" } } },
                    { "prompts", Json { { "agent", cache::agent_prompt(promptFacts) },
                                        { "issue", cache::issue_prompt(promptFacts) } } } };
    if (core && core->toPrepare > 0) envelope["progress"] = Json { { "done", core->prepared }, { "total", core->toPrepare } };
    std::lock_guard lock(impl.cacheMutex_);
    if (impl.lastSweepAt_) {
        envelope["lastSweep"] = Json { { "at", *impl.lastSweepAt_ }, { "freedBytes", impl.lastSweepFreed_ },
                                       { "files", impl.lastSweepFiles_ }, { "failed", impl.lastSweepFailed_ } };
    }
    return envelope;
}

SweepStart Workspace::prepare_sweep(const Json& params) {
    auto& impl = *impl_;
    SweepStart start;
    // `=`: braces would make an array holding the list, and no category would ever be found in it.
    const Json given = params.value("categories", Json::array());
    const auto wants = [&](std::string_view category, bool byDefault) {
        if (!given.is_array() || given.empty()) return byDefault;
        return std::ranges::find(given, Json(std::string { category })) != given.end();
    };
    start.dryRun = params.value("dryRun", false);
    if (impl.sweepRunning_.load()) {
        // C-13.1: one sweep at a time; a caller while one runs learns it and does the math itself.
        start.immediate = Json { { "ok", true }, { "alreadyRunning", true }, { "dryRun", start.dryRun },
                                 { "freedBytes", std::uint64_t { 0 } }, { "files", std::size_t { 0 } },
                                 { "instances", std::size_t { 0 } }, { "roots", 1 } };
        return start;
    }
    impl.sweepRunning_.store(true);
    start.started = true;
    start.copies = wants("copies", true);
    start.instances = wants("instances", true);
    start.trash = wants("trash", true);
    start.staleCommands = wants("staleCommands", false);
    start.budget = wants("budget", true);
    // The safety rules of S3 5.8: nothing an engine holds, no canonical BMI, no engine stopped.
    std::int64_t bound { base::file_time_now() };
    for (const auto& engine : impl.engines) {
        if (const std::int64_t began { engine->generation_started_at() }; began > 0) {
            start.engineLive = true;
            bound = std::min(bound, began);
        }
    }
    start.bound = bound;
    start.cacheDirectory = impl.cacheDirectory;
    start.workspaceDirectory = impl.workspaceDirectory_;
    start.workspacesRoot = base::join_path(base::cache_directory(), "workspaces");
    start.ownToken = impl.ownToken_;
    start.limits = impl.cache_budget();
    start.grace = impl.cache_grace();
    start.now = std::chrono::system_clock::now();
    return start;
}

Json run_prepared_sweep(const SweepStart& start) {
    std::uint64_t freed { 0 };
    std::size_t files { 0 }, instances { 0 }, failed { 0 };
    if (start.copies) {
        for (const auto& context : cache::contexts_of(start.cacheDirectory)) {
            const cache::Sweep one { cache::sweep_copies(cache::modules_root(context), start.bound, start.dryRun) };
            freed += one.bytes;
            files += one.files;
            failed += one.failed;
        }
    }
    if (start.instances) {
        const cache::Sweep one { cache::sweep_instances(start.workspaceDirectory, start.now, start.grace, start.dryRun) };
        freed += one.bytes;
        instances += one.instances;
        failed += one.failed;
    }
    if (start.trash) {
        const cache::Sweep one { cache::sweep_trash(start.cacheDirectory, start.dryRun) };
        freed += one.bytes;
        failed += one.failed;
    }
    // C-2's job on the engine path, and only ever by explicit request, and only where no engine
    // lives: a command that stops for nothing must not take the command directories either. What
    // it counts is what it removed (review 2026-10-03): a removal that fails says so, and nothing
    // is counted freed that is still on disk.
    if (start.staleCommands && !start.engineLive) {
        for (const auto& context : cache::contexts_of(start.cacheDirectory)) {
            for (const auto& build : engine::clangd::stale_module_builds(base::join_path(context, "cdb"), 2)) {
                if (start.dryRun) {
                    freed += cache::tree_bytes(build);
                    continue;
                }
                const cache::Removal removed { cache::remove_tree(build) };
                freed += removed.bytes;
                failed += removed.failed;
            }
        }
    }
    if (start.budget && !start.dryRun) {
        const cache::Sweep one { cache::enforce_budget(start.workspacesRoot, start.limits, base::file_name(start.workspaceDirectory), start.now) };
        freed += one.bytes;
        files += one.files;
        failed += one.failed;
    }
    Json outcome { { "origin", "command" }, { "dryRun", start.dryRun }, { "bytes", freed }, { "files", files },
                   { "instances", instances }, { "failed", failed } };
    if (!start.dryRun) {
        try {
            outcome["report"] = cache::report(start.cacheDirectory, start.limits, std::chrono::system_clock::now(), start.ownToken);
        } catch (...) {
            outcome["report"] = Json::object();   // the reply stands even when a fresh report does not
        }
    }
    return outcome;
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
    if (options.serverCommand.empty()) {
        log::warning("cannot review {}: the server does not know its own executable", root_);
        return false;
    }
    os::process::spawn_options spawn;
    spawn.program = options.serverCommand.front();
    spawn.args.assign(options.serverCommand.begin() + 1, options.serverCommand.end());
    spawn.args.insert(spawn.args.end(), { "review", "--format", "lsp", "--root", root_ });
    const Json settings = arguments.is_array() && !arguments.empty() && arguments[0].is_object() ? arguments[0] : Json::object();
    if (auto base = lsp::string_at(settings, "base"); base && !base->empty()) spawn.args.insert(spawn.args.end(), { "--base", *base });
    if (!options.payloadDirectory.empty()) spawn.args.insert(spawn.args.end(), { "--payload", options.payloadDirectory });
    if (!options.clangd.empty()) spawn.args.insert(spawn.args.end(), { "--clangd", options.clangd });
    if (!options.kit.empty()) spawn.args.insert(spawn.args.end(), { "--kit", options.kit });
    if (!options.mcpp.empty()) spawn.args.insert(spawn.args.end(), { "--mcpp", options.mcpp });
    if (options.mcppTarget != "auto") spawn.args.insert(spawn.args.end(), { "--mcpp-target", options.mcppTarget });
    if (!options.database.empty()) spawn.args.insert(spawn.args.end(), { "--database", options.database });
    if (options.engine != "clangd") spawn.args.insert(spawn.args.end(), { "--engine", options.engine });
    if (!options.discoverCompilers) spawn.args.push_back("--no-discover");
    if (!options.trusted) spawn.args.push_back("--untrusted");
    spawn.cwd = root_;
    // This server's input is the editor's channel: the review reads nothing.
    spawn.in = os::process::stdio::null;
    spawn.out = os::process::stdio::pipe;
    spawn.err = os::process::stdio::error;
    // The review starts an engine and may start the build tool: ending it must end all of that,
    // which is what a unit of its own buys (design 4.2). shut_down() kills the unit, not one process.
    spawn.own_unit = true;
    spawn.bound_lifetime = true;
    auto started = os::process::process::spawn(spawn);
    if (!started) {
        log::warning("cannot review {}: {}", root_, started.error().message());
        return false;
    }
    impl_->reviewProcess = std::make_shared<os::process::process>(std::move(*started));
    log::info("review of {} started", root_);
    // The review runs its own headless session; this one keeps serving the editor meanwhile.
    std::thread { [process = impl_->reviewProcess, events = impl_->events, rootKey = key_] {
        std::string output;
        while (true) {
            auto chunk = process->read_output();
            if (!chunk || chunk->empty()) break;
            output += *chunk;
        }
        const auto status = process->wait();
        const int code { status && !status->by_environment ? status->code : -1 };
        events->push(Event { EventKind::review_finished, Json { { "output", std::move(output) }, { "exitCode", code } }, 0, {}, rootKey, {} });
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
        const auto environment = mcppls::toolenv::get();
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

bool Workspace::install_kit(std::string_view target) { return impl_->install_kit(target); }

void Workspace::handle_kit_installed(const Json& outcome) { impl_->kit_installed(outcome); }

void Workspace::apply_build_program() {
    // Asked for: the project is described again whether or not a change is waiting.
    if (impl_->buildProgramPending.empty() && impl_->buildProgramHeld.empty()) {
        log::info("describing {} again (asked for)", root_);
        impl_->start_model_load();
        return;
    }
    impl_->apply_build_program("asked for");
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
    impl_->options.buildProgramApply = options.buildProgramApply;
    impl_->options.discoverCompilers = options.discoverCompilers;
    impl_->options.database = options.database;
    impl_->options.mcpp = options.mcpp;
    impl_->options.producerTimeout = options.producerTimeout;
    impl_->options.buildDiscovery = options.buildDiscovery;
    impl_->options.buildDiscoveryProviders = options.buildDiscoveryProviders;
    impl_->options.mcppTarget = options.mcppTarget;
    impl_->options.buildDiscoveryAskBeforeDownload = options.buildDiscoveryAskBeforeDownload;
    impl_->compilerOverride = compilerOverride;
    impl_->kitEnabled = kitEnabled;
    impl_->read_kits();
    impl_->select_kit();
    impl_->schedule_reload();
}

void Workspace::set_mcpp_target(const std::string& target) {
    if (impl_->options.mcppTarget == target) return;
    impl_->options.mcppTarget = target;
    // Keep the value for later discovery, without rerunning an unrelated build tool.
    if (impl_->detectedSource == project::SourceKind::mcpp) impl_->schedule_reload();
}

void Workspace::apply_live_options(const SessionOptions& options) {
    impl_->options.buildDiscoveryAskBeforeDownload = options.buildDiscoveryAskBeforeDownload;
    impl_->update_status();
}

void Workspace::handle_model_loaded(int generation, std::shared_ptr<project::ProjectModel> model, bool fromProducer) {
    if (generation != impl_->modelGeneration) return;
    impl_->handle_model_loaded(std::move(model), fromProducer);
}

void Workspace::handle_target_selected(int generation, const Json& message, std::shared_ptr<project::ProjectModel> selected) {
    if (selected) impl_->target_selected(generation, message, selected->targetSelection);
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
