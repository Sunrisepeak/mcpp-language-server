// One workspace root (overall design 6.1): its project model, source index, plan, documents and the
// engines serving it. The workspace routes each client request to engines by their declared
// capabilities, merges what they answer and publishes status (S3) and merged diagnostics. A session
// (the LSP entry) composes one per root and speaks for the client.
export module mcppls.orchestrator.workspace;

import std;
import nlohmann.json;
import mcppls.base.text;
import mcppls.platform.fs;
import mcppls.platform.task;
import mcppls.project.model;
import mcppls.normalize.plan;
import mcppls.engine;
import mcppls.engine.payload;
import mcppls.engine.native.index;
import mcppls.orchestrator.client;

export namespace mcppls::orchestrator {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

// The engines of a root, made by the composition root (cli): mcppls's own engine over the root's
// module index, and the core engine for C++ semantics (none when it returns nullptr).
struct EngineFactories {
    std::function<std::unique_ptr<engine::Engine>(const index::ModuleIndex&)> modules;
    std::function<std::unique_ptr<engine::Engine>()> core;
};

struct SessionOptions {
    std::string payloadDirectory;
    std::string clangd;
    std::string kit;
    std::string mcpp;                      // the mcpp executable for mcpp projects; empty: found on PATH
    std::string database;                  // a workspace's own S1 document, relative to the root (usable plan W9.2)
    std::string engine { "clangd" };       // the core engine: clangd | none (overall design 5.6)
    bool engineFromCommandLine { false };  // --engine was given, so the client's initializationOptions do not override it
    bool trusted { true };
    bool discoverCompilers { true };
    // mcppls.buildTool (design 4.4): offline --- the default --- runs the build tool without the
    // network; online lets it reach the network and gives it ten minutes; off never runs it.
    std::string buildTool { "offline" };
    // mcppls.toolEnvironment (design 4.3): auto reads the login shell on POSIX; editor always uses
    // this process's environment, for a shell configuration with side effects.
    std::string toolEnvironment { "auto" };
    // Zero: the design's own bound (a minute offline, ten minutes when the network is allowed).
    std::chrono::seconds producerTimeout { 0 };
    bool verboseEngineLog { false };
    std::chrono::milliseconds requestTimeout { std::chrono::seconds { 60 } };
    // Registered workarounds turned off (import-hang plan §9): to see whether one is still needed.
    std::vector<std::string> disabledWorkarounds;
    // initializationOptions.semanticTokens (design doc 2026-09-25 K/§7, contract T0): native
    // module-syntax tokens are on by default; the custom `module` type and `partition` modifier
    // only for a client that says it knows them.
    bool semanticTokensModules { true };
    bool semanticTokensModuleType { false };
    // This program, to run `mcppls review` for an editor's review command (overall design 7.7).
    std::string serverExecutable;
    // Given by the composition root; a test can substitute engines that start no process.
    std::function<EngineFactories(const SessionOptions&, const engine::PayloadPaths&, bool payloadCorrupt)> engineFactories;
};

// A background thread (model loading, an engine's I/O threads, watch polling) reports back through
// this queue; the session's one event loop is the only thing that ever changes state.
// `external` carries a message of an entry other than LSP (an MCP request, say) through the same loop.
// `review_finished` carries the output of a review an editor asked for.
// `tool_run` carries what the external-program runner wrote down about one run, so the
// workspace it belongs to can journal it (design 4.6).
enum class EventKind { client_message, client_closed, engine_event, model_loaded, external, review_finished, tool_run };

struct Event {
    EventKind kind { EventKind::client_message };
    Json message;
    int generation { 0 };
    std::shared_ptr<project::ProjectModel> model;
    // Empty for client_message and client_closed, which the session dispatches by document path;
    // every other kind names the root it belongs to, and engine events the engine.
    std::string rootKey;
    std::string engineId;
};

using EventChannel = platform::Channel<Event>;

enum class State { starting, loading, preparing, ready, degraded, error };
std::string_view to_string(State state);

bool is_build_file(std::string_view name);
// The files a watch (dynamic or the W9.3 polling fallback) cares about under `root`.
std::map<std::string, platform::fs::FileStamp> watched_files_snapshot(std::string_view root);

class Workspace {
public:
    Workspace(std::string root, std::string key, SessionOptions options, engine::PayloadPaths payload, bool payloadCorrupt, bool kitEnabled,
              std::string compilerOverride, std::shared_ptr<EventChannel> events, ClientSink& client);
    ~Workspace();
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    const std::string& root() const { return root_; }
    const std::string& key() const { return key_; }
    // The folder's URI as the client named it, which cxxModules/status reports as project.root (S3 4).
    void set_client_uri(std::string uri);
    bool owns_path(std::string_view path) const;

