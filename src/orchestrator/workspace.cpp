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

constexpr std::array<std::string_view, 6> BUILD_FILES { "mcpp.toml", "mcpp.lock", "CMakeLists.txt", "CMakePresets.json",
                                                        "compile_commands.json", "build_database.json" };

constexpr std::array<std::string_view, 7> WATCH_POLL_SKIP_DIRECTORIES { "target", "build", "node_modules", "out",
                                                                        "_build", "cmake-build-debug", "cmake-build-release" };

// F15 (fix plan 2026-09-26): a completion at the start of a declaration waits this long for the core
// engine; past it the module-syntax keywords go out alone, as an incomplete list the client asks
// again for as the person types on. A clangd stuck on the file no longer takes `import` with it.
constexpr std::chrono::milliseconds KEYWORD_PATIENCE { 1500 };

// Changes to cxxModules/status that keep its state are sent at most this often (S3 4).
constexpr std::chrono::milliseconds STATUS_COALESCE { 250 };
// import-hang plan §6: a change from a working state to degraded goes out only once it has lasted this
// long, so a condition that passes by itself (a file set aside and handed back as the user types) never
// reaches the editor. error goes out at once.
constexpr std::chrono::milliseconds DEGRADED_HOLD { 3000 };

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
        std::map<std::string, std::size_t, std::less<>> answeredBy;
    };
    std::map<std::string, MethodStats, std::less<>> requestStats;
    // F9 (D4): what space-triggered completion costs. Most never pass the gate and cost one look at a line.
    struct SpaceTriggerStats {
        std::size_t count { 0 };
        std::size_t passed { 0 };
        std::int64_t maxMicros { 0 };
        std::int64_t totalMicros { 0 };
    };
    SpaceTriggerStats spaceTrigger;
    std::size_t keywordsWithoutEngine { 0 };   // F15: keyword completions answered before the core engine did
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
        // F15: a completion's module-syntax keywords, merged into whatever the engines answer, and
        // when they go out without the core engine.
        Json keywords;
        std::optional<Clock::time_point> keywordsBy;
    };
    std::map<std::uint64_t, Job> jobs;
    std::uint64_t nextJob { 1 };

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
    // S5 2.2: advanced whenever a document, a watched file, the model or the plan changes.
    std::uint64_t snapshotGeneration { 0 };

    // Timers.
    std::optional<Clock::time_point> reloadAt;
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
    bool inferredLoadStarted { false };
    // Fix plan F4 (D1): a project whose build system was detected gets clangd with the build tool's model, not a
    // provisional one scanned from its sources: until the producer answers, or CORE_WAIT_LIMIT has passed, the
    // core engine is given no plan and asked nothing, and mcppls's own engine answers what it can.
    static constexpr std::chrono::seconds CORE_WAIT_LIMIT { 60 };
    std::optional<Clock::time_point> coreWaitUntil;
    bool coreWaitOver { false };
    std::optional<std::chrono::milliseconds> producerElapsed;   // set while the producer is past its soft bound
    // Written by the load thread, read by the event loop: how long the producer has been running
    // once it passed its soft bound. Nothing else crosses that boundary.
    std::shared_ptr<std::atomic<std::int64_t>> producerSlowMs { std::make_shared<std::atomic<std::int64_t>>(0) };
    std::optional<Clock::time_point> producerSoftAt;
    std::optional<Clock::time_point> lastManualReloadAt;
    std::string journaledToolEnvironment;                // the environment source the journal last recorded

    std::string lastStatus;                  // the last cxxModules/status sent, serialized
    State lastSentState { State::starting };
    std::optional<Clock::time_point> lastStatusSentAt;
    std::optional<Clock::time_point> statusFlushAt;   // a coalesced change goes out then
    std::optional<Clock::time_point> degradedSince;   // when compute_state() turned degraded, while that is held back
    State lastReportedState { State::starting };      // the state last let through DEGRADED_HOLD

    Impl(std::string root_, std::string key_, SessionOptions options_, engine::PayloadPaths payload_, bool payloadCorrupt_,
         bool kitEnabled_, std::string compilerOverride_, std::shared_ptr<EventChannel> events_, ClientSink& client_)
        : root { std::move(root_) }, key { std::move(key_) }, options { std::move(options_) }, events { std::move(events_) }, client { client_ },
          compilerOverride { std::move(compilerOverride_) }, kitEnabled { kitEnabled_ }, payload { std::move(payload_) },
          payloadCorrupt { payloadCorrupt_ } {
        const std::string workspaceDirectory { base::join_path(platform::dirs::cache_directory(), base::join_path("workspaces", project::workspace_key(root))) };
        lease = WorkspaceLease::acquire(workspaceDirectory, std::chrono::system_clock::now());
        cacheDirectory = lease->directory();
        if (!lease->shared()) leaseRenewAt = Clock::now() + LEASE_RENEWAL;
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
        consider(lastResortAt);
        consider(producerSoftAt);
        consider(sdkCheckAt);
        consider(statusFlushAt);
        consider(leaseRenewAt);
        consider(tokensRefreshAt);
        for (const auto& [id, job] : jobs) consider(job.keywordsBy);
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
        if (job.keywords.is_array() && !job.keywords.empty() && coreEngine != nullptr
            && std::ranges::find(job.answerers, coreEngine) != job.answerers.end()) {
            job.keywordsBy = job.started + KEYWORD_PATIENCE;
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

    // F15: the keywords go out without the core engine, which has not answered in KEYWORD_PATIENCE.
    // The job is finished first, so the engines' answers to the cancellation find nothing to finish.
    void answer_keywords_without_engine(std::uint64_t jobId) {
        const Json clientId { jobs.at(jobId).clientId };
        ++keywordsWithoutEngine;
        jobs.at(jobId).answeredBy = "mcppls";
        finish_job(jobId, Json(nullptr));
        for (const auto& engine : engines) engine->cancel(clientId);
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
        answerer->request(job.view, job.message, [this, jobId, engineId = std::string { answerer->id() }](engine::Answer answer) {
            auto current = jobs.find(jobId);
            if (current == jobs.end()) return;
            switch (answer.kind) {
            case engine::Answer::Kind::result:
                if (!answer.value.is_null()) {
                    current->second.answeredBy = engineId;
                    finish_job(jobId, std::move(answer.value));
                    return;
                }
                ask_next(jobId);
                return;
            case engine::Answer::Kind::unavailable: ask_next(jobId); return;
            case engine::Answer::Kind::error:
                // F15: a completion that has keywords to give gives them rather than the engine's error.
                if (current->second.keywords.is_array() && !current->second.keywords.empty()) finish_job(jobId, Json(nullptr));
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
    // scanning), ten when there is none at all.
    void adopt_cached_model() {
        const project::Detection detection { project::detect_project(root, options.database) };
        detectedSource = detection.kind;
        detectedManifest = detection.manifest;
        if (detection.kind == project::SourceKind::inferred) return;   // no producer: nothing to wait for
        // An untrusted workspace runs no build tool, and a model a trusted session once cached is
        // that build tool's word: scanned sources are what this workspace gets (design 4.1).
        if (!options.trusted) return;

        auto cached = project::load_model(cacheDirectory, detection.kind);
        if (!cached) {
            loadGiveUpAt = Clock::now() + std::chrono::seconds { 10 };
            return;
        }
        producerPath = cached->producer;
        producerVersion = cached->producerVersion;
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
        };
        return description.dump();
    }

    // One file per source, so a model built from scanned sources can never replace what the build
    // tool said (design P5), and a fingerprint of the inputs, so the next session can tell whether
    // what it has is still current (design 4.1).
    void save_model_cache() const {
        if (!model) return;
        project::CachedModel cached;
        cached.model = *model;
        cached.producer = producerPath;
        cached.producerVersion = producerVersion;
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
        // Design 4.4: a run the server starts by itself is offline unless the user allowed the
        // network; `off` means the build tool is not run at all, and what is cached or scanned is
        // all there is. Design 4.2: the hard bound is a minute, ten when the user allowed the
        // network and a download may be part of the answer.
        const bool online { options.buildTool == "online" };
        load.offline = !online;
        load.runBuildTool = options.buildTool != "off";
        load.producerHard = options.producerTimeout.count() > 0
                                ? std::chrono::milliseconds { options.producerTimeout }
                                : (online ? std::chrono::milliseconds { std::chrono::minutes { 10 } }
                                          : std::chrono::milliseconds { std::chrono::seconds { 60 } });
        load.producerSoft = std::chrono::seconds { 5 };
        // With a model already in hand nothing waits for the environment; without one, the producer
        // is worth the wait, because a producer found through the wrong PATH describes another build.
        load.environmentWait = model ? std::chrono::milliseconds { 0 } : std::chrono::milliseconds { std::chrono::seconds { 10 } };
        load.rootKey = key;
        producerSlowMs->store(0);
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
        // Fix plan F4: the build tool answered, whatever it said; clangd waits no longer. A model kept below
        // (a failed or poorer reload) is planned again for it.
        if (coreWaitUntil && !coreWaitOver) {
            coreWaitOver = true;
            if (model) replanAt = Clock::now();
        }
        ++snapshotGeneration;
        needsDownload.clear();
        for (const auto& issue : loadedModel->issues) {
            if (issue.code == spec::NEEDS_DOWNLOAD) needsDownload = issue.message;
        }
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
            if (!lastEdit || edited->second > *lastEdit) lastEdit = edited->second;
        }
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
        journal.add("plan", Json { { "context", contextSet.empty() ? std::string { "default" } : contextSet }, { "entries", plan.entries.size() },
                                   { "stdUnits", plan.stdUnits }, { "standIns", plan.stubModules }, { "openSources", plan.openSources },
                                   { "leftOut", plan.excludedFiles.size() },
                                   { "issues", plan.issues.size() } });
        const bool coreWaits { core_waits_for_producer() };
        if (coreWaits && !coreWaitUntil) {
            coreWaitUntil = Clock::now() + CORE_WAIT_LIMIT;
            log::info("clangd waits for {} to describe {} (at most {} s); mcppls's own engine answers meanwhile", project::to_string(detectedSource), root,
                      CORE_WAIT_LIMIT.count());
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

    bool core_waits_for_producer() const {
        return coreEngine != nullptr && model && modelOrigin == "inferred" && loading && !coreWaitOver
               && detectedSource != project::SourceKind::inferred && options.trusted && options.buildTool != "off";
    }
    void schedule_reload() { reloadAt = Clock::now() + std::chrono::milliseconds { 1500 }; }

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
        } else if (kit) {
            profile = Json { { "kind", "semantic-kit" }, { "stdlib", std::format("{} {}", kit->stdlibName, kit->stdlibVersion) }, { "target", kit->target } };
        } else {
            profile = Json { { "kind", "semantic-kit" }, { "stdlib", "unknown" }, { "target", std::string { mcppls::os::PLATFORM } } };
        }
        return profile;
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
        const State state { compute_state() };
        if (state == State::degraded && lastReportedState != State::degraded) {
            const auto now = Clock::now();
            if (!degradedSince) degradedSince = now;
            if (now < *degradedSince + DEGRADED_HOLD) {
                if (!statusFlushAt || *degradedSince + DEGRADED_HOLD < *statusFlushAt) statusFlushAt = *degradedSince + DEGRADED_HOLD;
                return;
            }
        }
        if (state != State::degraded) degradedSince.reset();
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
            for (const auto& issue : engine->status().issues) add(issue.code, issue.message, issue.command, "Fix", issue.category);
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
                std::format("the build description needs a download: {}. Run the build tool in your terminal, "
                            "or turn on mcppls.buildTool = online. It may also be an older build tool: updating it is worth trying",
                            needsDownload),
                "mcppls.runBuildToolInTerminal", "Run in Terminal", "environment");
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
                if (issue.code == spec::NEEDS_DOWNLOAD) continue;
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
        if (core && core->toPrepare > 0) params["progress"] = Json { { "done", core->prepared }, { "total", core->toPrepare } };
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
        const auto now = Clock::now();
        for (const auto& engine : engines) engine->handle_timers();
        {
            std::vector<std::uint64_t> due;
            for (const auto& [id, job] : jobs) {
                if (job.keywordsBy && *job.keywordsBy <= now) due.push_back(id);
            }
            for (const auto id : due) {
                if (jobs.contains(id)) answer_keywords_without_engine(id);
            }
        }
        if (leaseRenewAt && *leaseRenewAt <= now) {
            lease->renew(std::chrono::system_clock::now());
            leaseRenewAt = now + LEASE_RENEWAL;
        }
        if (reloadAt && *reloadAt <= now) {
            reloadAt.reset();
            start_model_load();
        }
        if (replanAt && *replanAt <= now) replan();
        if (coreWaitUntil && !coreWaitOver && *coreWaitUntil <= now) {
            coreWaitOver = true;
            if (modelOrigin == "inferred" && model) {
                log::warning("{} has not described {} in {} s; clangd starts with the model scanned from its sources", project::to_string(detectedSource), root,
                             CORE_WAIT_LIMIT.count());
                journal.add("engine-wait-over", Json { { "seconds", CORE_WAIT_LIMIT.count() } });
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
}

void Workspace::handle_watched_files(const Json& changes) {
    ++impl_->snapshotGeneration;
    bool reload { false };
    bool replan { false };
    for (const auto& change : changes) {
        const std::string path { impl_->path_of_uri(change.value("uri", std::string {})) };
        if (path.empty()) continue;
        const std::string_view name { base::file_name(path) };
        const int type { change.value("type", 2) };
        // S2 5: an input the producer named changes what it would answer, so the model is loaded again.
        // A source among them still updates the index at once below; the reload only confirms the model.
        if (impl_->model && impl_->model->source != project::SourceKind::inferred && name != "compile_commands.json"
            && matches_watch_entries(impl_->model->watch, impl_->root, path)) {
            reload = true;
        }
        if (is_build_file(name)) {
            // mcpp rewrites its own compile_commands.json while the model loads.
            if (name == "compile_commands.json" && impl_->model && impl_->model->source == project::SourceKind::mcpp) continue;
            reload = true;
            continue;
        }
        if (!project::is_cxx_source_name(path) || impl_->documents_.find_by_path(path) != nullptr) continue;
        if (type == 3) {
            impl_->index.remove(path);
        } else if (auto text = platform::fs::read_file(path)) {
            impl_->index.update(path, *text);
        }
        if (impl_->model && impl_->model->source == project::SourceKind::inferred && type != 2) reload = true;
        else replan = true;
    }
    if (reload || replan) {
        for (const auto& engine : impl_->engines) engine->sources_changed();
    }
    if (reload) impl_->schedule_reload();
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
        requests[method] = Json { { "count", stats.count }, { "empty", stats.empty }, { "errors", stats.errors }, { "cancelled", stats.cancelled },
                                  { "p50Ms", percentile(0.5) }, { "p95Ms", percentile(0.95) }, { "maxMs", static_cast<std::int64_t>(stats.maxMs) },
                                  { "answeredBy", std::move(answeredBy) } };
    }
    // F9, F15: what space-triggered completion cost, and how often keywords answered without the core engine.
    Json completionCosts { { "spaceTrigger", Json { { "count", impl.spaceTrigger.count }, { "passed", impl.spaceTrigger.passed },
                                                     { "maxMicros", impl.spaceTrigger.maxMicros }, { "totalMicros", impl.spaceTrigger.totalMicros } } },
                           { "keywordsWithoutEngine", impl.keywordsWithoutEngine } };
    return Json { { "root", root_ }, { "key", key_ }, { "cacheDirectory", impl.cacheDirectory },
                  { "trusted", impl.options.trusted }, { "state", std::string { to_string(impl.compute_state()) } }, { "project", std::move(project) },
                  { "toolEnvironment", std::move(environment) }, { "toolRuns", std::move(toolRuns) },
                  { "plan", std::move(plan) }, { "engines", std::move(engines) }, { "requests", std::move(requests) },
                  { "completion", std::move(completionCosts) },
                  { "eventTotals", impl.journal.totals() }, { "events", impl.journal.recent(300) } };
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
