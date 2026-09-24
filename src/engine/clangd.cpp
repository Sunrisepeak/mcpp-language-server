module mcppls.engine.clangd;

import std;
import nlohmann.json;
import mcppls.os;
import mcppls.base.error;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.base.uri;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.lsp.jsonrpc;
import mcppls.lsp.protocol;
import mcppls.project.scan;
import mcppls.normalize.plan;
import mcppls.engine;
import mcppls.engine.clangd.definition;
import mcppls.engine.clangd.guard;
import mcppls.engine.clangd.primer;
import mcppls.engine.clangd.process;
import mcppls.engine.clangd.workarounds;
import mcppls.engine.native.index;

namespace mcppls::engine::clangd {

namespace log = base::log;
namespace midx = mcppls::index;

EngineTraits traits_for_version(std::string_view version, std::span<const std::string> disabled) {
    // Every compensation for clangd's own defects is a registered workaround (import-hang plan §9).
    const auto on = [&](std::string_view id) { return needs(id, version) && std::ranges::find(disabled, id) == disabled.end(); };
    return EngineTraits {
        .importNavigation = false,
        .pushesDiagnostics = true,
        .hangsOnUnresolvedImports = on(UNRESOLVED_IMPORT_STAND_INS),
        .needsModulePreparation = on(MODULE_PREPARATION),
        .needsModuleHints = on(MODULE_HINTS),
        .msvcStlNeedsNoAlignedAllocation = on(MSVC_STL_ALIGNED_ALLOCATION),
        .hangsOnTrailingDotModuleName = on(TRAILING_DOT_MODULE_NAME),
        .kitStdlibVersion = std::string { version },
        .tested = version == "23.1.0",
    };
}

bool is_interactive(std::string_view method) {
    static constexpr std::array<std::string_view, 9> INTERACTIVE { "textDocument/definition", "textDocument/declaration", "textDocument/hover",
        "textDocument/completion", "textDocument/signatureHelp", "textDocument/documentHighlight", "textDocument/typeDefinition",
        "textDocument/implementation", "completionItem/resolve" };
    return std::ranges::find(INTERACTIVE, method) != INTERACTIVE.end();
}

Clock::time_point wait_limit(std::string_view method, std::chrono::milliseconds requestTimeout, Clock::time_point arrived) {
    return arrived + (is_interactive(method) ? std::min(requestTimeout, INTERACTIVE_LIMIT) : requestTimeout);
}

bool keep_waiting(const PendingRequest& request, bool filePreparing, std::optional<Clock::time_point> lastProgress, Clock::time_point now) {
    if (request.purpose != Purpose::client || !filePreparing || !lastProgress) return false;
    return now < request.limit && now - *lastProgress < INTERACTIVE_TIMEOUT;
}

namespace {

class ClangdEngine final : public Engine {
private:
    Options options_;
    EngineTraits traits_;
    Host* host_ { nullptr };
    std::function<void(Json)> sink_;
    std::unique_ptr<Process> process_;
    std::vector<MethodCapability> methods_ {
        { std::string { EVERY_METHOD }, Role::answer, 0 },
        { std::string { lsp::method::TEXT_DOCUMENT_DOCUMENT_SYMBOL }, Role::merge, 0 },
        { std::string { lsp::method::WORKSPACE_SYMBOL }, Role::merge, 0 },
    };

    std::string databaseDirectory_;
    std::string primeDirectory_;
    std::string moduleHintDirectory_;
    std::string stubDirectory_;       // stand-ins for modules nothing usable provides (robustness design C2)

    // Plan.
    bool planApplied_ { false };
    std::string writtenDatabase_;
    std::string writtenStructure_;   // the written database without module hints
    std::map<std::string, std::string, std::less<>> writtenArguments_;   // path key -> the unit's engine command, without hints
    std::set<std::string> excluded_;   // path keys
    // Modules clangd could not find (robustness design C3), with the reason and the unit the plan had
    // providing each then. An entry is forgotten when that unit, or its command, changes; not on every save.
    struct UnresolvedModule {
        std::string reason;
        std::string provider;                          // the plan's unit for the module; empty when it had none
        std::optional<platform::fs::FileStamp> stamp;  // of that unit
        std::string command;                           // its engine command
    };
    std::map<std::string, UnresolvedModule, std::less<>> unresolvedModules_;
    std::set<std::string, std::less<>> reportedFailures_;   // modules whose compile failure was logged
    std::set<std::string, std::less<>> loggedStandIns_;     // stand-in modules already named at warning
    std::map<std::string, std::string, std::less<>> moduleCommands_;   // importable module -> its unit's engine command
    // robustness design C5: clangd could not build the toolchain's standard library; C++ units are read with the kit.
    bool stdFromKit_ { false };
    // Closure-scoped failure (real-project plan RP1.1, design P1): a module clangd could not compile
    // dooms everything that imports it, transitively, and any plain importer of the closure. Their
    // files are routed to mcppls's own engine at once, never restart clangd, and are never primed
    // again — until the failed module's own unit or command changes.
    struct DoomRoot {
        std::string reason;
        std::string provider;                          // the plan's unit for the module; empty when it had none
        std::string command;                           // its engine command
        // The unit's own source and every module source it imports, transitively, as they were when
        // it failed: a fix in any of them -- not only in the failed unit itself -- is a reason to try again.
        std::map<std::string, std::optional<platform::fs::FileStamp>, std::less<>> inputs;
    };
    std::map<std::string, DoomRoot, std::less<>> doomRoots_;    // modules clangd reported it could not compile
    std::set<std::string, std::less<>> doomedModules_;          // roots and everything that imports them, transitively
    struct DoomedFile {
        std::string rootModule;   // the module that actually failed to compile
        std::string viaModule;    // the doomed module this file provides or directly imports
        std::string reason;
    };
    std::map<std::string, DoomedFile, std::less<>> doomedFiles_;                // path key -> why
    std::map<std::string, std::vector<std::string>, std::less<>> moduleRequires_;   // module -> the modules it imports, from the plan
    std::map<std::string, std::vector<std::string>, std::less<>> fileImports_;      // path key -> modules it imports directly
    std::map<std::string, std::string, std::less<>> fileModule_;                    // path key -> its module, any role
    static constexpr std::chrono::seconds PREPARATION_STALL_TIMEOUT { 60 };

    // Parallel module preparation (primer.cppm): `import M;` units opened in clangd.
    Primer primer_;
    std::map<std::string, std::string, std::less<>> primeModuleByPath_;
    std::map<std::string, Clock::time_point, std::less<>> primeDeadlines_;
    std::map<std::string, std::string, std::less<>> heldPrimeUnits_;
    std::optional<Clock::time_point> lastPrimeProgressAt_;
    std::map<std::string, std::string, std::less<>> moduleSources_;   // importable module -> the unit providing it, from the plan
    // clangd's persistent module cache as the plan found it (cached_bmis), read the first time a
    // module becomes ready for preparation and not again until the next plan.
    std::optional<std::map<std::string, std::vector<std::string>, std::less<>>> startupBmis_;

    // Process.
    int generation_ { 0 };
    bool handshakeDone_ { false };
    bool accepting_ { false };
    bool unavailable_ { false };
    // 0.0.3 plan B1: a clangd that dies before its handshake. The server's own initialize waits on
    // this engine settling, so such an exit has to settle it too, or the editor never gets past
    // initialize. A loader's message on its standard error (loader_failure) means it never can
    // run here: incompatible_, and no more restarts. The line and the exit arrive on different
    // threads, in either order; closedGeneration_ is how the later one knows the earlier came.
    int earlyExits_ { 0 };                  // exits before a handshake since the last one completed
    std::optional<std::string> loadFailure_;   // this generation's loader message
    int closedGeneration_ { -1 };
    bool incompatible_ { false };
    std::map<std::int64_t, PendingRequest> pending_;
    std::int64_t nextId_ { 1 };
    // A client message not yet given to clangd, and when it must have been answered (wait_limit;
    // never, for a notification).
    struct Waiting {
        Json message;
        Reply reply;
        Clock::time_point limit { Clock::time_point::max() };
    };
    std::vector<Waiting> deferred_;   // client messages before clangd accepts traffic
    // A request against a held_ file (robustness design C2): the comment on HeldFile says such a
    // file "waits, answered by mcppls's own engine, until clangd has read a database that has it",
    // and that holds for a method mcppls's own engine also answers (a fallback claims the request
    // meanwhile). A method with no such fallback -- textDocument/references, the two hierarchies --
    // has nothing to fall back to, so its own request must wait out the hold instead, the same way
    // one arriving before clangd accepts traffic waits in deferred_ (real-project plan RP1.1: a
    // transient reason is never a reason to answer as though clangd had already given up).
    std::map<std::string, std::vector<Waiting>, std::less<>> heldRequests_;
    std::set<std::string> diagnosed_;                // client URIs clangd published diagnostics for
    std::set<std::string> awaitingDiagnostics_;      // client URIs
    std::deque<Clock::time_point> crashes_;
    std::vector<Issue> issues_;
    std::optional<Clock::time_point> restartAt_;
    std::string restartReason_;
    // robustness design C4, C6: restarts spaced out; files clangd stopped answering for set aside one by one.
    RestartGate restartGate_;
    Quarantine quarantine_;                                   // path keys
    std::optional<Clock::time_point> lastAnswerAt_;           // clangd's last answer to any client request
    StuckWatch stuck_;
    SpinWatch spin_;   // import-hang plan §4: a file clangd will not finish, busy or not
    // WA-CLANGD-001: the `;` insertions in the text clangd has of each open document (client URI), for
    // the documents whose text it was given rewritten; mapped back out of what it reports.
    std::map<std::string, std::vector<Insertion>, std::less<>> rewritten_;
    // The request a watch was last tried for (when it was sent): one try each, so a platform that
    // cannot read clangd's CPU does not wake the loop again and again for the same request.
    std::optional<Clock::time_point> stuckTriedFor_;
    bool stuckAtCap_ { false };
    bool cpuReadInFlight_ { false };   // a reading of clangd's CPU is on its way back (read_cpu_)
    std::jthread cpuReading_;          // the thread taking it; joined when the engine goes, at most the ps(1) bound later   // a stuck clangd was found at the restart cap, and that was said
    // robustness design O1, O3: for a report of a problem.
    std::deque<std::pair<std::string, std::string>> restartHistory_;   // (UTC time, reason), the latest 20
    std::size_t linesLeftOut_ { 0 };                                   // clangd log lines the limiter left out
    ProcessConfig lastConfig_;
    std::map<std::string, Clock::time_point, std::less<>> touchedAt_;   // path key -> when the document was last opened or changed
    std::optional<Clock::time_point> lastSourceChangeAt_;   // the latest edit, save or watched change of any source
    std::map<std::string, std::string, std::less<>> deferredReclaims_;   // path key -> path: aside, a restart put off by a recent edit
    // robustness design C6: files that wait for diagnostics clangd never publishes. A unit of a module that did not
    // compile gets FAILED_MODULE_PATIENCE (clangd was seen to stop building such a unit for good); any file gets
    // GENERAL_PATIENCE while module preparation makes no progress.
    static constexpr std::chrono::seconds FAILED_MODULE_PATIENCE { 5 };
    static constexpr std::chrono::seconds GENERAL_PATIENCE { 120 };
    static constexpr std::chrono::seconds SELF_EDIT_GRACE { 10 };
    std::map<std::string, Clock::time_point, std::less<>> awaitingSince_;     // client URI -> when it was handed to clangd
    std::map<std::string, Clock::time_point, std::less<>> modulesFailedAt_;   // module -> when clangd said it did not compile
    std::optional<Clock::time_point> stuckCheckAt_;
    // robustness design C2: files of the workspace clangd is not given yet. A C++ source the editor opens that the engine
    // database does not have joins it with the next plan, and clangd 23.1 reads compile_commands.json again at most every
    // five seconds: a file it is given before then gets a guessed command and imports without stand-ins, and one importing a
    // module nothing provides kept a core busy for as long as clangd ran (xlings' apps/gui/main.cpp). Such a file waits,
    // answered by mcppls's own engine, until clangd has read a database that has it.
    static constexpr std::chrono::seconds DATABASE_REREAD { 6 };
    static constexpr std::chrono::seconds PLAN_PATIENCE { 15 };
    struct HeldFile {
        Clock::time_point since;
        bool planned { false };   // a plan was applied after the file was held
    };
    std::map<std::string, HeldFile, std::less<>> held_;                // path key
    std::map<std::string, Clock::time_point, std::less<>> joinedAt_;   // path key -> when a database this clangd had read gained it
    bool databaseRead_ { false };                                      // this clangd was given a file, and so read the database
    // robustness design C6: why each file was set aside. It goes back to clangd when its time is up, when the plan gives it
    // another command, when its imports change, or, for a unit of a module that did not compile, when a source is saved.
    struct Aside {
        std::string structure;      // what it provided and imported then
        bool moduleFailed { false };
        // The text clangd spun on (SpinWatch): that exact text never goes back to clangd, and any other does at once.
        std::optional<std::size_t> spunOn;
    };
    std::map<std::string, Aside, std::less<>> aside_;                  // path key
    std::map<std::string, std::string, std::less<>> fileStatus_;       // client URI -> clangd's last textDocument/clangd.fileStatus state
    // robustness design C10: a definition that clangd finds only as a declaration in a module's interface is looked for in that
    // module's other units, which are opened in clangd without the editor (background units) and closed again when unused.
    static constexpr std::chrono::seconds DEFINITION_PATIENCE { 8 };
    static constexpr std::chrono::minutes BACKGROUND_IDLE { 10 };
    static constexpr std::chrono::minutes BACKGROUND_BUILD_LIMIT { 2 };
    static constexpr std::size_t BACKGROUND_UNITS { 12 };
    static constexpr std::size_t UNITS_PER_SEARCH { 4 };
    std::map<std::string, std::vector<UnitOfModule>, std::less<>> moduleUnits_;   // module -> its units other than its interface
    std::map<std::string, std::string, std::less<>> interfaceModules_;          // path key of an importable unit -> its module
    struct BackgroundUnit {
        std::string path;
        std::string uri;
        Clock::time_point openedAt;
        Clock::time_point usedAt;
        bool built { false };
        std::string state;   // clangd's last fileStatus state for it
    };
    std::map<std::string, BackgroundUnit, std::less<>> background_;           // path key
    std::set<std::string, std::less<>> closedBackground_;                     // path keys whose closing diagnostics are still to come
    std::map<std::string, std::optional<platform::fs::FileStamp>, std::less<>> backgroundRefused_;   // units that did not build in time, as they were
    struct DefinitionSearch {
        Json message;          // the client's request
        Json firstAnswer;      // clangd's answer before the units were built
        Reply reply;
        std::set<std::string> waitingFor;   // path keys
        Clock::time_point deadline;
        Clock::time_point limit;            // the client's request's own limit (wait_limit), which the search stays within
    };
    std::vector<DefinitionSearch> searches_;

public:
    explicit ClangdEngine(Options options)
        : options_ { std::move(options) }, traits_ { traits_for_version(options_.version, options_.disabledWorkarounds) }, stuck_ { options_.stuckWatch } {}

    std::string_view id() const override { return ENGINE_ID; }
    std::span<const MethodCapability> methods() const override { return methods_; }
    EngineTraits traits() const override { return traits_; }

