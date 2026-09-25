module mcppls.server.session;

import std;
import nlohmann.json;
import mcppls.os;
import mcppls.base.error;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.base.uri;
import mcppls.base.version;
import mcppls.platform.dirs;
import mcppls.platform.toolenv;
import mcppls.platform.toolrun;
import mcppls.platform.fs;
import mcppls.platform.stdio;
import mcppls.platform.task;
import mcppls.lsp.jsonrpc;
import mcppls.lsp.protocol;
import mcppls.engine.payload;
import mcppls.orchestrator.report;
import mcppls.orchestrator.client;
import mcppls.orchestrator.completion;
import mcppls.orchestrator.routing;
import mcppls.orchestrator.workspace;

namespace mcppls::server {

namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace log = base::log;
using orchestrator::Event;
using orchestrator::EventChannel;
using orchestrator::EventKind;
using orchestrator::Workspace;

// usable plan W9.1: one event loop feeding one or more WorkspaceRoots. Everything a root itself
// owns (project model, module index, plan, clangd engine, documents) moved to
// mcppls.server.workspace; this class is left with the client-facing LSP protocol (capability
// negotiation, initialize/shutdown/exit), workspace/didChangeWorkspaceFolders, and routing a
// document-bearing message to the root with the longest matching root prefix, or to the first
// root for the handful of S3 requests that name no document (documented in handle_modules_request_).
class Session {
private:
    SessionOptions options_;
    const std::chrono::steady_clock::time_point started_ { std::chrono::steady_clock::now() };
    std::shared_ptr<EventChannel> events_ { std::make_shared<EventChannel>() };

    // Editor side.
    bool initializeReceived_ { false };
    bool initializeAnswered_ { false };
    bool clientInitialized_ { false };
    bool shutdownRequested_ { false };
    bool exitRequested_ { false };
    Json clientInitializeId_;
    Json clientParams_;
    Json clientCapabilities_;
    std::string compilerOverride_;
    bool kitEnabled_ { true };
    std::int64_t nextServerRequest_ { 1 };
    bool usePolling_ { false };   // usable plan W9.3: decided once from the client's own capabilities

    // The payload (clangd, kit) is the same for every root; resolved and checked once (design 15.3, W9.4).
    engine::PayloadPaths payload_;
    bool payloadCorrupt_ { false };

    orchestrator::StdioSink client_;
    // usable plan W9.1: one project model and one set of engines per workspace root.
    std::vector<std::unique_ptr<Workspace>> roots_;

public:
    explicit Session(SessionOptions options) : options_ { std::move(options) } {}

    int run() {
        start_input_reader_();
        while (!exitRequested_) {
            const auto deadline = next_deadline_();
            std::optional<Event> event { deadline ? events_->pop_until(*deadline) : events_->pop() };
            if (event) handle_(*event);
            handle_timers_();
        }
        for (auto& root : roots_) root->shut_down();
        return shutdownRequested_ ? 0 : 1;
    }

private:
    // ---- plumbing ---------------------------------------------------------

    void start_input_reader_() {
        std::thread { [events = events_] {
            lsp::FrameReader reader;
            while (true) {
                auto chunk = platform::stdio::read_input();
                if (!chunk || chunk->empty()) break;
                reader.feed(*chunk);
                while (auto message = reader.next()) {
                    if (!*message) {
                        log::warning("dropped a malformed frame from the client: {}", message->error().message);
                        continue;
                    }
                    events->push(Event { EventKind::client_message, std::move(**message) });
                }
            }
            events->push(Event { EventKind::client_closed });
        } }.detach();
    }

    void reply_(const Json& id, Json result) { client_.reply(id, std::move(result)); }
    void reply_error_(const Json& id, int code, std::string_view message) { client_.reply_error(id, code, message); }

    static std::string uri_of_params_(const Json& params) {
        const Json* uri { lsp::find_path(params, { "textDocument", "uri" }) };
        return uri != nullptr && uri->is_string() ? uri->get<std::string>() : std::string {};
    }

    // Session's own routing needs a canonical path from a URI, same as every WorkspaceRoot's own
    // (uncached: unlike a root's, this is not on the path of every engine message, only one lookup
    // per incoming client message, to decide which root that message belongs to).
    static std::string canonical_path_of_uri_(std::string_view uri) {
        auto path = base::uri_to_path(uri);
        return path ? platform::fs::canonical_path(*path) : std::string {};
    }