    // ---- lifecycle ----------------------------------------------------------------
    // `onEngineSettled` fires once, with the merged server capabilities of this root's engines, when
    // the core engine finished its handshake or was found unavailable (right away without one).
    void start(Json clientParams, bool clientSupportsStatus, bool usePolling, std::function<void(Json)> onEngineSettled = {});
    void shut_down();
    // Nothing is sent to the client before its own initialize was answered.
    void allow_status_notifications();

    // ---- client-driven, already known to belong to this root ----------------------------
    void did_open(const Json& params);
    void did_change(const Json& message, const Json& params);
    void did_close(const Json& message, const Json& params);
    void did_save(const Json& message, const Json& params);
    // Only the entries of a workspace/didChangeWatchedFiles notification this root owns.
    void handle_watched_files(const Json& changes);
    void cancel(const Json& id);
    void route_client_request(const Json& message);
    // The client's response to a request one of this root's engines sent through to it.
    void handle_client_response(const EngineRequestKey& key, const Json& response);
    void forward_other_notification(const Json& message);

    // ---- cxxModules/* (S3) ----------------------------------------------------------
    Json graph() const;
    Json module_info(std::string_view name) const;
    Json module_info_at(std::string_view path, base::Position at) const;
    Json contexts() const;
    // What a report of a problem needs about this root (robustness design O3): model, plan, engines with their
    // own details, request statistics by method, and the recent events.
    Json report() const;
    // Replies to `id` itself, then replans if it changed anything.
    void set_context(const Json& id, std::string_view context);

    // ---- the review an editor asks for (overall design 7.7) ----------------------------
    // Runs `mcppls review` on this root in the background; its findings are published as
    // diagnostics with source "mcppls review" until cleared or replaced. False when one is running.
    bool start_review(const Json& arguments);
    void handle_review_finished(const Json& outcome);
    void handle_tool_run(const Json& record);
    // Reads the build description again, offline. What the user does about a needed download
    // happens outside this server, so the only way to learn it worked is to look again.
    void reload_build_description();
    void clear_review();

    // ---- background events ------------------------------------------------------------
    void handle_engine_event(std::string_view engineId, const Json& event);
    void handle_model_loaded(int generation, std::shared_ptr<project::ProjectModel> model, bool fromProducer = true);

    // ---- timers -----------------------------------------------------------------------
    std::optional<Clock::time_point> next_deadline() const;
    void handle_timers();

    // ---- what the AI capabilities read (overall design 7) --------------------------------
    const index::ModuleIndex& module_index() const;
    std::shared_ptr<const project::ProjectModel> project_model() const;   // null before the first model
    const normalize::EnginePlan& engine_plan() const;
    std::string canonical_path_of(std::string_view uri) const;
    bool model_loading() const;
    std::uint64_t snapshot_generation() const;
    // The open buffer of a file by its canonical path, if a client has it open.
    std::optional<std::string> document_text(std::string_view path) const;
    std::optional<engine::EngineStatus> core_engine_status() const;
    const std::string& cache_directory() const;
    bool trusted() const;
    const std::string& mcpp_executable() const;   // as configured; empty: found on PATH
    // The document version of the core engine's latest diagnostics for a client URI: -1 when the
    // engine did not say, nullopt when it published none since the document opened.
    std::optional<std::int64_t> core_diagnostics_version(std::string_view uri) const;
    // Whether the core engine takes requests of `method` for `path` (a unit left out of its database does not).
    bool core_engine_serves(std::string_view method, std::string_view path) const;

private:
    std::string root_;
    std::string key_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mcppls::orchestrator