    EngineStatus status() const override {
        EngineStatus status;
        status.name = std::string { ENGINE_ID };
        status.version = options_.version.empty() ? std::string { "unknown" } : options_.version;
        status.role = "core";
        status.accepting = accepting_;
        const bool preparingBusy { primer_.busy() };
        // The status settles instead of saying "preparing" forever (real-project plan RP1.4, design P7): once
        // preparation has made no progress for a minute, it counts as not preparing any more, whatever
        // it is still nominally waiting on. A known cause (doomed modules) explains it in issues_
        // already; an unexplained stall gets a generic one below so degraded always says why.
        const bool stalled { preparingBusy && lastPrimeProgressAt_ && Clock::now() - *lastPrimeProgressAt_ >= PREPARATION_STALL_TIMEOUT };
        status.preparing = (!awaitingDiagnostics_.empty() || (preparingBusy && !stalled) || !held_.empty()) && accepting_;
        status.failed = options_.payloadCorrupt || incompatible_ || (unavailable_ && crashes_.size() >= 3);
        if (preparingBusy) std::tie(status.prepared, status.toPrepare) = primer_.progress();
        status.issues = issues_;
        if (stalled && doomedModules_.empty()) {
            status.issues.push_back(Issue { "preparation-stalled",
                std::format("module preparation made no progress for a minute ({}/{} done)", status.prepared, status.toPrepare), "mcppls.showLogs" });
        }
        status.state = unavailable_ ? "unavailable" : status.preparing ? "preparing" : accepting_ ? "ready" : "starting";
        // overall design 5.6: a clangd outside the traits table runs with every compensation on, and says so.
        if (!unavailable_ && !traits_.tested && !options_.version.empty()) {
            status.notices.push_back(Issue { "engine-version-untested",
                std::format("clangd {} has not been run through this server's conformance suite; every workaround for clangd 23.1 stays on", options_.version), "" });
        }
        return status;
    }

    // clangd has the file and is building what it needs for it: its own file status says it is
    // working, or it has not published diagnostics for it yet. A file it will never answer for
    // (doomed, set aside, held, excluded) is not "busy": something else already explains that.
    bool busy_with(std::string_view path) const override {
        if (path.empty() || !accepting_) return false;
        const std::string key { base::path_key(path) };
        if (doomedFiles_.contains(key) || quarantine_.contains(key) || excluded_.contains(key) || held_.contains(key)) return false;
        for (const auto& document : host_->documents()) {
            if (document.path.empty() || base::path_key(document.path) != key) continue;
            if (awaitingDiagnostics_.contains(document.uri)) return true;
            const auto status = fileStatus_.find(document.uri);
            return status != fileStatus_.end() && engine_working(status->second);
        }
        return false;
    }

    Json report() const override {
        Json restarts = Json::array();
        for (const auto& [at, reason] : restartHistory_) restarts.push_back(Json { { "at", at }, { "reason", reason } });
        Json unresolved = Json::object();
        for (const auto& [name, module] : unresolvedModules_) unresolved[name] = Json { { "reason", module.reason }, { "provider", module.provider } };
        Json compileFailures = Json::array();
        for (const auto& name : reportedFailures_) compileFailures.push_back(name);
        Json doomed = Json::object();
        for (const auto& [name, root] : doomRoots_) doomed[name] = Json { { "reason", root.reason }, { "provider", root.provider } };
        Json doomedFiles = Json::array();
        for (const auto& [key, info] : doomedFiles_) doomedFiles.push_back(Json { { "file", key }, { "rootModule", info.rootModule }, { "viaModule", info.viaModule } });
        const auto [done, wanted] = primer_.progress();
        return Json {
            { "executable", options_.executable },
            { "arguments", clangd_arguments(lastConfig_) },
            { "generation", generation_ },
            { "handshakeDone", handshakeDone_ },
            { "accepting", accepting_ },
            { "unavailable", unavailable_ },
            { "recentExits", crashes_.size() },
            { "restarts", std::move(restarts) },
            { "restartScheduled", restartAt_.has_value() },
            { "filesSetAside", quarantine_.members() },
            { "filesWaitingForDatabase", held_files_() },
            { "fileStates", fileStatus_ },
            { "backgroundUnits", background_files_() },
            { "definitionSearches", searches_.size() },
            { "unresolvedModules", std::move(unresolved) },
            { "modulesThatDidNotCompile", std::move(compileFailures) },
            { "doomedModules", std::move(doomed) },
            { "filesRoutedToOwnEngine", std::move(doomedFiles) },
            { "stdFromSemanticKit", stdFromKit_ },
            { "preparation", Json { { "done", done }, { "wanted", wanted }, { "running", primer_.running() },
                                    { "limit", preparation_limit(std::thread::hardware_concurrency(), mcppls::os::FAMILY == mcppls::os::Family::macos,
                                                                 awaitingDiagnostics_.size()) } } },
            { "pendingRequests", pending_.size() },
            { "deferredRequests", deferred_.size() },
            { "heldRequests", std::ranges::fold_left(heldRequests_ | std::views::values | std::views::transform(&std::vector<Waiting>::size), std::size_t { 0 }, std::plus {}) },
            { "filesAwaitingDiagnostics", awaitingDiagnostics_.size() },
            { "logLinesLeftOut", linesLeftOut_ },
            { "databaseDirectory", databaseDirectory_ },
            { "workarounds", workarounds_json_() },
        };
    }

    // import-hang plan §9: the registered workarounds this clangd needs, as the report shows them.
    Json workarounds_json_() const {
        Json list = Json::array();
        for (const auto& workaround : workarounds()) {
            if (!needs(workaround, options_.version)) continue;
            const bool off { std::ranges::find(options_.disabledWorkarounds, workaround.id) != options_.disabledWorkarounds.end() };
            list.push_back(Json { { "id", workaround.id }, { "title", workaround.title }, { "upstream", workaround.upstream },
                                  { "removeWhen", workaround.removeWhen }, { "turnedOff", off } });
        }
        return list;
    }

    void start(Host& host) override {
        host_ = &host;
        sink_ = host.event_sink(ENGINE_ID);
        const std::string& cache { host.cache_directory() };
        databaseDirectory_ = base::join_path(cache, "contexts/default/cdb");
        primeDirectory_ = base::join_path(cache, "contexts/default/prime");
        moduleHintDirectory_ = base::join_path(cache, "contexts/default/module-hints");   // never created
        stubDirectory_ = base::join_path(cache, "contexts/default/stubs");
        (void)platform::fs::create_directories(databaseDirectory_);
        // clangd starts without a database; the first plan is written before any document reaches it.
        platform::fs::remove_all(base::join_path(databaseDirectory_, "compile_commands.json"));
        log::info("clangd {} at {}", options_.version.empty() ? "?" : options_.version, options_.executable.empty() ? "(none)" : options_.executable);
        // import-hang plan §9: which of clangd's known defects this server works around for this version.
        if (!options_.executable.empty()) {
            const auto active = active_workarounds(options_.version);
            std::string ids;
            for (const auto id : active) {
                const bool off { std::ranges::find(options_.disabledWorkarounds, id) != options_.disabledWorkarounds.end() };
                ids += std::format("{}{}{}", ids.empty() ? "" : ", ", id, off ? " (turned off)" : "");
            }
            for (const auto& id : options_.disabledWorkarounds) {
                if (find_workaround(id) == nullptr) log::warning("--disable-workaround {}: no such workaround", id);
            }
            log::info("workarounds for clangd {}: {}", options_.version.empty() ? "?" : options_.version, ids.empty() ? "none" : ids);
        }
        start_process_();
    }

    void shut_down() override {
        if (process_ && process_->running() && handshakeDone_) (void)send_(lsp::make_notification("exit", nullptr));
        if (process_) process_->stop(std::chrono::seconds { 2 });
    }

    void configure_plan(normalize::PlanInput& input) const override {
        input.engineDriverDirectory = options_.executable.empty() ? std::string {} : base::parent_path(options_.executable);
        for (const auto& [name, unresolved] : unresolvedModules_) {
            // The kit brings its own standard library: what clangd could not find of the toolchain's does not apply.
            if (stdFromKit_ && input.kit != nullptr && (name == "std" || name == "std.compat")) continue;
            input.unresolvedModules.emplace(name, unresolved.reason);
        }
        input.preferKit = stdFromKit_;
        input.primeDirectory = traits_.needsModulePreparation ? primeDirectory_ : std::string {};
        input.moduleHintDirectory = traits_.needsModuleHints ? moduleHintDirectory_ : std::string {};
        input.stubDirectory = traits_.hangsOnUnresolvedImports ? stubDirectory_ : std::string {};
        input.excludeUnresolvedImports = traits_.hangsOnUnresolvedImports;
        input.noAlignedAllocationWithMsvcStl = traits_.msvcStlNeedsNoAlignedAllocation;
    }

    void apply(const normalize::EnginePlan* plan) override {
        if (plan == nullptr) {
            // The project model did not load in time: serve without a database.
            if (!planApplied_) {
                planApplied_ = true;
                accept_traffic_if_ready_();
            }
            return;
        }
        write_prime_sources_(*plan);
        const std::string database { normalize::to_compile_commands(*plan).dump(1) };
        const std::string structure { normalize::to_compile_commands(*plan, false).dump(1) };
        const bool changed { database != writtenDatabase_ };
        // Hints alone change as imports do; clangd rereads the database within five seconds.
        const bool structureChanged { structure != writtenStructure_ };
        writtenStructure_ = structure;
        if (changed) {
            // clangd rereads --compile-commands-dir/compile_commands.json itself (v1 design 15.1).
            if (auto written = platform::fs::write_file_atomic(base::join_path(databaseDirectory_, "compile_commands.json"), database); !written) {
                log::error("cannot write the engine database ({}): {}", host_->root_directory(), written.error().message);
            }
            writtenDatabase_ = database;
            log::info("engine database ({}): {} entries ({} standard library units, {} stand-ins), {} left out, {} issues", host_->root_directory(),
                      plan->entries.size(), plan->stdUnits, plan->stubModules.size(), plan->excludedFiles.size(), plan->issues.size());
            host_->record_event("engine-database", Json { { "entries", plan->entries.size() }, { "standIns", plan->stubModules.size() },
                                                          { "leftOut", plan->excludedFiles.size() } });
        }
        // Each stand-in named at warning, with why it got one, not just counted (robustness design O3,
        // real-project plan RP3.3): a module nothing usable provides is worth a person's attention once,
        // not every time the database is rewritten for something else.
        {
            std::set<std::string, std::less<>> currentStubs { plan->stubModules.begin(), plan->stubModules.end() };
            for (const auto& name : currentStubs) {
                if (loggedStandIns_.contains(name)) continue;
                std::string reason { "no usable provider" };
                for (const auto& issue : plan->issues) {
                    if (issue.module == name) {
                        reason = issue.message;
                        break;
                    }
                }
                log::warning("module {} has a stand-in ({}): {}", name, host_->root_directory(), reason);
            }
            loggedStandIns_ = std::move(currentStubs);
        }
        std::set<std::string> newExcluded;
        for (const auto& file : plan->excludedFiles) newExcluded.insert(base::path_key(file));
        std::map<std::string, std::string, std::less<>> newModuleSources;
        std::map<std::string, std::string, std::less<>> newModuleCommands;
        for (const auto& entry : plan->entries) {
            if (entry.provides.empty()) continue;
            newModuleSources.emplace(entry.provides, entry.file);
            newModuleCommands.emplace(entry.provides, lsp::dump(Json(entry.arguments)));
        }
        // robustness design C4: clangd keeps where it found a module after the database drops that unit, and
        // building the dropped unit can deadlock it (experiment S12). Only a provider leaving, or moving, needs a
        // fresh clangd; new units, units coming back and every other change are read from the database as it is.
        std::set<std::string, std::less<>> imported;
        for (const auto& entry : plan->entries) imported.insert(entry.imports.begin(), entry.imports.end());
        bool providerLeft { false };
        for (const auto& [name, source] : moduleSources_) {
            const auto now = newModuleSources.find(name);
            const bool moved { now == newModuleSources.end() || !base::same_path(now->second, source) };
            // A module nothing imports any more cannot be built by mistake.
            if (moved && imported.contains(name)) providerLeft = true;
        }
        // A unit of the project compiled with other arguments (another context, changed build flags): clangd
        // does not rebuild a document it has open for a changed database, so a fresh clangd applies them.
        std::map<std::string, std::string, std::less<>> newArguments;
        for (const auto& entry : plan->entries) newArguments.emplace(base::path_key(entry.file), lsp::dump(Json(entry.arguments)));
        bool argumentsChanged { false };
        for (const auto& [file, arguments] : writtenArguments_) {
            if ((!stubDirectory_.empty() && base::is_within(file, base::path_key(stubDirectory_)))
                || (!primeDirectory_.empty() && base::is_within(file, base::path_key(primeDirectory_)))) continue;
            if (const auto now = newArguments.find(file); now != newArguments.end() && now->second != arguments) argumentsChanged = true;
        }
        const auto appliedAt = Clock::now();
        std::vector<std::string> commandChanged;   // path keys whose command the database gained or changed
        for (const auto& [file, arguments] : newArguments) {
            const auto before = writtenArguments_.find(file);
            if (before == writtenArguments_.end() && databaseRead_) joinedAt_[file] = appliedAt;
            if (before == writtenArguments_.end() || before->second != arguments) commandChanged.push_back(file);
        }
        std::erase_if(joinedAt_, [&](const auto& item) { return !newArguments.contains(item.first); });
        writtenArguments_ = std::move(newArguments);
        for (auto& [key, held] : held_) held.planned = true;
        (void)structureChanged;
        const bool restartNeeded { (providerLeft || argumentsChanged) && planApplied_ && handshakeDone_ };
        if (!restartNeeded && accepting_) {
            for (const auto& document : host_->documents()) {
                if (document.path.empty()) continue;
                const std::string pathKey { base::path_key(document.path) };
                const bool wasExcluded { excluded_.contains(pathKey) };
                const bool isExcluded { newExcluded.contains(pathKey) };
                if (wasExcluded && !isExcluded) open_or_hold_(document, true);
                if (!wasExcluded && isExcluded) {
                    (void)send_(lsp::make_notification("textDocument/didClose", Json { { "textDocument", Json { { "uri", document.uri } } } }));
                }
            }
        }
        excluded_ = std::move(newExcluded);
        // A file set aside that the database now gives another command goes back to clangd with it.
        for (const auto& file : commandChanged) {
            if (!quarantine_.release(file)) continue;
            aside_.erase(file);
            log::info("handing {} back to clangd ({}): the engine database has another command for it", file, host_->root_directory());
            host_->record_event("file-handed-back", Json { { "file", file }, { "why", "command changed" } });
            if (restartNeeded || !accepting_) continue;
            for (const auto& document : host_->documents()) {
                if (!document.path.empty() && base::path_key(document.path) == file && !excluded_path_(document.path)) open_or_hold_(document, true);
            }
        }
        update_quarantine_issue_();
        {
            std::vector<PrimeModule> modules;
            for (const auto& module : plan->modules) modules.push_back(PrimeModule { module.name, module.requires_, module.primeFile });
            // An edit that leaves the module graph as it was leaves its preparation running, units and all.
            if (restartNeeded || !primer_.same_modules(modules)) {
                close_prime_units_();
                primer_.set_modules(std::move(modules));
            }
        }
        moduleSources_ = std::move(newModuleSources);
        moduleCommands_ = std::move(newModuleCommands);
        moduleUnits_.clear();
        interfaceModules_.clear();
        for (const auto& entry : plan->entries) {
            if (entry.module.empty()) continue;
            if (!entry.provides.empty()) interfaceModules_.emplace(base::path_key(entry.file), entry.module);
            if (entry.provides != entry.module) moduleUnits_[entry.module].push_back(UnitOfModule { entry.file, !entry.provides.empty() });
        }
        // The module import graph and each file's direct imports, for closure-scoped failure
        // containment (real-project plan RP1.1, design P1): who is doomed with a module that fails to compile.
        moduleRequires_.clear();
        for (const auto& module : plan->modules) moduleRequires_.emplace(module.name, module.requires_);
        fileImports_.clear();
        fileModule_.clear();
        for (const auto& entry : plan->entries) {
            const std::string key { base::path_key(entry.file) };
            if (!entry.imports.empty()) fileImports_[key] = entry.imports;
            if (!entry.module.empty()) fileModule_[key] = entry.module;
        }
        std::vector<std::string> backgroundLeaving;
        for (const auto& [key, unit] : background_) {
            if (!writtenArguments_.contains(key) || excluded_.contains(key) || restartNeeded) backgroundLeaving.push_back(key);
        }
        for (const auto& key : backgroundLeaving) close_background_(key);
        startupBmis_.reset();
        planApplied_ = true;
        const bool unresolvedForgot { forget_changed_unresolved_() };
        const bool doomForgot { forget_changed_doom_() };
        recompute_doom_();
        if (restartNeeded) {
            request_restart_(providerLeft ? "a module's unit left the engine database" : "units are compiled with other arguments");
        } else {
            accept_traffic_if_ready_();
            open_held_files_(appliedAt);
            prepare_modules_();
        }
        if (unresolvedForgot || doomForgot) host_->request_replan();
    }