    Workspace* root_by_key_(const std::string& key) const {
        auto it = std::ranges::find_if(roots_, [&](const auto& root) { return root->key() == key; });
        return it == roots_.end() ? nullptr : it->get();
    }

    // The root with the longest matching prefix of `path` (usable plan W9.1); the first root when
    // `path` is empty or matches none (a document outside every known folder, or an S3 request
    // that names no document at all).
    Workspace* root_for_path_(std::string_view path) const {
        Workspace* best { nullptr };
        for (const auto& root : roots_) {
            if (!path.empty() && root->owns_path(path) && (!best || root->root().size() > best->root().size())) best = root.get();
        }
        if (best) return best;
        return roots_.empty() ? nullptr : roots_.front().get();
    }

    Workspace* root_for_message_(const Json& params) const {
        const std::string uri { uri_of_params_(params) };
        return root_for_path_(uri.empty() ? std::string {} : canonical_path_of_uri_(uri));
    }

    std::optional<Clock::time_point> next_deadline_() const {
        std::optional<Clock::time_point> deadline;
        for (const auto& root : roots_) {
            if (auto at = root->next_deadline(); at && (!deadline || *at < *deadline)) deadline = at;
        }
        return deadline;
    }

    // ---- dispatch -----------------------------------------------------------

    void handle_(Event& event) {
        switch (event.kind) {
        case EventKind::client_message: handle_client_(event.message); break;
        case EventKind::client_closed:
            log::info("the client closed its input");
            exitRequested_ = true;
            break;
        case EventKind::engine_event:
            if (auto* root = root_by_key_(event.rootKey)) root->handle_engine_event(event.engineId, event.message);
            break;
        case EventKind::model_loaded:
            if (auto* root = root_by_key_(event.rootKey)) {
                // A default-constructed message is null, not an object: the producer's own load carries none.
                const bool fromProducer { !event.message.is_object()
                                          || event.message.value("origin", std::string { "producer" }) == "producer" };
                root->handle_model_loaded(event.generation, std::move(event.model), fromProducer);
            }
            break;
        case EventKind::tool_run:
            if (auto* root = root_by_key_(event.rootKey)) root->handle_tool_run(event.message);
            break;
        case EventKind::external: break;   // no other entry shares an LSP session's loop
        case EventKind::review_finished:
            if (auto* root = root_by_key_(event.rootKey)) root->handle_review_finished(event.message);
            break;
        }
    }

    void handle_client_(const Json& message) {
        switch (lsp::kind_of(message)) {
        case lsp::Kind::request: handle_client_request_(message); break;
        case lsp::Kind::notification: handle_client_notification_(message); break;
        case lsp::Kind::response: handle_client_response_(message); break;
        case lsp::Kind::invalid: log::warning("ignored an invalid message from the client"); break;
        }
    }

    // ---- client requests ----------------------------------------------------

    void handle_client_request_(const Json& message) {
        const Json& id { message["id"] };
        const std::string method { message.value("method", std::string {}) };
        const Json params = message.contains("params") ? message["params"] : Json::object();
        if (method == lsp::method::INITIALIZE) {
            handle_initialize_(id, params);
            return;
        }
        if (!initializeReceived_) {
            reply_error_(id, lsp::SERVER_NOT_INITIALIZED, "the server is not initialized");
            return;
        }
        if (shutdownRequested_ && method != lsp::method::SHUTDOWN) {
            reply_error_(id, lsp::INVALID_REQUEST, "the server is shutting down");
            return;
        }
        if (method == lsp::method::SHUTDOWN) {
            handle_shutdown_(id);
            return;
        }
        if (method.starts_with("cxxModules/")) {
            handle_modules_request_(id, method, params);
            return;
        }
        // Build description design 4.4: the build description needed a download, the user has gone
        // and fetched it, and the window is theirs again. Reading it again is cheap and offline.
        if (method == lsp::method::WORKSPACE_EXECUTE_COMMAND && params.value("command", std::string {}) == "mcppls.reloadBuildDescription") {
            for (auto& root : roots_) root->reload_build_description();
            reply_(id, nullptr);
            return;
        }
        // overall design 7.7: the review of the workspace's changes, run in the background, its findings published as diagnostics.
        if (method == lsp::method::WORKSPACE_EXECUTE_COMMAND && params.value("command", std::string {}).starts_with("mcppls.review.")) {
            const std::string command { params.value("command", std::string {}) };
            const Json arguments = params.value("arguments", Json::array());
            for (auto& root : roots_) {
                if (command == "mcppls.review.run") (void)root->start_review(arguments);
                else if (command == "mcppls.review.clear") root->clear_review();
            }
            reply_(id, nullptr);
            return;
        }
        if (auto* root = root_for_message_(params)) root->route_client_request(message);
        else reply_(id, nullptr);
    }