    void document(const DocumentEvent& event) override {
        const DocumentView& document { event.document };
        switch (event.change) {
        case DocumentChange::opened:
            touch_(document.path);
            // clangd has it open without the editor: it is opened again as the editor's, so its diagnostics come again.
            if (!document.path.empty()) close_background_(base::path_key(document.path));
            if (const std::string key { document.path.empty() ? std::string {} : base::path_key(document.path) }; !key.empty()) {
                if (const auto doom = doomedFiles_.find(key); doom != doomedFiles_.end()) {
                    // Never sent to clangd: it does not know it, so there is nothing to close there.
                    publish_doom_diagnostic_(document, doom->second);
                    break;
                }
            }
            if (excluded_path_(document.path) || quarantined_(document.path)) break;
            if (accepting_) {
                open_or_hold_(document, false);
                prepare_imports_of_(document);
            } else if (!document.path.empty() && !writtenDatabase_.empty() && !writtenArguments_.contains(base::path_key(document.path))
                       && project::is_cxx_source_name(document.path) && base::is_within(document.path, host_->root_directory())) {
                // Opened while clangd restarts: the database it reads may not have the file yet. Before the first plan nothing is
                // held; that plan has every open file, and its requests wait for clangd.
                held_.try_emplace(base::path_key(document.path), HeldFile { Clock::now(), false });
            }
            break;
        case DocumentChange::changed:
            touch_(document.path);
            // clangd is given the whole text when it is given the file.
            if (held_path_(document.path) || doomed_path_(document.path)) break;
            if (quarantined_(document.path)) {
                const std::string key { base::path_key(document.path) };
                // What clangd stopped on is still there: the file stays aside until its time is up or what it imports changes.
                // A file clangd spun on goes back as soon as its text is any other than the one it spun on.
                if (const auto aside = aside_.find(key); aside != aside_.end()) {
                    if (aside->second.spunOn ? *aside->second.spunOn == text_hash_(document.text)
                                             : aside->second.structure == structure_of_text_(document.text)) break;
                }
                quarantine_.release(key);
                aside_.erase(key);
                update_quarantine_issue_();
                if (accepting_ && !excluded_path_(document.path)) open_or_hold_(document, true);
                break;
            }
            if (accepting_ && !excluded_path_(document.path) && event.message != nullptr) send_change_(document, *event.message);
            break;
        case DocumentChange::closed: {
            const bool wasExcluded { excluded_path_(document.path) || quarantined_(document.path) || held_path_(document.path)
                                     || doomed_path_(document.path) };
            if (!document.path.empty()) {
                const std::string key { base::path_key(document.path) };
                held_.erase(key);
                release_held_requests_(key, false);
            }
            awaitingDiagnostics_.erase(document.uri);
            awaitingSince_.erase(document.uri);
            diagnosed_.erase(document.uri);
            fileStatus_.erase(document.uri);
            rewritten_.erase(document.uri);
            spin_.forget(document.uri);
            if (accepting_ && !wasExcluded && event.message != nullptr) (void)send_(*event.message);
            release_prime_units_if_idle_();
            break;
        }
        case DocumentChange::saved:
            if (!document.path.empty() && !excluded_path_(document.path) && !quarantined_(document.path) && !held_path_(document.path)
                && !doomed_path_(document.path) && accepting_ && event.message != nullptr) {
                (void)send_(*event.message);
            }
            sources_changed();
            break;
        }
    }

    void notify(const Json& message) override {
        if (accepting_) {
            (void)send_(message);
        } else if (!unavailable_ && message.value("method", std::string {}) != lsp::method::WORKSPACE_DID_CHANGE_WATCHED_FILES) {
            deferred_.push_back(Waiting { message, Reply {} });
        }
    }

    void sources_changed() override {
        lastSourceChangeAt_ = Clock::now();
        modulesFailedAt_.clear();   // a module that still does not compile is reported again
        // A unit set aside because its module did not compile goes back to clangd: the module may compile now.
        std::vector<std::string> released;
        for (auto it = aside_.begin(); it != aside_.end();) {
            if (it->second.moduleFailed && quarantine_.release(it->first)) {
                released.push_back(it->first);
                it = aside_.erase(it);
            } else {
                ++it;
            }
        }
        if (!released.empty()) {
            update_quarantine_issue_();
            for (const auto& document : host_->documents()) {
                if (document.path.empty() || std::ranges::find(released, base::path_key(document.path)) == released.end()) continue;
                if (accepting_ && !excluded_path_(document.path)) open_or_hold_(document, true);
            }
        }
        // A module that failed to compile is tried again only when its own unit or command changes
        // (real-project plan RP1.1, design P1): a save elsewhere in the project is not, by itself, a reason to
        // hand a doomed file back to clangd only to fail the same way again.
        if (forget_changed_doom_()) recompute_doom_();
        if (forget_changed_unresolved_()) host_->request_replan();
    }

    bool claims(const RequestView& request) const override {
        // held_path_ is not checked here, the same as !accepting_ is not: both are transient, and
        // request() below waits them out rather than refusing on this engine's behalf, so a method
        // with no other answerer still gets clangd's answer once the file is given to it.
        return !unavailable_ && !excluded_path_(request.path) && !quarantined_(request.path) && !doomed_path_(request.path);
    }

    void request(const RequestView& view, const Json& message, Reply reply) override {
        if (unavailable_) {
            reply(Answer {});
            return;
        }
        const auto limit = wait_limit(message.value("method", std::string {}), options_.requestTimeout, Clock::now());
        if (!accepting_) {
            deferred_.push_back(Waiting { message, std::move(reply), limit });
            return;
        }
        if (held_path_(view.path)) {
            heldRequests_[base::path_key(view.path)].push_back(Waiting { message, std::move(reply), limit });
            return;
        }
        request_now_(message, std::move(reply), limit);
    }

    // Requests still waiting for clangd (to start, or to be given their file) at their limit are
    // answered unavailable, so the next engine answers them: whatever holds clangd up, a person's
    // request is never left unanswered past INTERACTIVE_LIMIT (wait_limit).
    void answer_overdue_waiting_(Clock::time_point now) {
        std::vector<std::pair<Waiting, std::string_view>> overdue;   // the request, and what it waited for
        const auto take = [&](std::vector<Waiting>& waiting, std::string_view waitedFor) {
            for (auto it = waiting.begin(); it != waiting.end();) {
                if (it->reply && it->limit <= now) {
                    overdue.emplace_back(std::move(*it), waitedFor);
                    it = waiting.erase(it);
                } else {
                    ++it;
                }
            }
        };
        take(deferred_, "clangd to accept requests");
        for (auto& [key, requests] : heldRequests_) take(requests, "its file to be given to clangd");
        std::erase_if(heldRequests_, [](const auto& item) { return item.second.empty(); });
        for (auto& [waiting, waitedFor] : overdue) {
            const std::string method { waiting.message.value("method", std::string {}) };
            const Json* uri { lsp::find_path(waiting.message, { "params", "textDocument", "uri" }) };
            const std::string file { uri != nullptr && uri->is_string() ? host_->path_of_uri(uri->get<std::string>()) : std::string {} };
            log::warning("{} waited too long for {} ({}); the next engine answers it", method, waitedFor, host_->root_directory());
            host_->record_event("request-timeout", Json { { "method", method }, { "file", file }, { "waitedFor", std::string { waitedFor } } });
            waiting.reply(Answer {});
        }
    }

    // A held path is no longer held: its own pending requests (held there by request(), above)
    // either go to clangd now (it is being given the file) or, when it never will be (excluded,
    // quarantined, doomed), are answered unavailable so routing tries the next engine, exactly as
    // if the hold had never delayed them.
    void release_held_requests_(const std::string& key, bool toClangd) {
        const auto pending = heldRequests_.find(key);
        if (pending == heldRequests_.end()) return;
        auto toFlush = std::move(pending->second);
        heldRequests_.erase(pending);
        for (auto& [message, reply, limit] : toFlush) {
            if (toClangd) request_now_(message, std::move(reply), limit);
            else if (reply) reply(Answer {});
        }
    }

    void cancel(const Json& clientRequestId) override {
        // Waiting here, not yet sent to clangd: answered cancelled at once.
        const auto take = [&](std::vector<Waiting>& waiting) {
            for (auto it = waiting.begin(); it != waiting.end(); ++it) {
                if (lsp::kind_of(it->message) == lsp::Kind::request && it->message["id"] == clientRequestId) {
                    Reply reply { std::move(it->reply) };
                    waiting.erase(it);
                    if (reply) reply(Answer { Answer::Kind::cancelled, nullptr });
                    return true;
                }
            }
            return false;
        };
        if (take(deferred_)) return;
        for (auto& [key, requests] : heldRequests_) {
            if (take(requests)) return;
        }
        for (const auto& [engineId, request] : pending_) {
            if (request.purpose == Purpose::client && request.clientId == clientRequestId) {
                (void)send_(lsp::make_notification("$/cancelRequest", Json { { "id", engineId } }));
                return;
            }
        }
    }

    void client_response(int generation, const Json& engineRequestId, const Json& response) override {
        if (generation != generation_) return;
        Json forwarded = response;
        forwarded["id"] = engineRequestId;
        (void)send_(forwarded);
    }

    void handle_event(const Json& event) override {
        const std::string kind { event.value("kind", std::string {}) };
        // Whichever process it was read from, the reading is back and another may start.
        if (kind == "cpu") cpuReadInFlight_ = false;
        if (event.value("generation", -1) != generation_) return;
        if (kind == "message") {
            handle_message_(event["message"]);
        } else if (kind == "closed") {
            handle_closed_();
        } else if (kind == "load-failure") {
            handle_load_failure_(event.value("line", std::string {}));
        } else if (kind == "module-failed") {
            handle_module_failure_(event["failure"]);
        } else if (kind == "cpu") {
            const Json& seconds = event["seconds"];
            cpu_read_(event.value("endOfWatch", false), seconds.is_number() ? std::optional<double> { seconds.get<double>() } : std::nullopt);
        } else if (kind == "log-left-out") {
            linesLeftOut_ += event.value("count", std::size_t { 0 });
            host_->record_event("engine-log-left-out", Json { { "count", event.value("count", std::size_t { 0 }) } });
        }
    }

    std::optional<Clock::time_point> next_deadline() const override {
        std::optional<Clock::time_point> deadline;
        auto consider = [&](const std::optional<Clock::time_point>& at) {
            if (at && (!deadline || *at < *deadline)) deadline = at;
        };
        for (const auto& [id, request] : pending_) consider(request.deadline);
        for (const auto& [module, at] : primeDeadlines_) consider(at);
        consider(restartAt_);
        consider(stuckCheckAt_);
        if (accepting_) consider(spin_.next_due());   // only acted on while accepting (handle_spins_)
        // While a reading of clangd's CPU is on its way, its event is what wakes the loop.
        if (!cpuReadInFlight_) {
            consider(stuck_.due());
            if (const auto oldest = oldest_unanswered_(); oldest && !stuck_.watching() && stuckTriedFor_ != oldest) consider(*oldest + options_.stuckAfter);
        }
        if (!deferredReclaims_.empty() && lastSourceChangeAt_) consider(*lastSourceChangeAt_ + GENERAL_PATIENCE);
        // So the status settles into ready or degraded on its own, not only when something else wakes
        // the event loop (real-project plan RP1.4, design P7).
        if (primer_.busy() && lastPrimeProgressAt_) consider(*lastPrimeProgressAt_ + PREPARATION_STALL_TIMEOUT);
        for (const auto& [key, held] : held_) {
            if (const auto joined = joinedAt_.find(key); joined != joinedAt_.end()) consider(joined->second + DATABASE_REREAD);
            else if (!held.planned) consider(held.since + PLAN_PATIENCE);
        }
        for (const auto& search : searches_) consider(search.deadline);
        for (const auto& waiting : deferred_) {
            if (waiting.reply) consider(waiting.limit);
        }
        for (const auto& [key, requests] : heldRequests_) {
            for (const auto& waiting : requests) consider(waiting.limit);
        }
        for (const auto& [key, unit] : background_) consider(unit.built ? unit.usedAt + BACKGROUND_IDLE : unit.openedAt + BACKGROUND_BUILD_LIMIT);
        return deadline;
    }

    void handle_timers() override {
        const auto now = Clock::now();
        // Before the requests that expire now are answered: they are part of what clangd left unanswered.
        watch_for_stuck_(now);
        if (accepting_) handle_spins_(now);
        std::vector<std::int64_t> expired;
        for (const auto& [id, request] : pending_) {
            if (request.deadline <= now) expired.push_back(id);
        }
        bool restart { false };
        bool stalled { false };
        for (const auto id : expired) {
            PendingRequest request { std::move(pending_[id]) };
            pending_.erase(id);
            switch (request.purpose) {
            case Purpose::client: {
                const bool filePreparing { awaitingDiagnostics_.contains(host_->client_uri(request.uri)) && primer_.busy() };
                if (keep_waiting(request, filePreparing, lastPrimeProgressAt_, now)) {
                    request.deadline = std::min(now + PREPARING_GRACE, request.limit);
                    pending_[id] = std::move(request);
                    break;
                }
                log::warning("clangd ({}) did not answer {} in time", host_->root_directory(), request.method);
                host_->record_event("request-timeout", Json { { "method", request.method }, { "file", host_->path_of_uri(request.uri) },
                                                              { "seconds", std::chrono::duration_cast<std::chrono::seconds>(now - request.sent).count() } });
                if (request.reply) request.reply(Answer {});
                (void)send_(lsp::make_notification("$/cancelRequest", Json { { "id", id } }));
                // A file whose modules are still being built is slow, not stuck: setting it aside would throw that work away.
                if (awaitingDiagnostics_.contains(host_->client_uri(request.uri))) break;
                // Nor is one clangd had for less than its own timeout, because the request spent the rest of its limit
                // waiting before it was sent (wait_limit).
                if (now - request.sent < own_timeout_(request.method)) break;
                const std::string path { host_->path_of_uri(request.uri) };
                if (path.empty()) break;
                // Just after this file, or a source of a module it imports, changed, clangd is rebuilding what the
                // change touched, which either compiles (and the file answers again) or fails and is contained
                // with its importers (real-project plan RP1.1, RP1.2): a timeout meanwhile says nothing about this
                // file. An edit elsewhere is no excuse, and clangd answering nobody, which no edit explains,
                // still counts.
                switch (quarantine_.timed_out(base::path_key(path), request.sent, now, lastAnswerAt_, changed_recently_(path, now))) {
                case Quarantine::Verdict::wait: break;
                case Quarantine::Verdict::quarantined: set_aside_(path, "it stopped answering its requests", Reclaim::if_busy); break;
                case Quarantine::Verdict::stalled: stalled = true; break;
                }
                break;
            }
            case Purpose::engine_initialize:
                log::error("clangd ({}) did not answer initialize", host_->root_directory());
                add_issue_(Issue { "engine-timeout", "clangd did not answer initialize", "mcppls.restartServer" });
                host_->engine_settled(ENGINE_ID, Json::object());
                restart = true;
                break;
            }
        }
        if (stalled) {
            // clangd answered nobody: the engine is stuck. The file asked about first is the likeliest cause.
            host_->record_event("engine-stalled", Json { { "firstFile", quarantine_.first_stalled().value_or(std::string {}) } });
            if (auto first = quarantine_.first_stalled()) {
                for (const auto& document : host_->documents()) {
                    if (!document.path.empty() && base::path_key(document.path) == *first) set_aside_(document.path, "clangd stopped answering after it", Reclaim::no);
                }
            }
            add_issue_(Issue { "engine-timeout", "clangd stopped answering; it was restarted", "mcppls.restartServer" });
            request_restart_("clangd stopped answering");
        } else if (restart) {
            request_restart_("clangd did not answer initialize");
        }
        for (const auto& key : quarantine_.due(now)) {
            // The text clangd spun on is never given back to it, however long it has been aside.
            if (const auto aside = aside_.find(key); aside != aside_.end() && aside->second.spunOn) {
                const auto documents = host_->documents();
                const bool unchanged { std::ranges::any_of(documents, [&](const DocumentView& document) {
                    return !document.path.empty() && base::path_key(document.path) == key && text_hash_(document.text) == *aside->second.spunOn;
                }) };
                if (unchanged) {
                    quarantine_.put(key, now);
                    continue;
                }
            }
            aside_.erase(key);
            for (const auto& document : host_->documents()) {
                if (document.path.empty() || base::path_key(document.path) != key) continue;
                log::info("handing {} back to clangd ({})", document.path, host_->root_directory());
                host_->record_event("file-handed-back", Json { { "file", document.path }, { "why", "its time was up" } });
                if (accepting_ && !excluded_path_(document.path)) open_or_hold_(document, true);
            }
            update_quarantine_issue_();
        }
        std::erase_if(joinedAt_, [&](const auto& item) { return now >= item.second + DATABASE_REREAD && !held_.contains(item.first); });
        open_held_files_(now);
        answer_overdue_waiting_(now);
        handle_background_timers_(now);
        std::vector<std::string> overdue;
        for (const auto& [module, at] : primeDeadlines_) {
            if (at <= now) overdue.push_back(module);
        }
        for (const auto& module : overdue) {
            log::warning("stopped waiting for module {} to be prepared ({})", module, host_->root_directory());
            if (const auto* planned = primer_.find(module)) (void)finish_prime_(base::path_to_uri(planned->primeFile));
        }
        if (stuckCheckAt_ && *stuckCheckAt_ <= now) check_stuck_files_(now);
        reconsider_deferred_reclaims_(now);
        if (restartAt_ && *restartAt_ <= now) {
            restartAt_.reset();
            restart_(restartReason_.empty() ? std::string_view { "recovering from an exit" } : std::string_view { restartReason_ });
        }
        // The status settles once preparation has made no progress for a minute (real-project plan RP1.4,
        // design P7), so a client polling only when told to is told, even with nothing else happening.
        const bool stalledNow { primer_.busy() && lastPrimeProgressAt_ && now - *lastPrimeProgressAt_ >= PREPARATION_STALL_TIMEOUT };
        if (!expired.empty() || stalledNow) host_->status_changed();
    }

private:
    std::unique_ptr<Process> make_process_() const {
        return options_.processFactory ? options_.processFactory() : std::make_unique<ClangdProcess>();
    }