    void handle_initialize_(const Json& id, const Json& params) {
        if (initializeReceived_) {
            reply_error_(id, lsp::INVALID_REQUEST, "initialize was already received");
            return;
        }
        initializeReceived_ = true;
        clientInitializeId_ = id;
        clientParams_ = params;
        clientCapabilities_ = params.value("capabilities", Json::object());
        if (const Json* init = lsp::find(params, "initializationOptions"); init != nullptr && init->is_object()) {
            if (auto compiler = lsp::string_at(*init, "compiler")) compilerOverride_ = *compiler;
            if (auto kit = lsp::string_at(*init, "semanticKit")) kitEnabled_ = *kit != "off";
            // overall design 5.6: the core engine a client chose (mcppls.engine), unless the command line named one.
            if (auto chosen = lsp::string_at(*init, "engine"); chosen && !chosen->empty() && !options_.engineFromCommandLine) options_.engine = *chosen;
            // design 4.4 and 4.3. Anything else is the default: a setting nobody here understands
            // must not silently turn the network on or take the environment away.
            if (auto buildTool = lsp::string_at(*init, "buildTool");
                buildTool && (*buildTool == "offline" || *buildTool == "online" || *buildTool == "off")) {
                options_.buildTool = *buildTool;
            }
            if (auto environment = lsp::string_at(*init, "toolEnvironment");
                environment && (*environment == "auto" || *environment == "editor")) {
                options_.toolEnvironment = *environment;
            }
            // design doc 2026-09-25 K/§7, contract T0: initializationOptions.semanticTokens.
            if (const Json* semanticTokens = lsp::find(*init, "semanticTokens"); semanticTokens != nullptr && semanticTokens->is_object()) {
                if (const auto modules = semanticTokens->find("modules"); modules != semanticTokens->end() && modules->is_boolean()) {
                    options_.semanticTokensModules = modules->get<bool>();
                }
                if (const auto moduleType = semanticTokens->find("moduleType"); moduleType != semanticTokens->end() && moduleType->is_boolean()) {
                    options_.semanticTokensModuleType = moduleType->get<bool>();
                }
            }
        }
        // The environment the user's build tools run in is resolved once, in the background, before
        // anything needs it (design 4.3): an editor started from a desktop entry has none of the
        // user's shell configuration, and a build tool found through the wrong PATH is another build.
        platform::toolenv::configure(platform::toolenv::parse_mode(options_.toolEnvironment).value_or(platform::toolenv::Mode::automatic),
                                     options_.serverExecutable);
        platform::toolenv::begin();
        // Every external run reaches the journal of the workspace it belongs to (design 4.6). The
        // runner records from whatever thread ran the program; the event loop is the only place
        // that touches a workspace, so it travels as an event like everything else does.
        platform::toolrun::set_recorder([events = events_](const platform::toolrun::Record& record) {
            if (record.root.empty()) return;
            events->push(Event { EventKind::tool_run, platform::toolrun::to_json(record), 0, nullptr, record.root });
        });

        // usable plan W9.4: resolved and checked once, before any root trusts either with anything.
        payload_ = engine::resolve_payload(engine::PayloadRequest { options_.payloadDirectory, options_.clangd, options_.kit, options_.engine });
        {
            const std::string integrityCache { base::join_path(platform::dirs::cache_directory(), "payload-integrity.json") };
            for (const auto& problem : engine::verify_payload_integrity(payload_, integrityCache)) {
                log::error("payload integrity: {} {}", problem.path, problem.reason);
                payloadCorrupt_ = true;
            }
        }

        // usable plan W9.3: decided once, for every root this session ever creates (including ones
        // workspace/didChangeWorkspaceFolders adds later).
        const Json* dynamic { lsp::find_path(clientCapabilities_, { "workspace", "didChangeWatchedFiles", "dynamicRegistration" }) };
        usePolling_ = !(dynamic != nullptr && dynamic->is_boolean() && dynamic->get<bool>());

        for (const auto& folder : workspace_roots_(params)) create_root_(folder, roots_.empty());
        if (roots_.empty()) answer_initialize_(orchestrator::merge_capabilities(Json::object()));   // workspace_roots_ always names at least one
    }