    bool excluded_path_(std::string_view path) const { return !path.empty() && excluded_.contains(base::path_key(path)); }

    void add_issue_(Issue issue) {
        for (const auto& existing : issues_) {
            if (existing.code == issue.code) return;
        }
        issues_.push_back(std::move(issue));
    }

    Json engine_view_(const Json& message) const {
        Json copy = message;
        const auto fix = [&](Json& uri) {
            if (uri.is_string()) uri = host_->engine_uri(uri.get<std::string>());
        };
        if (copy.contains("params") && copy["params"].is_object()) {
            Json& params = copy["params"];
            if (params.contains("textDocument") && params["textDocument"].is_object() && params["textDocument"].contains("uri")) {
                fix(params["textDocument"]["uri"]);
            }
            if (params.contains("changes") && params["changes"].is_array()) {
                for (auto& change : params["changes"]) {
                    if (change.is_object() && change.contains("uri")) fix(change["uri"]);
                }
            }
        }
        return copy;
    }

    bool send_(const Json& message) {
        if (!process_ || !process_->running()) return false;
        if (auto sent = process_->send(engine_view_(message)); !sent) {
            log::warning("cannot write to clangd ({}): {}", host_->root_directory(), sent.error().message);
            return false;
        }
        return true;
    }

    // clangd will not answer: every request waiting for it -- before it accepted traffic, or on a held
    // file -- is answered unavailable, so routing gives it to the next engine instead of leaving it.
    void flush_deferred_without_engine_() {
        std::vector<Waiting> toFlush;
        toFlush.swap(deferred_);
        for (auto& [key, requests] : heldRequests_) {
            for (auto& request : requests) toFlush.push_back(std::move(request));
        }
        heldRequests_.clear();
        for (auto& waiting : toFlush) {
            if (waiting.reply) waiting.reply(Answer {});
        }
    }

    void start_process_() {
        handshakeDone_ = false;
        loadFailure_.reset();
        accepting_ = false;
        stuck_.clear();
        stuckAtCap_ = false;
        // A new clangd reads the database when it is given its first file.
        databaseRead_ = false;
        joinedAt_.clear();
        fileStatus_.clear();
        background_.clear();
        closedBackground_.clear();
        if (options_.payloadCorrupt) {
            unavailable_ = true;
            add_issue_(Issue { "payload-corrupt", "the extension's payload is corrupt or was modified; reinstall the extension", "mcppls.showLogs", "environment" });
            flush_deferred_without_engine_();
            host_->engine_settled(ENGINE_ID, Json::object());
            host_->status_changed();
            return;
        }
        if (options_.executable.empty() || !platform::fs::is_regular_file(options_.executable)) {
            unavailable_ = true;
            add_issue_(Issue { "engine-missing", "clangd was not found; only module-level features are available", "mcppls.showLogs", "environment" });
            flush_deferred_without_engine_();
            host_->engine_settled(ENGINE_ID, Json::object());
            host_->status_changed();
            return;
        }
        const int generation { ++generation_ };
        ProcessConfig config;
        config.executable = options_.executable;
        config.version = options_.version;
        config.databaseDirectory = databaseDirectory_;
        config.workDirectory = host_->root_directory();
        config.verboseLog = options_.verboseLog;
        config.workers = engine_workers(std::thread::hardware_concurrency(), mcppls::os::FAMILY == mcppls::os::Family::macos);
        config.extraArguments = options_.extraArguments;
        // Extra engine arguments for troubleshooting, e.g. MCPPLS_ENGINE_ARGUMENTS="-j=8 --background-index-priority=background".
        if (auto extra = platform::env::get("MCPPLS_ENGINE_ARGUMENTS")) {
            for (auto word : base::split(*extra, ' ')) {
                if (!base::trim(word).empty()) config.extraArguments.emplace_back(base::trim(word));
            }
        }
        if (!process_) process_ = make_process_();
        lastConfig_ = config;
        host_->record_event("engine-start", Json { { "engine", std::string { ENGINE_ID } }, { "generation", generation }, { "arguments", clangd_arguments(config) } });
        auto sink = sink_;
        const std::string root { host_->root_directory() };
        // robustness design C7: clangd's errors are forwarded without flooding the log; failures are read from every line.
        // A verbose log is asked for to see everything, so it is not limited.
        struct LimitedLog {
            explicit LimitedLog(std::size_t burst) : limiter { burst, std::chrono::seconds { 10 } } {}
            std::mutex mutex;
            LineLimiter limiter;
        };
        auto limited = std::make_shared<LimitedLog>(options_.verboseLog ? std::numeric_limits<std::size_t>::max() : std::size_t { 40 });
        auto started = process_->start(
            config,
            [sink, generation](Json message) { sink(Json { { "kind", "message" }, { "generation", generation }, { "message", std::move(message) } }); },
            [sink, generation] { sink(Json { { "kind", "closed" }, { "generation", generation } }); },
            [sink, generation, root, limited](std::string_view line) {
                LineLimiter::Decision decision;
                {
                    const std::lock_guard lock { limited->mutex };
                    decision = limited->limiter.admit(GuardClock::now());
                }
                if (decision.suppressedBefore > 0) {
                    log::info("clangd ({}): {} more lines left out of this log", root, decision.suppressedBefore);
                    sink(Json { { "kind", "log-left-out" }, { "generation", generation }, { "count", decision.suppressedBefore } });
                }
                // Forwarded at clangd's own severity (robustness design C7, real-project plan RP3.3): a
                // problem worth someone's attention (E[..]) is not lost among the chatter (I[..]/V[..]/
                // D[..]), which stays at debug instead of crowding the default log at info.
                if (decision.forward) {
                    switch (clangd_log_level(line)) {
                    case log::Level::warning: log::warning("clangd ({}): {}", root, line); break;
                    case log::Level::debug: log::debug("clangd ({}): {}", root, line); break;
                    default: log::info("clangd ({}): {}", root, line); break;
                    }
                }
                if (loader_failure(line)) {
                    sink(Json { { "kind", "load-failure" }, { "generation", generation }, { "line", std::string { base::trim(line) } } });
                }
                if (auto failure = parse_module_failure(line)) {
                    sink(Json { { "kind", "module-failed" }, { "generation", generation },
                                { "failure", Json { { "module", failure->module }, { "reason", failure->reason }, { "source", failure->failedSource } } } });
                }
            });
        if (!started) {
            unavailable_ = true;
            add_issue_(Issue { "engine-crashed", std::format("clangd could not start: {}", started.error().message), "mcppls.restartServer" });
            flush_deferred_without_engine_();
            host_->engine_settled(ENGINE_ID, Json::object());
            host_->status_changed();
            return;
        }
        unavailable_ = false;
        Json params = host_->client_initialize_params();
        // What clangd is doing with each file (textDocument/clangd.fileStatus) tells a file it is still building from one it gave up on.
        params["initializationOptions"] = Json { { "clangdFileStatus", true } };
        params["processId"] = nullptr;
        // Positions are UTF-16 everywhere in this server.
        if (params.contains("capabilities") && params["capabilities"].is_object()) {
            params["capabilities"].erase("offsetEncoding");
            if (params["capabilities"].contains("general") && params["capabilities"]["general"].is_object()) {
                params["capabilities"]["general"].erase("positionEncodings");
            }
        }
        const std::int64_t engineId { nextId_++ };
        pending_[engineId] = PendingRequest { Purpose::engine_initialize, nullptr, "initialize", {}, Clock::now() + std::chrono::seconds { 60 }, generation, {}, {} };
        (void)send_(lsp::make_request(engineId, "initialize", std::move(params)));
    }

    void restart_(std::string_view reason) {
        if (incompatible_) return;   // it cannot run on this machine; another start changes nothing
        log::info("restarting clangd ({}): {}", host_->root_directory(), reason);
        restartGate_.record(Clock::now());
        restartHistory_.emplace_back(std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now())), std::string { reason });
        if (restartHistory_.size() > 20) restartHistory_.pop_front();
        host_->record_event("engine-restart", Json { { "reason", std::string { reason } } });
        restartAt_.reset();
        spin_.restarted();
        // Requests to the old process are answered by the other engines.
        auto old = std::move(pending_);
        pending_.clear();
        for (auto& [id, request] : old) {
            if (request.purpose == Purpose::client && request.reply) request.reply(Answer {});
        }
        answer_searches_();
        forget_primes_();
        ++generation_;   // late events of the old process are ignored
        if (process_) process_->stop(std::chrono::milliseconds { 500 });
        diagnosed_.clear();
        host_->forget_engine_diagnostics(ENGINE_ID);
        start_process_();
    }

    // When the oldest client request clangd still has was sent, if clangd has answered nothing since.
    std::optional<Clock::time_point> oldest_unanswered_() const {
        std::optional<Clock::time_point> oldest;
        for (const auto& [id, request] : pending_) {
            if (request.purpose == Purpose::client && request.generation == generation_ && (!oldest || request.sent < *oldest)) oldest = request.sent;
        }
        if (!oldest || (lastAnswerAt_ && *lastAnswerAt_ >= *oldest)) return std::nullopt;
        return oldest;
    }

    // A request unanswered for options_.stuckAfter, with nothing answered since it was sent, starts a
    // watch of clangd's CPU (StuckWatch). At its end, a clangd that has still answered nothing, still
    // owes an answer to a request older than the watch -- so it had work the whole time -- and used
    // next to no CPU is stuck, and is restarted within the restart cap. A request that timed out and
    // was cancelled meanwhile leaves clangd rightly idle, and proves nothing. The CPU is read off the
    // event loop (read_cpu_); what to make of each reading is decided when it comes back (cpu_read_).
    void watch_for_stuck_(Clock::time_point now) {
        if (!accepting_ || !process_) {
            stuck_.clear();
            return;
        }
        if (cpuReadInFlight_) return;
        if (!stuck_.watching()) {
            const auto oldest = oldest_unanswered_();
            if (oldest && now - *oldest >= options_.stuckAfter && stuckTriedFor_ != oldest) {
                stuckTriedFor_ = oldest;
                read_cpu_(false);
            }
            return;
        }
        if (const auto due = stuck_.due(); due && now >= *due) read_cpu_(true);
    }

    // Reads clangd's CPU on a thread of its own -- ps(1) on macOS can take a while, and the event loop
    // serves every root -- and hands the reading back as a "cpu" event. One reading at a time; none
    // where the platform cannot say.
    void read_cpu_(bool endOfWatch) {
        auto reader = process_ ? process_->cpu_reader() : std::function<std::optional<double>()> {};
        if (!reader) {
            stuck_.clear();
            return;
        }
        // The previous reading's thread has handed its event over already (cpuReadInFlight_ is cleared by that event).
        if (cpuReading_.joinable()) cpuReading_.join();
        cpuReadInFlight_ = true;
        cpuReading_ = std::jthread { [sink = sink_, generation = generation_, endOfWatch, reader = std::move(reader)] {
            const auto seconds = reader();
            sink(Json { { "kind", "cpu" }, { "generation", generation }, { "endOfWatch", endOfWatch },
                        { "seconds", seconds ? Json(*seconds) : Json(nullptr) } });
        } };
    }

    // A reading of clangd's CPU, back from read_cpu_: the start of a watch, or its end and verdict.
    void cpu_read_(bool endOfWatch, std::optional<double> seconds) {
        if (!accepting_) return;
        const auto now = Clock::now();
        const auto oldest = oldest_unanswered_();
        if (!endOfWatch) {
            // Answered while it was being read: nothing to watch.
            if (oldest && !stuck_.watching()) stuck_.suspect(now, seconds);
            return;
        }
        if (!stuck_.watching()) return;   // clangd answered something meanwhile
        if (!oldest || *oldest > *stuck_.started()) {
            stuck_.clear();
            return;
        }
        const auto verdict = stuck_.check(now, seconds);
        if (!verdict.stuck) {
            log::debug("clangd ({}) has left a request unanswered for {:.0f} s and is busy, not stuck: {:.2f} s of CPU in the last {:.0f} s",
                       host_->root_directory(), std::chrono::duration<double>(now - *oldest).count(), verdict.cpuSeconds, verdict.seconds);
            return;
        }
        host_->record_event("engine-stuck", Json { { "unansweredSeconds", std::chrono::duration<double>(now - *oldest).count() },
                                                   { "watchedSeconds", verdict.seconds }, { "cpuSeconds", verdict.cpuSeconds } });
        // At the restart cap, clangd stays as it is until the window frees up; that is said once
        // (restart_capped_), not again at the end of every watch meanwhile.
        if (restartGate_.at_cap(now)) {
            if (!stuckAtCap_) (void)restart_capped_("clangd stopped answering and used no CPU");
            stuckAtCap_ = true;
            return;
        }
        log::warning("clangd ({}) has left a request unanswered for {:.0f} s and used {:.2f} s of CPU in the last {:.0f} s: it is stuck, not busy; restarting it",
                     host_->root_directory(), std::chrono::duration<double>(now - *oldest).count(), verdict.cpuSeconds, verdict.seconds);
        add_issue_(Issue { "engine-timeout", "clangd stopped making progress; it was restarted", "mcppls.restartServer" });
        // Worded like the "answers nobody" verdict's reason: both are clangd stopping, told apart by
        // how it was seen (module-faults F8 allows exactly these).
        request_restart_("clangd stopped answering and used no CPU: it was stuck");
    }