    // A workspace folder: the path it names, and the URI the client named it by.
    struct Folder {
        std::string path;
        std::string uri;
    };

    static std::optional<Folder> folder_of_uri_(const std::string& uri) {
        auto path = base::uri_to_path(uri);
        if (!path) return std::nullopt;
        return Folder { *path, uri };
    }

    // Every folder `initialize` names, or the single root a pre-3.6 client implies (design 13.1
    // predates multi-root; usable plan W9.1 is additive over it).
    std::vector<Folder> workspace_roots_(const Json& params) const {
        std::vector<Folder> found;
        if (const Json* folders = lsp::find(params, "workspaceFolders"); folders != nullptr && folders->is_array()) {
            for (const auto& folder : *folders) {
                if (auto uri = lsp::string_at(folder, "uri")) {
                    if (auto named = folder_of_uri_(*uri)) found.push_back(std::move(*named));
                }
            }
            if (!found.empty()) return found;
        }
        if (auto uri = lsp::string_at(params, "rootUri")) {
            if (auto named = folder_of_uri_(*uri)) return { std::move(*named) };
        }
        if (auto path = lsp::string_at(params, "rootPath"); path && !path->empty()) {
            const std::string normalized { base::normalize_path(*path) };
            return { Folder { normalized, base::path_to_uri(normalized) } };
        }
        const std::string current { platform::fs::current_directory() };
        return { Folder { current, base::path_to_uri(current) } };
    }

    // `isFirst` wires the session's own `initialize` response to this root's engine settling
    // (design 13.1's timing, kept for the first root when there are several).
    void create_root_(const Folder& folder, bool isFirst) {
        const std::string root { platform::fs::canonical_path(folder.path) };
        if (std::ranges::any_of(roots_, [&](const auto& existing) { return existing->root() == root; })) return;
        auto created = std::make_unique<Workspace>(root, root /* key: a root's own path is already unique */, options_, payload_,
                                                   payloadCorrupt_, kitEnabled_, compilerOverride_, events_, client_);
        Workspace* handle { created.get() };
        // S3 4: status names the root by the client's own URI, which a symbolic link, an 8.3 short
        // name or a different spelling would otherwise make differ from the canonical path used inside.
        handle->set_client_uri(folder.uri);
        roots_.push_back(std::move(created));
        std::function<void(Json)> onEngineSettled;
        if (isFirst) onEngineSettled = [this](Json capabilities) { answer_initialize_(capabilities); };
        handle->start(clientParams_, orchestrator::client_supports(clientCapabilities_, "status"), usePolling_, std::move(onEngineSettled));
        // The client's own initialize was answered before this root even existed (added later by
        // workspace/didChangeWorkspaceFolders, or created after an earlier root in this same batch
        // settled synchronously and so answered it already): unlock its status notifications now,
        // since answer_initialize_ below will never see this root to do it.
        if (initializeAnswered_) handle->allow_status_notifications();
    }

    void answer_initialize_(const Json& capabilities) {
        if (initializeAnswered_) return;
        initializeAnswered_ = true;
        Json result {
            { "capabilities", capabilities.empty() ? orchestrator::merge_capabilities(Json::object()) : capabilities },
            { "serverInfo", Json { { "name", "mcppls" }, { "version", std::string { base::VERSION } } } },
        };
        // F9 (D4 layer 4): a space opens the module list after `import`, for a client that drops the
        // other spaces itself (VS Code's middleware) or asked for it (completion.triggerOnSpace).
        if (orchestrator::completion::space_trigger_wanted(clientParams_)) orchestrator::completion::add_space_trigger(result["capabilities"]);
        reply_(clientInitializeId_, std::move(result));
        for (auto& root : roots_) root->allow_status_notifications();
    }