    // How long clangd itself has to answer a client request, once it is sent.
    std::chrono::milliseconds own_timeout_(std::string_view method) const {
        return is_interactive(method) ? std::min(options_.requestTimeout, INTERACTIVE_TIMEOUT) : options_.requestTimeout;
    }

    // Sends a client request to clangd. `limit` is when it must have been answered (wait_limit, from
    // when it arrived): time it spent waiting for clangd counts against it.
    void request_now_(const Json& message, Reply reply, Clock::time_point limit, bool searchDefinitions = true) {
        if (Clock::now() >= limit) {
            // Its time went on waiting for clangd; the next engine answers it.
            if (reply) reply(Answer {});
            return;
        }
        const Json& id { message["id"] };
        const std::string method { message.value("method", std::string {}) };
        if (searchDefinitions && method == lsp::method::TEXT_DOCUMENT_DEFINITION && !moduleUnits_.empty()) {
            reply = [this, message, limit, reply = std::move(reply)](Answer answer) mutable { search_definition_(message, std::move(answer), std::move(reply), limit); };
        }
        const Json* params { lsp::find(message, "params") };
        const Json* uri { params != nullptr ? lsp::find_path(*params, { "textDocument", "uri" }) : nullptr };
        const std::int64_t engineId { nextId_++ };
        const auto now = Clock::now();
        pending_[engineId] = PendingRequest { Purpose::client, id, method, uri != nullptr && uri->is_string() ? uri->get<std::string>() : std::string {},
                                              std::min(now + own_timeout_(method), limit), generation_, limit, std::move(reply), now };
        if (uri != nullptr && uri->is_string()) spin_.asked(uri->get<std::string>(), now);
        Json forwarded = message;
        forwarded["id"] = engineId;
        if (!send_(forwarded)) {
            auto it = pending_.find(engineId);
            Reply failed { std::move(it->second.reply) };
            pending_.erase(it);
            if (failed) failed(Answer {});
        }
    }

    // The text clangd is given for an open document: the document's own, or, where clangd 23.1 would spin on it
    // (WA-CLANGD-001), the same text with `;` after each trailing dot, remembered so what clangd says maps back.
    std::string engine_text_(const std::string& uri, std::string_view text) {
        if (traits_.hangsOnTrailingDotModuleName) {
            if (auto sanitized = sanitize_module_names(text); sanitized.changed()) {
                if (!rewritten_.contains(uri)) {
                    log::debug("{} is given to clangd with ';' after a module name that ends in '.' ({}, {})", host_->path_of_uri(uri),
                               TRAILING_DOT_MODULE_NAME, host_->root_directory());
                }
                rewritten_[uri] = std::move(sanitized.insertions);
                return std::move(sanitized.text);
            }
        }
        rewritten_.erase(uri);
        return std::string { text };
    }

    // A change goes to clangd as the client sent it, unless clangd's text is, or was until now, a rewrite of
    // the document's: then clangd gets the whole text as engine_text_ makes it.
    void send_change_(const DocumentView& document, const Json& message) {
        spin_.sent(document.uri, text_hash_(document.text), Clock::now());
        const bool wasRewritten { rewritten_.contains(document.uri) };
        std::string text { engine_text_(document.uri, document.text) };
        if (!wasRewritten && !rewritten_.contains(document.uri)) {
            (void)send_(message);
            return;
        }
        (void)send_(lsp::make_notification("textDocument/didChange",
                                           Json { { "textDocument", Json { { "uri", document.uri }, { "version", document.version } } },
                                                  { "contentChanges", Json::array({ Json { { "text", std::move(text) } } }) } }));
    }

    static void map_out_of_rewrite_(std::span<const Insertion> insertions, Json& diagnostics) {
        const auto map_position = [&](Json& position) {
            if (!position.is_object()) return;
            const auto line = lsp::int_at(position, "line");
            const auto character = lsp::int_at(position, "character");
            if (!line || !character) return;
            const auto original = to_original(insertions, TextPosition { static_cast<int>(*line), static_cast<int>(*character) });
            position["character"] = original.character;
        };
        for (auto& diagnostic : diagnostics) {
            if (!diagnostic.is_object() || !diagnostic.contains("range")) continue;
            map_position(diagnostic["range"]["start"]);
            map_position(diagnostic["range"]["end"]);
        }
    }

    static std::size_t text_hash_(std::string_view text) { return std::hash<std::string_view> {}(text); }

    void open_in_engine_(const DocumentView& document) {
        spin_.sent(document.uri, text_hash_(document.text), Clock::now());
        Json params { { "textDocument", Json { { "uri", document.uri }, { "languageId", document.languageId },
                                               { "version", document.version }, { "text", engine_text_(document.uri, document.text) } } } };
        if (send_(lsp::make_notification("textDocument/didOpen", std::move(params)))) {
            databaseRead_ = true;
            if (!diagnosed_.contains(document.uri)) {
                awaitingDiagnostics_.insert(document.uri);
                const auto now = Clock::now();
                awaitingSince_[document.uri] = now;
                schedule_stuck_check_(now + GENERAL_PATIENCE);
            }
        }
    }

    void schedule_stuck_check_(Clock::time_point at) {
        if (!stuckCheckAt_ || at < *stuckCheckAt_) stuckCheckAt_ = at;
    }

    // Files clangd will not publish diagnostics for go to mcppls's engine until they change.
    void check_stuck_files_(Clock::time_point now) {
        stuckCheckAt_.reset();
        std::vector<std::tuple<std::string, std::string, bool>> stuck;   // (path, why, its module did not compile)
        const bool preparing { lastPrimeProgressAt_ && now - *lastPrimeProgressAt_ < std::chrono::seconds { 60 } };
        for (const auto& document : host_->documents()) {
            const auto since = awaitingSince_.find(document.uri);
            if (document.path.empty() || since == awaitingSince_.end() || !awaitingDiagnostics_.contains(document.uri)) continue;
            // A unit of a module other than its interface: an implementation unit or a partition.
            const auto scan = project::scan_source(document.text);
            if (scan.declaration && (!scan.declaration->isExported || !scan.declaration->partition.empty())) {
                if (const auto failed = modulesFailedAt_.find(scan.declaration->module); failed != modulesFailedAt_.end()) {
                    const auto due = std::max(failed->second, since->second) + FAILED_MODULE_PATIENCE;
                    if (now >= due) {
                        stuck.emplace_back(document.path, std::format("module {} did not compile and clangd published nothing for this unit of it", failed->first), true);
                        continue;
                    }
                    schedule_stuck_check_(due);
                }
            }
            const auto due = since->second + GENERAL_PATIENCE;
            if (now >= due && !preparing) {
                stuck.emplace_back(document.path, "clangd published no diagnostics for it in two minutes, and no module was being prepared", false);
                continue;
            }
            schedule_stuck_check_(now >= due ? now + std::chrono::seconds { 30 } : due);
        }
        for (const auto& [path, why, moduleFailed] : stuck) set_aside_(path, why, moduleFailed ? Reclaim::no : Reclaim::if_busy, moduleFailed);
    }

    void accept_traffic_if_ready_() {
        if (!handshakeDone_ || !planApplied_ || accepting_) return;
        accepting_ = true;
        for (const auto& document : host_->documents()) {
            if (!excluded_path_(document.path) && !quarantined_(document.path)) open_or_hold_(document, planApplied_);
        }
        std::vector<Waiting> toFlush;
        toFlush.swap(deferred_);
        for (auto& [message, reply, limit] : toFlush) {
            if (lsp::kind_of(message) == lsp::Kind::request) {
                const Json* params { lsp::find(message, "params") };
                const Json* uri { params != nullptr ? lsp::find_path(*params, { "textDocument", "uri" }) : nullptr };
                const std::string path { uri != nullptr && uri->is_string() ? host_->path_of_uri(uri->get<std::string>()) : std::string {} };
                if (held_path_(path)) {
                    heldRequests_[base::path_key(path)].push_back(Waiting { std::move(message), std::move(reply), limit });
                } else if (excluded_path_(path) || quarantined_(path)) {
                    if (reply) reply(Answer {});
                } else {
                    request_now_(message, std::move(reply), limit);
                }
            } else {
                (void)send_(message);
            }
        }
        prepare_modules_();
        host_->status_changed();
    }

    // ---- messages from clangd ----------------------------------------------------------

    void handle_message_(const Json& message) {
        switch (lsp::kind_of(message)) {
        case lsp::Kind::response: handle_response_(message); break;
        case lsp::Kind::request: {
            Json forwarded = message;
            forwarded["id"] = host_->client_request_id(ENGINE_ID, generation_, message["id"]);
            if (forwarded.contains("params")) host_->client_view(forwarded["params"]);
            host_->send_to_client(forwarded);
            break;
        }
        case lsp::Kind::notification: handle_notification_(message); break;
        case lsp::Kind::invalid: break;
        }
    }

    void handle_response_(const Json& message) {
        const Json& id { message["id"] };
        if (!id.is_number_integer()) return;
        const auto it = pending_.find(id.get<std::int64_t>());
        if (it == pending_.end()) return;   // answered already, after a timeout
        PendingRequest request { std::move(it->second) };
        pending_.erase(it);
        switch (request.purpose) {
        case Purpose::engine_initialize: {
            const Json capabilities = lsp::find_path(message, { "result", "capabilities" }) != nullptr ? message["result"]["capabilities"] : Json::object();
            host_->engine_settled(ENGINE_ID, capabilities);
            (void)send_(lsp::make_notification("initialized", Json::object()));
            handshakeDone_ = true;
            earlyExits_ = 0;
            accept_traffic_if_ready_();
            host_->status_changed();
            break;
        }
        case Purpose::client: {
            lastAnswerAt_ = Clock::now();
            stuck_.clear();
            if (const std::string path { host_->path_of_uri(request.uri) }; !path.empty()) quarantine_.answered(base::path_key(path));
            if (!request.reply) return;
            if (message.contains("error")) {
                request.reply(Answer { Answer::Kind::error, message["error"] });
                return;
            }
            Json result = message.value("result", Json {});
            drop_generated_locations_(result);
            host_->client_view(result);
            request.reply(Answer { Answer::Kind::result, std::move(result) });
            break;
        }
        }
    }