    void handle_shutdown_(const Json& id) {
        // Every root's engine is told (politely, then not) in run()'s cleanup once exit follows,
        // exactly as a single engine's shutdown+exit pair used to be, so this needs only reply.
        shutdownRequested_ = true;
        reply_(id, nullptr);
    }

    void handle_modules_request_(const Json& id, std::string_view method, const Json& params) {
        if (method == "cxxModules/graph") {
            // usable plan W9.1: S3's graph request carries no per-root parameter (documented
            // limitation); the first root answers, as the only root always did.
            Workspace* root { roots_.empty() ? nullptr : roots_.front().get() };
            reply_(id, root ? root->graph() : Json(nullptr));
        } else if (method == "cxxModules/moduleInfo") {
            if (auto name = lsp::string_at(params, "name")) {
                Workspace* root { roots_.empty() ? nullptr : roots_.front().get() };
                reply_(id, root ? root->module_info(*name) : Json(nullptr));
                return;
            }
            const std::string uri { uri_of_params_(params) };
            const std::string path { uri.empty() ? std::string {} : canonical_path_of_uri_(uri) };
            const Json* position { lsp::find(params, "position") };
            if (path.empty() || position == nullptr) {
                reply_(id, nullptr);
                return;
            }
            const base::Position at { static_cast<int>(lsp::int_at(*position, "line").value_or(0)),
                                      static_cast<int>(lsp::int_at(*position, "character").value_or(0)) };
            Workspace* root { root_for_path_(path) };
            reply_(id, root ? root->module_info_at(path, at) : Json(nullptr));
        } else if (method == "cxxModules/report") {
            // robustness design O3: what a bug report needs, for every root.
            Json roots = Json::array();
            for (const auto& root : roots_) roots.push_back(root->report());
            const Json* clientInfo { lsp::find(clientParams_, "clientInfo") };
            reply_(id, orchestrator::make_report(std::move(roots), clientInfo != nullptr ? *clientInfo : Json(nullptr), options_.engine, payload_, payloadCorrupt_,
                                                 std::chrono::steady_clock::now() - started_));
        } else if (method == "cxxModules/contexts") {
            Workspace* root { root_for_message_(params) };
            reply_(id, root ? root->contexts() : Json(nullptr));
        } else if (method == "cxxModules/setContext") {
            Workspace* root { root_for_message_(params) };
            if (root == nullptr) {
                reply_error_(id, lsp::INVALID_PARAMS, "no workspace root");
                return;
            }
            root->set_context(id, params.value("context", std::string { "default" }));
        } else {
            reply_error_(id, lsp::METHOD_NOT_FOUND, std::format("unknown request {}", method));
        }
    }

    // ---- client notifications -------------------------------------------------

    void handle_client_notification_(const Json& message) {
        const std::string method { message.value("method", std::string {}) };
        const Json params = message.contains("params") ? message["params"] : Json::object();
        if (method == lsp::method::EXIT) {
            // Each root's engine is told politely, then stopped outright, in run()'s cleanup once
            // the loop notices exitRequested_ (also reached via the client simply closing its
            // input, which sends no exit notification at all).
            exitRequested_ = true;
            return;
        }
        if (!initializeReceived_) return;
        if (method == lsp::method::INITIALIZED) {
            // Each root's own engine `initialized` is sent when its initialize result arrives.
            clientInitialized_ = true;
            register_watchers_();
            return;
        }
        if (method == lsp::method::TEXT_DOCUMENT_DID_OPEN) {
            if (auto* root = root_for_message_(params)) root->did_open(params);
            return;
        }
        if (method == lsp::method::TEXT_DOCUMENT_DID_CHANGE) {
            if (auto* root = root_for_message_(params)) root->did_change(message, params);
            return;
        }
        if (method == lsp::method::TEXT_DOCUMENT_DID_CLOSE) {
            if (auto* root = root_for_message_(params)) root->did_close(message, params);
            return;
        }
        if (method == lsp::method::TEXT_DOCUMENT_DID_SAVE) {
            if (auto* root = root_for_message_(params)) root->did_save(message, params);
            return;
        }
        if (method == lsp::method::WORKSPACE_DID_CHANGE_WATCHED_FILES) {
            dispatch_watched_files_(params);
            return;
        }
        if (method == lsp::method::WORKSPACE_DID_CHANGE_WORKSPACE_FOLDERS) {
            handle_did_change_workspace_folders_(params);
            return;
        }
        if (method == lsp::method::CANCEL_REQUEST) {
            // A client-issued id does not say which root it was sent to; every root's own
            // pending/deferred lists are harmlessly unaffected if it is not theirs.
            const Json requestId = params.value("id", Json {});
            for (auto& root : roots_) root->cancel(requestId);
            return;
        }
        if (method == lsp::method::WORKSPACE_DID_CHANGE_CONFIGURATION || method == lsp::method::SET_TRACE || !method.starts_with("$/")) {
            for (auto& root : roots_) root->forward_other_notification(message);
        }
    }

    // Splits a (possibly multi-root) notification by the longest root prefix of each entry's path,
    // so did_change_watched_files_-equivalent handling runs once per root with only its own entries
    // (usable plan W9.1); a polling root's own notification already carries only its own entries,
    // so it round-trips through this unchanged.
    void dispatch_watched_files_(const Json& params) {
        std::map<Workspace*, Json> perRoot;
        for (const auto& change : params.value("changes", Json::array())) {
            // Not `Json uriValue { change.value(...) }`: brace-initializing a Json from a Json makes a
            // one-element array, which left every change without a path and so sent it to the first root.
            const auto uriValue = change.find("uri");
            const std::string uri { uriValue != change.end() && uriValue->is_string() ? uriValue->get<std::string>() : std::string {} };
            const std::string path { uri.empty() ? std::string {} : canonical_path_of_uri_(uri) };
            Workspace* root { root_for_path_(path) };
            if (root == nullptr) continue;
            auto& bucket = perRoot[root];
            if (!bucket.is_array()) bucket = Json::array();
            bucket.push_back(change);
        }
        for (auto& [root, changes] : perRoot) root->handle_watched_files(changes);
    }

    void handle_did_change_workspace_folders_(const Json& params) {
        const Json* event { lsp::find(params, "event") };
        if (event == nullptr) return;
        for (const auto& removed : event->value("removed", Json::array())) {
            auto uri = lsp::string_at(removed, "uri");
            if (!uri) continue;
            auto path = base::uri_to_path(*uri);
            if (!path) continue;
            const std::string root { platform::fs::canonical_path(*path) };
            auto it = std::ranges::find_if(roots_, [&](const auto& existing) { return existing->root() == root; });
            if (it == roots_.end()) continue;
            (*it)->shut_down();
            roots_.erase(it);
        }
        for (const auto& added : event->value("added", Json::array())) {
            auto uri = lsp::string_at(added, "uri");
            if (!uri) continue;
            if (auto folder = folder_of_uri_(*uri)) create_root_(*folder, false);
        }
    }

    void handle_client_response_(const Json& message) {
        const Json& id { message["id"] };
        if (!id.is_string()) return;
        const auto key = orchestrator::parse_engine_request_key(id.get<std::string>());
        if (!key) return;   // a response to this server's own request
        if (auto* root = root_by_key_(key->rootKey)) root->handle_client_response(*key, message);
    }

    void register_watchers_() {
        // usable plan W9.3: no dynamic registration means every root already started its own
        // polling worker in create_root_.
        if (usePolling_) return;
        Json watchers = Json::array();
        for (std::string_view glob : { "**/mcpp.toml", "**/mcpp.lock", "**/CMakeLists.txt", "**/CMakePresets.json",
                                       "**/compile_commands.json", "**/build_database.json",
                                       "**/*.{cppm,ccm,cxxm,c++m,ixx,mpp,mxx,cpp,cc,cxx}" }) {
            watchers.push_back(Json { { "globPattern", std::string { glob } } });
        }
        Json params { { "registrations", Json::array({ Json { { "id", "mcppls-watched-files" },
                                                              { "method", "workspace/didChangeWatchedFiles" },
                                                              { "registerOptions", Json { { "watchers", watchers } } } } }) } };
        client_.send(lsp::make_request(std::format("s:{}", nextServerRequest_++), "client/registerCapability", std::move(params)));
    }

    // ---- timers -----------------------------------------------------------------------

    void handle_timers_() {
        for (auto& root : roots_) root->handle_timers();
    }
};

} // namespace

int run_session(const SessionOptions& options) {
    Session session { options };
    return session.run();
}

} // namespace mcppls::server