    void handle_notification_(const Json& message) {
        const std::string method { message.value("method", std::string {}) };
        if (method == "textDocument/clangd.fileStatus") {
            const Json* params { lsp::find(message, "params") };
            if (params != nullptr && params->is_object()) {
                const std::string uri { host_->client_uri(params->value("uri", std::string {})) };
                if (host_->has_document(uri)) {
                    fileStatus_[uri] = params->value("state", std::string {});
                    spin_.state(uri, fileStatus_[uri], Clock::now());
                } else if (const std::string path { host_->path_of_uri(uri) }; !path.empty()) {
                    if (const auto unit = background_.find(base::path_key(path)); unit != background_.end()) unit->second.state = params->value("state", std::string {});
                }
            }
            return;
        }
        if (method == lsp::method::TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS) {
            const Json& params { message["params"] };
            if (finish_prime_(params.value("uri", std::string {}))) return;
            const std::string uri { host_->client_uri(params.value("uri", std::string {})) };
            const std::string diagnosedPath { host_->path_of_uri(uri) };
            const std::string diagnosedKey { diagnosedPath.empty() ? std::string {} : base::path_key(diagnosedPath) };
            // A doomed file's diagnostics are mcppls's own (real-project plan RP1.1, design P1):
            // mark_doomed_ already closed it in clangd, but a publish clangd had queued before that
            // close can still arrive after, and would otherwise clobber the module-failed diagnostic
            // with whatever clangd last computed for it -- usually empty, since it never got far.
            if (!diagnosedKey.empty() && doomedFiles_.contains(diagnosedKey)) return;
            // A unit opened without the editor: its diagnostics are nobody's.
            if (const auto unit = background_.find(diagnosedKey); unit != background_.end() && !host_->has_document(uri)) {
                if (!unit->second.built) {
                    unit->second.built = true;
                    log::info("{} built in clangd in {} ms to find definitions ({})", unit->second.path,
                              std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - unit->second.openedAt).count(), host_->root_directory());
                }
                unit_built_(diagnosedKey);
                return;
            }
            // The empty list clangd sends when such a unit is closed, even when the editor opens the file right after.
            if (closedBackground_.contains(diagnosedKey) && !lsp::int_at(params, "version") && params.value("diagnostics", Json::array()).empty()) {
                closedBackground_.erase(diagnosedKey);
                return;
            }
            if (!searches_.empty() && !diagnosedKey.empty()) unit_built_(diagnosedKey);
            awaitingDiagnostics_.erase(uri);
            awaitingSince_.erase(uri);
            release_prime_units_if_idle_();
            if (!host_->has_document(uri)) {
                Json forwarded = message;
                host_->client_view(forwarded["params"]);
                host_->send_to_client(forwarded);
            } else {
                diagnosed_.insert(uri);
                const auto version = lsp::int_at(params, "version");
                Json diagnostics = params.value("diagnostics", Json::array());
                if (const auto rewritten = rewritten_.find(uri); rewritten != rewritten_.end()) map_out_of_rewrite_(rewritten->second, diagnostics);
                host_->publish_engine_diagnostics(ENGINE_ID, uri, std::move(diagnostics), version);
            }
            host_->status_changed();
            return;
        }
        if (method == lsp::method::WINDOW_SHOW_MESSAGE) {
            // Nothing pops up from the engine; it goes to the log (v1 design 16.2).
            Json forwarded = message;
            forwarded["method"] = "window/logMessage";
            host_->send_to_client(forwarded);
            return;
        }
        host_->send_to_client(message);
    }

    void handle_closed_() {
        const bool early { !handshakeDone_ };
        closedGeneration_ = generation_;
        handshakeDone_ = false;
        accepting_ = false;
        forget_primes_();
        log::warning("clangd exited unexpectedly ({})", host_->root_directory());
        auto old = std::move(pending_);
        pending_.clear();
        for (auto& [id, request] : old) {
            if (request.purpose == Purpose::client && request.reply) request.reply(Answer {});
        }
        answer_searches_();
        if (early && loadFailure_) {   // not a crash: no suspects, no restart
            become_incompatible_(*loadFailure_);
            return;
        }
        const auto now = Clock::now();
        crashes_.push_back(now);
        while (!crashes_.empty() && now - crashes_.front() > std::chrono::minutes { 5 }) crashes_.pop_front();
        add_issue_(Issue { "engine-crashed", "clangd exited unexpectedly", "mcppls.restartServer" });
        // robustness design C6: what clangd was asked about, or given, just before it exited is set aside, so
        // one file that crashes it does not take clangd away from the others.
        std::set<std::string> suspects;
        for (const auto& [id, request] : old) {
            if (request.purpose == Purpose::client) {
                if (const std::string path { host_->path_of_uri(request.uri) }; !path.empty()) suspects.insert(path);
            }
        }
        for (const auto& document : host_->documents()) {
            const auto touched = touchedAt_.find(base::path_key(document.path));
            if (!document.path.empty() && touched != touchedAt_.end() && now - touched->second < std::chrono::seconds { 10 }) suspects.insert(document.path);
        }
        host_->record_event("engine-exit", Json { { "recentExits", crashes_.size() }, { "suspects", Json(std::vector<std::string> { suspects.begin(), suspects.end() }) } });
        for (const auto& path : suspects) set_aside_(path, "clangd exited while working on it", Reclaim::no);
        // An exit before the handshake leaves the server's own initialize waiting on this engine. One
        // may be a fluke the restart below mends; a second is not, and the editor is not kept
        // waiting for the restarts after it: initialize is answered with mcppls's own capabilities.
        // If a later restart succeeds, clangd answers those again; what only clangd advertises
        // (semantic tokens, rename, ...) waits for the next server start.
        if (early && ++earlyExits_ >= 2) host_->engine_settled(ENGINE_ID, Json::object());
        if (crashes_.size() >= 5) {
            unavailable_ = true;
            flush_deferred_without_engine_();
            host_->engine_settled(ENGINE_ID, Json::object());
        } else {
            restartReason_ = "recovering from an exit";
            restartAt_ = std::max(now + std::chrono::seconds { 1 << std::min<std::size_t>(crashes_.size() - 1, 6) }, restartGate_.earliest(now));
        }
        host_->status_changed();
    }

    // The loader said why clangd exited: before its exit was handled, it waits for it; after, it is
    // handled now (the restart that exit scheduled is not wanted any more).
    void handle_load_failure_(const std::string& line) {
        if (handshakeDone_ || incompatible_ || line.empty()) return;
        loadFailure_ = line;
        if (closedGeneration_ == generation_) become_incompatible_(line);
    }

    void become_incompatible_(const std::string& line) {
        log::error("clangd cannot run on this system ({}): {}", host_->root_directory(), line);
        incompatible_ = true;
        unavailable_ = true;
        restartAt_.reset();
        // Not a crash, whatever the exit looked like: the one issue says what it is.
        std::erase_if(issues_, [](const Issue& issue) { return issue.code == "engine-crashed"; });
        add_issue_(Issue { "engine-incompatible",
            std::format("the bundled clangd cannot run on this system ({}); only module-level features are available. "
                        "Supported systems are listed in the install guide", line),
            "mcppls.showLogs", "environment" });
        host_->record_event("engine-incompatible", Json { { "line", line } });
        flush_deferred_without_engine_();
        host_->engine_settled(ENGINE_ID, Json::object());
        host_->status_changed();
    }

    void handle_module_failure_(const Json& failure) {
        const ModuleFailure parsed { failure.value("module", std::string {}), failure.value("reason", std::string {}), failure.value("source", std::string {}) };
        if (parsed.module.empty()) return;
        const FailureKind kind { failure_kind(parsed) };
        // The standard library, which nearly every unit imports: a failure there is how this server built it,
        // not the project, and it would take nearly every module with it. The semantic kit brings its own
        // (robustness design C5).
        const auto standard = moduleSources_.find("std");
        const bool stdFailed { parsed.module == "std" || parsed.module == "std.compat"
                               || (kind == FailureKind::compile && standard != moduleSources_.end() && base::same_path(parsed.failedSource, standard->second)) };
        if (stdFailed && kind != FailureKind::other && !stdFromKit_) {
            stdFromKit_ = true;
            log::warning("clangd could not build the standard library module ({}): {}; reading the project with the semantic kit",
                         host_->root_directory(), parsed.reason);
            host_->record_event("std-fallback-kit", Json { { "module", parsed.module }, { "reason", parsed.reason } });
            add_issue_(Issue { "std-fallback-kit",
                std::format("clangd could not build the toolchain's standard library module ({}); files are read with the semantic kit", parsed.reason),
                "mcppls.showLogs", "environment" });
            host_->request_replan();
            host_->status_changed();
        }
        if (kind == FailureKind::compile) {
            const auto now = Clock::now();
            modulesFailedAt_[parsed.module] = now;
            schedule_stuck_check_(now + FAILED_MODULE_PATIENCE);
            // Closure-scoped failure containment (real-project plan RP1.1, design P1): everything that imports this
            // module, transitively, is doomed with it, and is routed to mcppls's own engine at once
            // instead of waiting out clangd's request timeout or its preparation deadline. The standard
            // library is excepted: it already gets the whole-project kit fallback above, which fixes
            // every importer at once instead of setting them all aside one by one.
            if (!stdFailed && !doomRoots_.contains(parsed.module)) {
                DoomRoot root { parsed.reason, {}, {}, {} };
                if (const auto provider = moduleSources_.find(parsed.module); provider != moduleSources_.end()) {
                    root.provider = provider->second;
                    root.command = moduleCommands_.contains(parsed.module) ? moduleCommands_.find(parsed.module)->second : std::string {};
                }
                for (const auto& source : closure_sources_(parsed.module)) root.inputs.emplace(source, platform::fs::stamp(source));
                doomRoots_.emplace(parsed.module, std::move(root));
                recompute_doom_();
            }
        }
        if (kind != FailureKind::unresolved) {
            // Found and not compiled: its importers get errors, they do not hang (experiment S3). Nothing to replan.
            if (reportedFailures_.insert(parsed.module).second) {
                log::warning("clangd could not build module {} ({}): {}", parsed.module, host_->root_directory(), parsed.reason);
                host_->record_event("module-failed", Json { { "module", parsed.module }, { "kind", kind == FailureKind::compile ? "compile" : "other" },
                                                            { "reason", parsed.reason } });
            }
            return;
        }
        if (unresolvedModules_.contains(parsed.module)) return;
        log::warning("clangd could not find module {} ({}): {}", parsed.module, host_->root_directory(), parsed.reason);
        host_->record_event("module-failed", Json { { "module", parsed.module }, { "kind", "unresolved" }, { "reason", parsed.reason } });
        UnresolvedModule unresolved { parsed.reason, {}, {}, {} };
        if (const auto provider = moduleSources_.find(parsed.module); provider != moduleSources_.end()) {
            unresolved.provider = provider->second;
            unresolved.stamp = platform::fs::stamp(provider->second);
            unresolved.command = moduleCommands_.contains(parsed.module) ? moduleCommands_.find(parsed.module)->second : std::string {};
        }
        unresolvedModules_.emplace(parsed.module, std::move(unresolved));
        host_->request_replan();
    }

    // Unresolved modules whose unit or command is no longer what it was when clangd reported them are
    // forgotten, so the next plan tries them again. Returns whether any was.
    bool forget_changed_unresolved_() {
        bool forgot { false };
        for (auto it = unresolvedModules_.begin(); it != unresolvedModules_.end();) {
            const auto provider = moduleSources_.find(it->first);
            const std::string current { provider == moduleSources_.end() ? std::string {} : provider->second };
            const auto command = moduleCommands_.find(it->first);
            const bool changed { !base::same_path(current, it->second.provider) || (!current.empty() && platform::fs::stamp(current) != it->second.stamp)
                                 || (!current.empty() && (command == moduleCommands_.end() ? std::string {} : command->second) != it->second.command) };
            if (changed) {
                log::info("trying module {} again ({})", it->first, host_->root_directory());
                host_->record_event("module-retry", Json { { "module", it->first } });
                it = unresolvedModules_.erase(it);
                forgot = true;
            } else {
                ++it;
            }
        }
        return forgot;
    }

    // ---- closure-scoped failure containment (real-project plan RP1.1, design P1) ------------------------

    bool doomed_path_(std::string_view path) const { return !path.empty() && doomedFiles_.contains(base::path_key(path)); }

    // Whether the file itself changed within SELF_EDIT_GRACE, or a source of any module it imports (transitively)
    // within GENERAL_PATIENCE: what clangd is busy with is then this change, not this file being stuck.
    bool changed_recently_(std::string_view path, Clock::time_point now) const {
        const auto touched = [&](std::string_view file, Clock::duration within) {
            const auto at = touchedAt_.find(base::path_key(file));
            return at != touchedAt_.end() && now - at->second < within;
        };
        // import-hang plan §4: an edit to the file itself rebuilds only the file, on a preamble it already has, which
        // takes milliseconds to seconds. Only a change to a module it imports makes clangd rebuild modules first.
        if (touched(path, SELF_EDIT_GRACE)) return true;
        for (const auto& module : host_->imports_of(path)) {
            for (const auto& source : closure_sources_(module)) {
                if (touched(source, GENERAL_PATIENCE)) return true;
            }
        }
        return false;
    }

    // A doom root whose unit or command is no longer what it was when clangd reported it failed is
    // forgotten (same rule as forget_changed_unresolved_), so the next attempt goes to clangd again.
    // The sources of `module` and of every module it imports, directly or not: what its compile read.
    std::vector<std::string> closure_sources_(std::string_view module) const {
        std::vector<std::string> sources;
        std::set<std::string, std::less<>> seen { std::string { module } };
        std::deque<std::string> pending { std::string { module } };
        while (!pending.empty()) {
            const std::string next { std::move(pending.front()) };
            pending.pop_front();
            if (const auto source = moduleSources_.find(next); source != moduleSources_.end()) sources.push_back(source->second);
            if (const auto imports = moduleRequires_.find(next); imports != moduleRequires_.end()) {
                for (const auto& imported : imports->second) {
                    if (seen.insert(imported).second) pending.push_back(imported);
                }
            }
        }
        return sources;
    }

    bool forget_changed_doom_() {
        bool forgot { false };
        for (auto it = doomRoots_.begin(); it != doomRoots_.end();) {
            const auto provider = moduleSources_.find(it->first);
            const std::string current { provider == moduleSources_.end() ? std::string {} : provider->second };
            const auto command = moduleCommands_.find(it->first);
            // An input edited, or the closure now made of other sources -- a module provided by another
            // file than before, e.g. a recovered generated source in place of a stand-in.
            const auto sources = closure_sources_(it->first);
            const bool inputChanged { sources.size() != it->second.inputs.size()
                                      || std::ranges::any_of(sources, [&](const std::string& source) { return !it->second.inputs.contains(source); })
                                      || std::ranges::any_of(it->second.inputs, [](const auto& input) { return platform::fs::stamp(input.first) != input.second; }) };
            const bool changed { !base::same_path(current, it->second.provider) || inputChanged
                                 || (!current.empty() && (command == moduleCommands_.end() ? std::string {} : command->second) != it->second.command) };
            if (changed) {
                log::info("trying module {} again ({}): it or a module it imports changed", it->first, host_->root_directory());
                host_->record_event("module-retry", Json { { "module", it->first } });
                it = doomRoots_.erase(it);
                forgot = true;
            } else {
                ++it;
            }
        }
        return forgot;
    }

    // The modules doomed by every current root, and the files that provide one or directly import
    // one — a plain importer like the incident's main.cpp included. Reconciles with what was doomed
    // before: newly doomed files are routed to mcppls's own engine at once, and files no longer
    // doomed go back to clangd.
    void recompute_doom_() {
        std::set<std::string, std::less<>> doomedModules;
        std::map<std::string, std::string, std::less<>> rootOf;   // a doomed module -> the root that dooms it
        for (const auto& [root, info] : doomRoots_) {
            for (const auto& module : doomed_modules(moduleRequires_, root)) {
                doomedModules.insert(module);
                rootOf.try_emplace(module, root);
            }
        }
        doomedModules_ = std::move(doomedModules);
        abandon_doomed_modules_();
        std::map<std::string, DoomedFile, std::less<>> next;
        const auto consider = [&](const std::string& file, const std::string& viaModule) {
            if (next.contains(file)) return;
            const auto root = rootOf.find(viaModule);
            if (root == rootOf.end()) return;
            next.emplace(file, DoomedFile { root->second, viaModule, doomRoots_.at(root->second).reason });
        };
        for (const auto& [file, module] : fileModule_) {
            if (doomedModules_.contains(module)) consider(file, module);
        }
        for (const auto& [file, imports] : fileImports_) {
            for (const auto& imported : imports) {
                if (doomedModules_.contains(imported)) {
                    consider(file, imported);
                    break;
                }
            }
        }
        apply_doom_diff_(std::move(next));
    }

    // Doomed modules are never primed again: nothing is gained by waiting out clangd's own attempt at
    // a module known to be doomed, and a prime unit already running for one is closed so it stops
    // holding a worker.
    void abandon_doomed_modules_() {
        if (doomedModules_.empty()) return;
        for (const auto& module : doomedModules_) {
            primeDeadlines_.erase(module);
            if (const auto* planned = primer_.find(module); planned != nullptr && !planned->primeFile.empty()) {
                const std::string key { base::path_key(planned->primeFile) };
                if (primeModuleByPath_.erase(key) > 0 && accepting_) {
                    (void)send_(lsp::make_notification("textDocument/didClose", Json { { "textDocument", Json { { "uri", base::path_to_uri(planned->primeFile) } } } }));
                }
            }
        }
        primer_.abandon(std::vector<std::string> { doomedModules_.begin(), doomedModules_.end() });
        lastPrimeProgressAt_ = Clock::now();   // resolved, however it resolved: preparation is not stalled by this
        pump_primer_();
    }

    std::string doom_issue_message_() const {
        if (doomedModules_.empty() || doomRoots_.empty()) return {};
        const auto& [first, info] { *doomRoots_.begin() };
        return std::format("{} module{} cannot be prepared because {} failed: {}", doomedModules_.size(), doomedModules_.size() == 1 ? "" : "s",
                            first, info.reason);
    }

    void update_doom_issue_() {
        std::erase_if(issues_, [](const Issue& issue) { return issue.code == "modules-doomed"; });
        if (doomedModules_.empty()) return;
        issues_.push_back(Issue { "modules-doomed", doom_issue_message_(), "mcppls.showLogs", "code" });
    }

    // One diagnostic, on the import (or the module declaration, for a unit of a doomed module
    // itself), naming plainly which module failed — never a flood, whatever else the file imports.
    void publish_doom_diagnostic_(const DocumentView& document, const DoomedFile& info) {
        const auto scan = project::scan_source(document.text);
        base::Range range { { 0, 0 }, { 0, 1 } };
        bool found { false };
        for (const auto& import : scan.imports) {
            if (project::imported_name(scan, import) == info.viaModule) {
                range = import.nameRange;
                found = true;
                break;
            }
        }
        if (!found && scan.declaration && scan.declaration->module == info.viaModule) range = scan.declaration->nameRange;
        const std::string message { info.viaModule == info.rootModule
            ? std::format("module {} did not compile: {}", info.rootModule, info.reason)
            : std::format("module {} cannot be built because {} did not compile: {}", info.viaModule, info.rootModule, info.reason) };
        Json diagnostic { { "range", midx::to_json(range) }, { "severity", 1 }, { "code", std::string { "module-failed" } },
                          { "source", std::string { "mcppls" } }, { "message", message } };
        const std::optional<std::int64_t> version { document.version != 0 ? std::optional<std::int64_t> { document.version } : std::nullopt };
        host_->publish_engine_diagnostics(ENGINE_ID, document.uri, Json::array({ std::move(diagnostic) }), version);
    }

    void mark_doomed_(const std::string& key) {
        const auto info = doomedFiles_.find(key);
        if (info == doomedFiles_.end()) return;
        held_.erase(key);   // never waits for the database either: it is answered by mcppls's own engine
        release_held_requests_(key, false);
        for (const auto& document : host_->documents()) {
            if (document.path.empty() || base::path_key(document.path) != key) continue;
            awaitingDiagnostics_.erase(document.uri);
            awaitingSince_.erase(document.uri);
            // A doomed file is never a reason to restart clangd (design P1): it is released from the
            // quarantine machinery entirely, not merely set aside by it.
            quarantine_.release(key);
            aside_.erase(key);
            if (accepting_) (void)send_(lsp::make_notification("textDocument/didClose", Json { { "textDocument", Json { { "uri", document.uri } } } }));
            publish_doom_diagnostic_(document, info->second);
        }
        host_->record_event("file-doomed", Json { { "file", key }, { "module", info->second.viaModule },
                                                  { "rootModule", info->second.rootModule }, { "reason", info->second.reason } });
    }

    void release_doomed_(const std::string& key) {
        log::info("handing {} back to clangd ({}): what made its module fail has changed, so it is tried again", key, host_->root_directory());
        host_->record_event("file-handed-back", Json { { "file", key }, { "why", "the failed module's inputs changed" } });
        for (const auto& document : host_->documents()) {
            if (document.path.empty() || base::path_key(document.path) != key) continue;
            host_->publish_engine_diagnostics(ENGINE_ID, document.uri, Json::array(), std::nullopt);
            if (accepting_ && !excluded_path_(document.path) && !quarantined_(document.path) && !held_path_(document.path)) open_or_hold_(document, true);
        }
    }

    void apply_doom_diff_(std::map<std::string, DoomedFile, std::less<>> next) {
        std::vector<std::string> toMark;
        for (const auto& [key, info] : next) {
            const auto previous = doomedFiles_.find(key);
            if (previous == doomedFiles_.end() || previous->second.viaModule != info.viaModule || previous->second.reason != info.reason) {
                toMark.push_back(key);
            }
        }
        std::vector<std::string> released;
        for (const auto& [key, info] : doomedFiles_) {
            if (!next.contains(key)) released.push_back(key);
        }
        doomedFiles_ = std::move(next);
        for (const auto& key : toMark) mark_doomed_(key);
        for (const auto& key : released) release_doomed_(key);
        if (toMark.empty() && released.empty()) return;
        update_doom_issue_();
        if (!toMark.empty()) {
            log::warning("{} file{} routed to mcppls's own engine ({}): {}", toMark.size(), toMark.size() == 1 ? "" : "s", host_->root_directory(),
                         doom_issue_message_());
        }
        host_->status_changed();
    }

    // ---- files set aside, and restarts -------------------------------------------------------

    void touch_(std::string_view path) {
        if (path.empty()) return;
        touchedAt_[base::path_key(path)] = Clock::now();
        lastSourceChangeAt_ = Clock::now();
    }

    bool quarantined_(std::string_view path) const { return !path.empty() && quarantine_.contains(base::path_key(path)); }

    // import-hang plan §4: clangd has been building a file far longer than it ever took while the editor has moved
    // on. Whatever the cause (the defect behind WA-CLANGD-001 was one, found in the field), the build will not end, so the file goes to
    // mcppls's engine with the text clangd spun on remembered, and clangd is restarted without it.
    void handle_spins_(Clock::time_point now) {
        for (const auto& spin : spin_.check(now)) {
            std::string path;
            for (const auto& document : host_->documents()) {
                if (document.uri == spin.uri) path = document.path;
            }
            if (path.empty() || quarantined_(path)) continue;
            const auto seconds = [](std::chrono::milliseconds duration) { return std::chrono::duration<double>(duration).count(); };
            log::warning("clangd ({}) has built {} for {:.0f} s, past its {:.0f} s budget, while the editor waited on it: it will not finish it",
                         host_->root_directory(), base::file_name(path), seconds(spin.building), seconds(spin.budget));
            host_->record_event("engine-spin", Json { { "file", path }, { "buildingSeconds", seconds(spin.building) }, { "budgetSeconds", seconds(spin.budget) } });
            set_aside_(path, "clangd would not finish building it", Reclaim::now, false, spin.textHash);
        }
    }

    // Whether a restart gets back what clangd spends on a file it is no longer given.
    // `now`: what clangd spends on it is never coming back (SpinWatch): restart at once, past the gate and the cap.
    enum class Reclaim { no, if_busy, now };

    void set_aside_(const std::string& path, std::string_view why, Reclaim reclaim, bool moduleFailed = false,
                    std::optional<std::size_t> spunOn = std::nullopt) {
        const std::string key { base::path_key(path) };
        if (aside_.contains(key) && quarantine_.contains(key)) return;   // set aside already
        if (!quarantine_.contains(key)) quarantine_.put(key, Clock::now());
        Aside aside { {}, moduleFailed, spunOn };
        std::optional<std::string> state;
        for (const auto& document : host_->documents()) {
            if (document.path.empty() || base::path_key(document.path) != key) continue;
            aside.structure = structure_of_text_(document.text);
            if (const auto status = fileStatus_.find(document.uri); status != fileStatus_.end()) state = status->second;
            awaitingDiagnostics_.erase(document.uri);
            awaitingSince_.erase(document.uri);
            if (accepting_) (void)send_(lsp::make_notification("textDocument/didClose", Json { { "textDocument", Json { { "uri", document.uri } } } }));
        }
        aside_[key] = std::move(aside);
        log::warning("setting {} aside from clangd for a while ({}): {}; clangd was {}; mcppls's engine answers for it", path, host_->root_directory(), why,
                     state.value_or("in an unknown state"));
        host_->record_event("file-set-aside", Json { { "file", path }, { "why", std::string { why } }, { "clangdState", state.value_or("") } });
        update_quarantine_issue_();
        // clangd does not stop building a file it is no longer given: a build that never ends (the spin in experiment S17) keeps a
        // core and one of clangd's workers for as long as clangd runs. A fresh clangd, without the file, gets both back.
        if (reclaim == Reclaim::now && accepting_) {
            // Not deferred, not spaced out, not capped: a clangd left spinning answers nothing for this file and holds a core,
            // and the file it spun on stays with mcppls's engine, so the new clangd cannot be sent the same way.
            if (restartGate_.at_cap(Clock::now())) {
                log::warning("restarting clangd ({}) past the restart cap: it cannot be left spinning on {}, which stays with mcppls's engine",
                             host_->root_directory(), base::file_name(path));
                host_->record_event("engine-restart-past-cap", Json { { "file", path } });
            }
            restart_(std::format("clangd would not finish {}", base::file_name(path)));
            return;
        }
        if (reclaim == Reclaim::if_busy && accepting_ && (!state || engine_working(*state))) {
            // Right after a source changed, clangd being busy is clangd rebuilding what the change
            // touched -- a module this file imports, one that may be about to fail and be contained
            // (real-project plan RP1.2). A restart would throw that work away and start it again;
            // the file stays aside, and a restart is considered only once the edit is not recent.
            if (changed_recently_(path, Clock::now())) {
                log::info("not restarting clangd ({}) for {} yet: it is rebuilding after a source changed", host_->root_directory(), base::file_name(path));
                host_->record_event("restart-deferred", Json { { "file", path }, { "why", "a source it needs changed recently" } });
                deferredReclaims_.insert_or_assign(key, path);
                return;
            }
            schedule_restart_(std::format("clangd kept working on {} after it was set aside", base::file_name(path)));
        }
    }

    // A restart put off because a source the file needs had just changed: once that is no longer
    // recent, a file still aside that clangd is still working on is what it was set aside for in the
    // first place (experiment S17's spin), and the restart goes ahead.
    void reconsider_deferred_reclaims_(Clock::time_point now) {
        if (deferredReclaims_.empty() || !accepting_) return;
        auto deferred = std::move(deferredReclaims_);
        deferredReclaims_.clear();
        for (const auto& [key, path] : deferred) {
            if (!aside_.contains(key)) continue;
            if (changed_recently_(path, now)) {
                deferredReclaims_.insert_or_assign(key, path);   // still rebuilding what changed
                continue;
            }
            std::optional<std::string> state;
            for (const auto& document : host_->documents()) {
                if (document.path.empty() || base::path_key(document.path) != key) continue;
                if (const auto status = fileStatus_.find(document.uri); status != fileStatus_.end()) state = status->second;
            }
            if (!state || engine_working(*state)) {
                schedule_restart_(std::format("clangd kept working on {} after it was set aside", base::file_name(path)));
                return;   // one restart serves every file
            }
        }
    }

    // A file's module structure: what it provides and what it imports.
    static std::string structure_of_text_(std::string_view text) {
        const auto scan = project::scan_source(text);
        std::string structure { project::provided_name(scan) };
        for (const auto& name : project::required_names(scan)) structure += "|" + name;
        return structure;
    }

    bool held_path_(std::string_view path) const { return !path.empty() && held_.contains(base::path_key(path)); }

    std::vector<std::string> held_files_() const {
        std::vector<std::string> files;
        for (const auto& [key, held] : held_) files.push_back(key);
        return files;
    }

    // Whether clangd may be given the file now (held_): its database has had it long enough to have been read, or the
    // plan did not take it, or no plan will.
    bool ready_for_engine_(std::string_view path, const std::string& key, const HeldFile& held, Clock::time_point now) const {
        if (const auto joined = joinedAt_.find(key); joined != joinedAt_.end()) return now >= joined->second + DATABASE_REREAD;
        if (writtenArguments_.contains(key) || writtenDatabase_.empty()) return true;
        if (!project::is_cxx_source_name(path) || !base::is_within(path, host_->root_directory())) return true;
        return held.planned || now >= held.since + PLAN_PATIENCE;
    }

    // Gives clangd the document, or holds it until clangd's database has it.
    void open_or_hold_(const DocumentView& document, bool planned) {
        if (document.path.empty()) {
            open_in_engine_(document);
            return;
        }
        // A single gate for every caller (real-project plan RP1.1, design P1): a doomed file is never opened in
        // clangd, from here, whatever reason brought this call about.
        if (doomed_path_(document.path)) return;
        const auto now = Clock::now();
        const std::string key { base::path_key(document.path) };
        const auto [it, fresh] = held_.try_emplace(key, HeldFile { now, planned });
        if (!ready_for_engine_(document.path, key, it->second, now)) {
            if (fresh) {
                log::info("{} waits for clangd to read an engine database that has it ({})", document.path, host_->root_directory());
                host_->record_event("file-waits-for-database", Json { { "file", document.path } });
            }
            return;
        }
        held_.erase(it);
        open_in_engine_(document);
        release_held_requests_(key, true);
    }

    void open_held_files_(Clock::time_point now) {
        if (held_.empty() || !accepting_) return;
        for (const auto& document : host_->documents()) {
            if (document.path.empty()) continue;
            const std::string key { base::path_key(document.path) };
            const auto it = held_.find(key);
            if (it == held_.end()) continue;
            if (excluded_.contains(key) || quarantine_.contains(key) || doomedFiles_.contains(key)) {
                held_.erase(it);
                release_held_requests_(key, false);
                continue;
            }
            if (!ready_for_engine_(document.path, key, it->second, now)) continue;
            held_.erase(it);
            host_->record_event("file-given-to-engine", Json { { "file", document.path } });
            open_in_engine_(document);
            prepare_imports_of_(document);
            release_held_requests_(key, true);
        }
        release_prime_units_if_idle_();
    }

    // A restart at the next timer, as soon as the gate allows: for callers in the middle of work on the requests a restart ends.
    // Restarts are capped, not merely spaced out (real-project plan RP1.2): past RestartGate::
    // MAX_RESTARTS_PER_WINDOW in RestartGate::WINDOW, clangd stays down for whatever it cannot answer
    // until the window ages out, rather than restarting forever for reasons that keep recurring.
    bool restart_capped_(std::string_view reason) {
        const auto now = Clock::now();
        if (!restartGate_.at_cap(now)) return false;
        add_issue_(Issue { "engine-restart-capped",
            std::format("clangd was restarted {} times in the last {} minutes; it stays down for the files it cannot answer for until that passes",
                        RestartGate::MAX_RESTARTS_PER_WINDOW, RestartGate::WINDOW.count()), "mcppls.showLogs" });
        log::warning("not restarting clangd again ({}): already {} restarts in the last {} minutes ({})", host_->root_directory(),
                     RestartGate::MAX_RESTARTS_PER_WINDOW, RestartGate::WINDOW.count(), reason);
        host_->record_event("engine-restart-capped", Json { { "reason", std::string { reason } } });
        host_->status_changed();
        return true;
    }

    void schedule_restart_(std::string_view reason) {
        if (restart_capped_(reason)) return;
        const auto now = Clock::now();
        const auto at = restartGate_.earliest(now);
        if (restartAt_ && *restartAt_ <= at) return;
        restartAt_ = at;
        restartReason_ = std::string { reason };
        host_->record_event("engine-restart-scheduled", Json { { "reason", std::string { reason } },
                                                               { "seconds", std::chrono::duration_cast<std::chrono::seconds>(at - now).count() } });
        log::info("restarting clangd ({}) in {} s: {}", host_->root_directory(), std::chrono::duration_cast<std::chrono::seconds>(at - now).count(), reason);
    }

    void update_quarantine_issue_() {
        std::erase_if(issues_, [](const Issue& issue) { return issue.code == "file-quarantined"; });
        if (const auto members = quarantine_.members(); !members.empty()) {
            // import-hang plan §6: the files by name, and what they still get.
            std::string names;
            for (std::size_t i { 0 }; i < members.size() && i < 3; ++i) names += std::format("{}{}", i == 0 ? "" : ", ", base::file_name(members[i]));
            if (members.size() > 3) names += std::format(" and {} more", members.size() - 3);
            issues_.push_back(Issue { "file-quarantined",
                std::format("clangd stopped responding on {}; module-level features only for {} until {} changes", names,
                            members.size() == 1 ? "it" : "them", members.size() == 1 ? "it" : "they"), "mcppls.restartServer" });
        }
        host_->status_changed();
    }

    // A restart now, or as soon as the gate allows (robustness design C4).
    void request_restart_(std::string_view reason) {
        if (restart_capped_(reason)) return;
        const auto now = Clock::now();
        const auto at = restartGate_.earliest(now);
        if (at <= now) {
            restart_(reason);
            return;
        }
        if (!restartAt_ || at < *restartAt_) {
            restartAt_ = at;
            restartReason_ = std::string { reason };
            host_->record_event("engine-restart-deferred", Json { { "reason", std::string { reason } },
                                                                  { "seconds", std::chrono::duration_cast<std::chrono::seconds>(at - now).count() } });
            log::info("restarting clangd ({}) in {} s: {}", host_->root_directory(),
                      std::chrono::duration_cast<std::chrono::seconds>(at - now).count(), reason);
        }
    }

    // ---- definitions in implementation units (robustness design C10) ---------------------------

    std::vector<std::string> background_files_() const {
        std::vector<std::string> files;
        for (const auto& [key, unit] : background_) files.push_back(unit.path);
        return files;
    }

    static void for_each_location_(const Json& result, const std::function<void(const std::string&, const Json&)>& visit) {
        const auto one = [&](const Json& location) {
            if (!location.is_object()) return;
            if (const Json* target = lsp::find(location, "targetUri"); target != nullptr && target->is_string()) {
                const Json* range { lsp::find(location, "targetSelectionRange") };
                visit(target->get<std::string>(), range != nullptr ? *range : Json::object());
            } else if (const Json* uri = lsp::find(location, "uri"); uri != nullptr && uri->is_string()) {
                const Json* range { lsp::find(location, "range") };
                visit(uri->get<std::string>(), range != nullptr ? *range : Json::object());
            }
        };
        if (result.is_array()) {
            for (const auto& location : result) one(location);
        } else {
            one(result);
        }
    }

    DeclarationKind declaration_kind_at_(const std::string& path, const Json& range) const {
        const Json* end { lsp::find(range, "end") };
        if (end == nullptr || !end->is_object()) return DeclarationKind::unknown;
        const base::Position position { end->value("line", 0), end->value("character", 0) };
        std::string text;
        bool open { false };
        for (const auto& document : host_->documents()) {
            if (!document.path.empty() && base::same_path(document.path, path)) {
                text = std::string { document.text };
                open = true;
                break;
            }
        }
        if (!open) {
            auto read = platform::fs::read_file(path);
            if (!read) return DeclarationKind::unknown;
            text = std::move(*read);
        }
        const auto offset = base::offset_at(text, position);
        return offset ? declaration_kind(text, *offset) : DeclarationKind::unknown;
    }

    // clangd's answer to a definition request. When every location it gives is a declaration only, in a module's interface,
    // the module's other units are built and the question asked again.
    // `limit` is the client's request's own (wait_limit): the search for definitions in units clangd has
    // not built yet waits within it, never past it.
    void search_definition_(const Json& message, Answer answer, Reply reply, Clock::time_point limit) {
        if (answer.kind != Answer::Kind::result || !accepting_) {
            reply(std::move(answer));
            return;
        }
        std::set<std::string, std::less<>> modules;
        std::size_t locations { 0 };
        bool onlyDeclarations { true };
        for_each_location_(answer.value, [&](const std::string& uri, const Json& range) {
            ++locations;
            const std::string path { host_->path_of_uri(uri) };
            const auto module = path.empty() ? interfaceModules_.end() : interfaceModules_.find(base::path_key(path));
            if (module == interfaceModules_.end() || declaration_kind_at_(path, range) != DeclarationKind::declaration) {
                onlyDeclarations = false;
                return;
            }
            modules.insert(module->second);
        });
        if (locations == 0 || !onlyDeclarations) {
            reply(std::move(answer));
            return;
        }
        const auto now = Clock::now();
        std::set<std::string> waiting;
        std::vector<std::string> opened;
        for (const auto& module : modules) {
            const auto units = moduleUnits_.find(module);
            if (units == moduleUnits_.end()) continue;
            const auto interface = moduleSources_.find(module);
            for (const auto& path : units_to_search(interface == moduleSources_.end() ? std::string_view {} : std::string_view { interface->second },
                                                    units->second, UNITS_PER_SEARCH)) {
                const std::string key { base::path_key(path) };
                if (const auto uri = editor_uri_of_(key)) {
                    // The editor has it open: clangd builds it anyway.
                    if (awaitingDiagnostics_.contains(*uri)) waiting.insert(key);
                    continue;
                }
                if (const auto unit = background_.find(key); unit != background_.end()) {
                    unit->second.usedAt = now;
                    if (!unit->second.built) waiting.insert(key);
                    continue;
                }
                if (open_in_background_(path, module, now)) {
                    waiting.insert(key);
                    opened.push_back(path);
                }
            }
        }
        if (waiting.empty()) {
            reply(std::move(answer));
            return;
        }
        if (!opened.empty()) host_->record_event("definition-search", Json { { "modules", Json(std::vector<std::string> { modules.begin(), modules.end() }) }, { "opened", opened } });
        searches_.push_back(DefinitionSearch { message, std::move(answer.value), std::move(reply), std::move(waiting),
                                              std::min(now + DEFINITION_PATIENCE, limit), limit });
    }

    std::optional<std::string> editor_uri_of_(std::string_view key) const {
        for (const auto& document : host_->documents()) {
            if (document.path.empty() || base::path_key(document.path) != key) continue;
            // A document clangd does not have is no help.
            if (excluded_path_(document.path) || quarantined_(document.path) || held_path_(document.path)) return std::string {};
            return document.uri;
        }
        return std::nullopt;
    }

    bool open_in_background_(const std::string& path, std::string_view module, Clock::time_point now) {
        const std::string key { base::path_key(path) };
        if (const auto refused = backgroundRefused_.find(key); refused != backgroundRefused_.end()) {
            if (platform::fs::stamp(path) == refused->second) return false;
            backgroundRefused_.erase(refused);   // it changed: it may build now
        }
        if (excluded_.contains(key) || quarantine_.contains(key) || !writtenArguments_.contains(key)) return false;
        if (const auto joined = joinedAt_.find(key); joined != joinedAt_.end() && now < joined->second + DATABASE_REREAD) return false;
        // A unit of a module that did not compile can be one clangd never finishes (robustness design C6).
        if (modulesFailedAt_.contains(module) || reportedFailures_.contains(module)) return false;
        while (background_.size() >= BACKGROUND_UNITS) {
            auto oldest = background_.end();
            for (auto it = background_.begin(); it != background_.end(); ++it) {
                if (!it->second.built || waited_on_(it->first)) continue;
                if (oldest == background_.end() || it->second.usedAt < oldest->second.usedAt) oldest = it;
            }
            if (oldest == background_.end()) return false;
            close_background_(std::string { oldest->first });
        }
        auto text = platform::fs::read_file(path);
        if (!text) return false;
        const std::string uri { base::path_to_uri(path) };
        // WA-CLANGD-001: a file saved mid-edit can hold the text clangd spins on. Nobody reads positions in a
        // unit opened without the editor, so the rewrite needs no mapping back.
        if (traits_.hangsOnTrailingDotModuleName) {
            if (auto sanitized = sanitize_module_names(*text); sanitized.changed()) *text = std::move(sanitized.text);
        }
        Json params { { "textDocument", Json { { "uri", uri }, { "languageId", "cpp" }, { "version", 1 }, { "text", std::move(*text) } } } };
        if (!send_(lsp::make_notification("textDocument/didOpen", std::move(params)))) return false;
        databaseRead_ = true;
        closedBackground_.erase(key);
        background_[key] = BackgroundUnit { path, uri, now, now, false, {} };
        log::info("opening {} in clangd to find definitions in module {} ({})", path, module, host_->root_directory());
        return true;
    }

    bool waited_on_(std::string_view key) const {
        return std::ranges::any_of(searches_, [&](const DefinitionSearch& search) { return search.waitingFor.contains(std::string { key }); });
    }

    void close_background_(const std::string& key) {
        const auto unit = background_.find(key);
        if (unit == background_.end()) return;
        if (accepting_ && send_(lsp::make_notification("textDocument/didClose", Json { { "textDocument", Json { { "uri", unit->second.uri } } } }))) {
            closedBackground_.insert(key);
        }
        background_.erase(unit);
    }

    void unit_built_(const std::string& key) {
        std::vector<DefinitionSearch> ready;
        for (auto it = searches_.begin(); it != searches_.end();) {
            it->waitingFor.erase(key);
            if (it->waitingFor.empty()) {
                ready.push_back(std::move(*it));
                it = searches_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto& search : ready) ask_definition_again_(std::move(search));
    }

    void ask_definition_again_(DefinitionSearch search) {
        if (!accepting_) {
            search.reply(Answer { Answer::Kind::result, std::move(search.firstAnswer) });
            return;
        }
        // Asked again within the client's own limit, with clangd's first answer if this one finds nothing
        // (including when nothing of the limit is left: request_now_ answers unavailable at once).
        request_now_(search.message, [first = std::move(search.firstAnswer), reply = std::move(search.reply)](Answer answer) mutable {
            const bool found { answer.kind == Answer::Kind::result && !answer.value.is_null() && !(answer.value.is_array() && answer.value.empty()) };
            if (found) reply(std::move(answer));
            else reply(Answer { Answer::Kind::result, std::move(first) });
        }, search.limit, false);
    }

    // Searches get what clangd answered first, as when it stops.
    void answer_searches_() {
        auto searches = std::move(searches_);
        searches_.clear();
        for (auto& search : searches) {
            if (search.reply) search.reply(Answer { Answer::Kind::result, std::move(search.firstAnswer) });
        }
    }

    void handle_background_timers_(Clock::time_point now) {
        std::vector<DefinitionSearch> overdue;
        for (auto it = searches_.begin(); it != searches_.end();) {
            if (it->deadline <= now) {
                overdue.push_back(std::move(*it));
                it = searches_.erase(it);
            } else {
                ++it;
            }
        }
        // Asked again with what clangd has built by now.
        for (auto& search : overdue) ask_definition_again_(std::move(search));
        std::vector<std::string> closing;
        for (const auto& [key, unit] : background_) {
            if (!unit.built && now >= unit.openedAt + BACKGROUND_BUILD_LIMIT) {
                log::warning("{} did not build in clangd in {} minutes; it is not opened without the editor again until it changes ({}); clangd was {}",
                             unit.path, BACKGROUND_BUILD_LIMIT.count(), host_->root_directory(), unit.state.empty() ? std::string { "in an unknown state" } : unit.state);
                host_->record_event("background-unit-stuck", Json { { "file", unit.path }, { "clangdState", unit.state } });
                backgroundRefused_[key] = platform::fs::stamp(unit.path);
                closing.push_back(key);
                if (unit.state.empty() || engine_working(unit.state)) schedule_restart_(std::format("clangd kept working on {}", base::file_name(unit.path)));
            } else if (unit.built && now >= unit.usedAt + BACKGROUND_IDLE && !waited_on_(key)) {
                closing.push_back(key);
            }
        }
        for (const auto& key : closing) close_background_(key);
    }

    // ---- parallel module preparation ------------------------------------------------------

    void write_prime_sources_(const normalize::EnginePlan& plan) {
        if (!plan.primeSources.empty()) (void)platform::fs::create_directories(primeDirectory_);
        if (!plan.stubSources.empty()) (void)platform::fs::create_directories(stubDirectory_);
        for (const auto& sources : { &plan.primeSources, &plan.stubSources }) {
            for (const auto& [file, content] : *sources) {
                if (platform::fs::read_file(file).value_or("") != content) (void)platform::fs::write_file(file, content);
            }
        }
    }

    // Locations in files this server generates (stand-ins, prime units) are not places in the project: a
    // definition of a module nothing provides is no definition at all.
    void drop_generated_locations_(Json& result) const {
        const auto generated = [&](const Json& location) {
            if (!location.is_object()) return false;
            const std::string uri { location.value("uri", location.value("targetUri", std::string {})) };
            if (uri.empty()) return false;
            const std::string path { host_->path_of_uri(uri) };
            return !path.empty() && ((!stubDirectory_.empty() && base::is_within(path, stubDirectory_)) || (!primeDirectory_.empty() && base::is_within(path, primeDirectory_)));
        };
        if (result.is_array()) {
            result.erase(std::remove_if(result.begin(), result.end(), generated), result.end());
        } else if (generated(result)) {
            result = nullptr;
        }
    }

    // std, which nearly every file imports, and the imports of every open document, then as many
    // ready modules as the limit allows. std.compat waits for a file that imports it: building it
    // takes cores a cold start needs.
    void prepare_modules_() {
        if (!accepting_) return;
        const std::vector<std::string> standard { "std" };
        primer_.want(standard);
        for (const auto& document : host_->documents()) prepare_imports_of_(document, false);
        pump_primer_();
    }

    void prepare_imports_of_(const DocumentView& document, bool pump = true) {
        if (document.path.empty() || excluded_path_(document.path)) return;
        const auto names = host_->imports_of(document.path);
        if (primer_.want(names) > 0 && pump) pump_primer_();
    }

    // clangd's persistent module cache as it is now: for each source file name, the BMIs built from
    // it (<database>/.cache/clangd/modules/<source file name>-<hash>/<command hash>/<module>.pcm).
    std::map<std::string, std::vector<std::string>, std::less<>> cached_bmis_() const {
        std::map<std::string, std::vector<std::string>, std::less<>> bmis;
        for (const auto& sourceDirectory : platform::fs::list_directory(base::join_path(databaseDirectory_, ".cache/clangd/modules"))) {
            const std::string_view name { base::file_name(sourceDirectory) };
            const std::size_t dash { name.rfind('-') };
            if (dash == std::string_view::npos || dash == 0) continue;
            auto& files = bmis[std::string { name.substr(0, dash) }];
            for (const auto& commandDirectory : platform::fs::list_directory(sourceDirectory)) {
                for (auto& file : platform::fs::list_directory(commandDirectory)) {
                    if (file.ends_with(".pcm")) files.push_back(std::move(file));
                }
            }
        }
        return bmis;
    }

    // usable plan W7: whether clangd already keeps `module`'s BMI, finished (no lock file beside it)
    // and at least as new as the module's source. An importer's own build then reuses it, and a
    // prime unit would only take clangd workers from the files a person opened.
    bool module_already_built_(const PrimeModule& module) {
        const auto source = moduleSources_.find(module.name);
        if (source == moduleSources_.end()) return false;
        if (!startupBmis_) startupBmis_ = cached_bmis_();
        const auto files = startupBmis_->find(base::file_name(source->second));
        if (files == startupBmis_->end()) return false;
        const auto sourceStamp = platform::fs::stamp(source->second);
        if (!sourceStamp) return false;
        std::string wanted { module.name };
        std::ranges::replace(wanted, ':', '-');
        wanted += ".pcm";
        return std::ranges::any_of(files->second, [&](const std::string& file) {
            if (base::file_name(file) != wanted || platform::fs::exists(file + ".lock")) return false;
            const auto bmiStamp = platform::fs::stamp(file);
            return bmiStamp && bmiStamp->modified >= sourceStamp->modified;
        });
    }

    // Whether the files a person is waiting for are waiting on preparation itself. Throttling
    // preparation while that is true starves the work that would answer them (guard.cppm).
    bool waiting_on_preparation_() const {
        if (awaitingDiagnostics_.empty() || !primer_.busy()) return false;
        for (const std::string& uri : awaitingDiagnostics_) {
            const auto path = base::uri_to_path(uri);
            if (!path || path->empty()) continue;
            for (const std::string& name : host_->imports_of(*path)) {
                if (primer_.state(name) != Primer::State::done) return true;
            }
        }
        return false;
    }

    void pump_primer_() {
        if (!accepting_) return;
        // A baseline for the status settling (real-project plan RP1.4, design P7): preparation starting counts as
        // progress too, so the stall timeout is measured from when there was last something to show for
        // it, not from some earlier moment nothing had happened yet.
        if (primer_.busy() && !lastPrimeProgressAt_) lastPrimeProgressAt_ = Clock::now();
        // Prime units take the same clangd workers (-j, one per core) as everything a person does:
        // opening a file, typing, asking for completion. Preparation leaves a core to each file still
        // waiting for its modules and one more for requests, so it never holds every worker (hardware
        // threads count as two per core except on macOS, where they are cores).
        // robustness design C7: a quarter of the cores, and half of that while a file waits on something
        // preparation cannot supply — but NOT while it waits on preparation itself, which is the cold
        // start and was measured at one busy core of 32 before this distinction existed.
        primer_.set_limit(preparation_limit(std::thread::hardware_concurrency(), mcppls::os::FAMILY == mcppls::os::Family::macos,
                                            awaitingDiagnostics_.size(), waiting_on_preparation_()));
        for (const PrimeModule* module : primer_.start_ready([this](const PrimeModule& candidate) { return module_already_built_(candidate); })) {
            const std::string uri { base::path_to_uri(module->primeFile) };
            Json params { { "textDocument", Json { { "uri", uri }, { "languageId", "cpp" }, { "version", 1 },
                                                   { "text", std::format("import {};\n", module->name) } } } };
            if (!send_(lsp::make_notification("textDocument/didOpen", std::move(params)))) {
                primer_.finish(module->name);
                continue;
            }
            databaseRead_ = true;
            primeModuleByPath_[base::path_key(module->primeFile)] = module->name;
            primeDeadlines_[module->name] = Clock::now() + std::chrono::minutes { 3 };
        }
        host_->status_changed();
    }

    bool finish_prime_(std::string_view engineUri) {
        if (primeDirectory_.empty()) return false;
        const std::string canonical { host_->path_of_uri(engineUri) };
        if (canonical.empty() || !base::is_within(canonical, primeDirectory_)) return false;
        const std::string pathKey { base::path_key(canonical) };
        const auto it = primeModuleByPath_.find(pathKey);
        if (it == primeModuleByPath_.end()) return true;
        const std::string module { it->second };
        primeModuleByPath_.erase(it);
        primeDeadlines_.erase(module);
        heldPrimeUnits_.emplace(pathKey, canonical);
        primer_.finish(module);
        lastPrimeProgressAt_ = Clock::now();
        pump_primer_();
        release_prime_units_if_idle_();
        return true;
    }

    void release_prime_units_if_idle_() {
        // A file waiting for the database needs the same modules once it is given to clangd.
        if (heldPrimeUnits_.empty() || primer_.busy() || !awaitingDiagnostics_.empty() || !held_.empty()) return;
        log::info("module preparation idle ({}): closing {} prime units", host_->root_directory(), heldPrimeUnits_.size());
        close_prime_units_();
    }

    void close_prime_units_() {
        std::set<std::string> uris;
        for (const auto& [pathKey, module] : primeModuleByPath_) {
            if (const auto* planned = primer_.find(module)) uris.insert(base::path_to_uri(planned->primeFile));
        }
        for (const auto& [pathKey, path] : heldPrimeUnits_) uris.insert(base::path_to_uri(path));
        if (accepting_) {
            for (const auto& uri : uris) (void)send_(lsp::make_notification("textDocument/didClose", Json { { "textDocument", Json { { "uri", uri } } } }));
        }
        primeModuleByPath_.clear();
        primeDeadlines_.clear();
        heldPrimeUnits_.clear();
    }

    void forget_primes_() {
        primeModuleByPath_.clear();
        primeDeadlines_.clear();
        heldPrimeUnits_.clear();
        primer_.reset();
        // reset() forgets every module's state, doomed ones included (its own doc comment: "as after an
        // engine restart"); a fresh clangd still cannot build them, so they are marked doomed again at
        // once rather than waiting out the same failure a second time (real-project plan RP1.1, design P1).
        if (!doomedModules_.empty()) primer_.abandon(std::vector<std::string> { doomedModules_.begin(), doomedModules_.end() });
    }
};

} // namespace

std::unique_ptr<Engine> make_engine(Options options) { return std::make_unique<ClangdEngine>(std::move(options)); }

} // namespace mcppls::engine::clangd
