module mcppls.orchestrator.kernel;

import std;
import nlohmann.json;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.uri;
import mcppls.platform.dirs;
import mcppls.platform.fs;
import mcppls.platform.toolenv;
import mcppls.platform.toolrun;
import mcppls.platform.task;
import mcppls.lsp.jsonrpc;
import mcppls.engine.payload;
import mcppls.orchestrator.client;
import mcppls.orchestrator.workspace;

namespace mcppls::orchestrator {

namespace {

namespace log = base::log;

// What a client of this kind tells the engines it understands: enough for answers an agent can read
// (markdown hover, hierarchical outlines, related information on diagnostics) and progress reports.
Json initialize_params(const std::string& root) {
    const std::string uri { base::path_to_uri(root) };
    // Every symbol kind of LSP 3.17; without them clangd reports a struct as a class.
    Json symbolKinds = Json::array();
    for (int kind { 1 }; kind <= 26; ++kind) symbolKinds.push_back(kind);
    const Json symbolKind { { "valueSet", symbolKinds } };
    Json textDocument {
        { "hover", Json { { "contentFormat", Json::array({ "markdown", "plaintext" }) } } },
        { "documentSymbol", Json { { "hierarchicalDocumentSymbolSupport", true }, { "symbolKind", symbolKind } } },
        { "publishDiagnostics", Json { { "relatedInformation", true } } },
        { "definition", Json { { "linkSupport", false } } },
        { "references", Json::object() },
        { "callHierarchy", Json::object() },
        { "typeHierarchy", Json::object() },
        { "completion", Json { { "completionItem", Json { { "snippetSupport", false } } } } },
    };
    Json workspace {
        { "symbol", Json { { "symbolKind", symbolKind } } },
        { "didChangeWatchedFiles", Json { { "dynamicRegistration", false } } },
        { "workspaceFolders", true },
        { "configuration", false },
    };
    Json capabilities {
        { "textDocument", std::move(textDocument) },
        { "workspace", std::move(workspace) },
        { "window", Json { { "workDoneProgress", true } } },
        { "experimental", Json { { "cxxModules", Json { { "status", true }, { "graph", true }, { "contexts", true } } } } },
    };
    return Json {
        { "processId", nullptr },
        { "rootUri", uri },
        { "rootPath", root },
        { "workspaceFolders", Json::array({ Json { { "uri", uri }, { "name", std::string { base::file_name(root) } } } }) },
        { "capabilities", std::move(capabilities) },
        { "clientInfo", Json { { "name", "mcppls-kernel" } } },
    };
}

// The kernel's own answer to a request an engine sent to its client.
Json answer_for(const Json& request) {
    const std::string method { request.value("method", std::string {}) };
    if (method == "workspace/configuration") {
        const Json* items { lsp::find_path(request, { "params", "items" }) };
        Json values = Json::array();
        if (items != nullptr && items->is_array()) {
            for (std::size_t i { 0 }; i < items->size(); ++i) values.push_back(nullptr);
        }
        return values;
    }
    if (method == "workspace/applyEdit") return Json { { "applied", false }, { "failureReason", "a headless session applies no edits" } };
    return nullptr;
}

} // namespace

// The kernel as the workspace's client: everything the workspace sends lands here, on the loop's own
// thread (the workspace only ever sends from inside a call the loop made).
class CaptureSink final : public ClientSink {
public:
    std::map<std::string, Json> responses;          // by the kernel's own request id
    std::map<std::string, Json> diagnostics;        // latest list by URI
    std::map<std::string, int> diagnosticsCounts;   // publications by URI
    Json status;
    std::set<std::string> progress;                 // tokens with work begun and not ended
    int progressBegun { 0 };
    std::deque<Json> engineRequests;                // requests to the client, answered on the loop
    std::set<std::string> abandoned;                // requests that timed out: a late answer is dropped

    void send(const Json& message) override {
        switch (lsp::kind_of(message)) {
        case lsp::Kind::response: {
            const Json& id { message["id"] };
            if (!id.is_string() || abandoned.erase(id.get<std::string>()) > 0) break;
            responses[id.get<std::string>()] = message;
            break;
        }
        case lsp::Kind::notification: note(message); break;
        case lsp::Kind::request: engineRequests.push_back(message); break;
        case lsp::Kind::invalid: break;
        }
    }

private:
    static std::string token_of(const Json& params) {
        const Json* token { lsp::find(params, "token") };
        if (token == nullptr) return {};
        return token->is_string() ? token->get<std::string>() : lsp::dump(*token);
    }

    void note(const Json& message) {
        const std::string method { message.value("method", std::string {}) };
        const Json params = message.contains("params") ? message["params"] : Json::object();
        if (method == "textDocument/publishDiagnostics") {
            const std::string uri { params.value("uri", std::string {}) };
            diagnostics[uri] = params.value("diagnostics", Json::array());
            ++diagnosticsCounts[uri];
        } else if (method == "cxxModules/status") {
            status = params;
        } else if (method == "$/progress") {
            const Json* kind { lsp::find_path(params, { "value", "kind" }) };
            if (kind == nullptr || !kind->is_string()) return;
            if (*kind == "begin") {
                progress.insert(token_of(params));
                ++progressBegun;
            } else if (*kind == "end") {
                progress.erase(token_of(params));
            }
        }
    }
};

struct Kernel::Impl {
    KernelOptions options;
    std::string root;
    std::shared_ptr<EventChannel> events { std::make_shared<EventChannel>() };
    // The same two as the LSP session (design 4.3, 4.6): a review, a report and the daemon run the
    // user's build tools too, in the user's environment, and what they cost is written down.
    struct ToolWiring {
        explicit ToolWiring(std::shared_ptr<EventChannel> channel, const SessionOptions& session) {
            platform::toolenv::configure(platform::toolenv::parse_mode(session.toolEnvironment).value_or(platform::toolenv::Mode::automatic),
                                         session.serverExecutable);
            platform::toolenv::begin();
            platform::toolrun::set_recorder([channel](const platform::toolrun::Record& record) {
                if (record.root.empty()) return;
                channel->push(Event { EventKind::tool_run, platform::toolrun::to_json(record), 0, nullptr, record.root });
            });
        }
        ~ToolWiring() { platform::toolrun::set_recorder({}); }
    };
    // Emplaced by start(), once `options` is filled in: a default member initializer here would
    // read an empty SessionOptions and the login shell would never be asked.
    std::optional<ToolWiring> toolWiring;
    CaptureSink sink;
    std::unique_ptr<Workspace> workspace;
    std::int64_t nextRequest { 1 };
    struct OpenDocument {
        std::string path;
        std::int64_t version { 1 };
        std::string text;          // what the engines see
        bool overlay { false };    // `text` was given, not read from the file
        std::uint64_t lastUse { 0 };
    };
    std::map<std::string, OpenDocument> documents;   // by URI
    std::uint64_t useClock { 0 };
    std::deque<Json> external;
    bool externalClosed { false };
    bool shutDown { false };

    void handle(Event& event) {
        switch (event.kind) {
        case EventKind::engine_event:
            if (event.rootKey == workspace->key()) workspace->handle_engine_event(event.engineId, event.message);
            break;
        case EventKind::model_loaded:
            if (event.rootKey == workspace->key()) {
                // A default-constructed message is null, not an object: the producer's own load carries none.
                const bool fromProducer { !event.message.is_object()
                                          || event.message.value("origin", std::string { "producer" }) == "producer" };
                workspace->handle_model_loaded(event.generation, std::move(event.model), fromProducer);
            }
            break;
        case EventKind::client_message:
            // The polling watch reports changed files as the notification an editor would have sent.
            if (event.message.value("method", std::string {}) == "workspace/didChangeWatchedFiles") {
                const Json* changes { lsp::find_path(event.message, { "params", "changes" }) };
                if (changes != nullptr && changes->is_array()) workspace->handle_watched_files(*changes);
            }
            break;
        case EventKind::client_closed: break;
        case EventKind::tool_run:
            if (event.rootKey == workspace->key()) workspace->handle_tool_run(event.message);
            break;
        case EventKind::external:
            if (event.message.is_null()) externalClosed = true;
            else external.push_back(std::move(event.message));
            break;
        }
    }

    void answer_engine_requests() {
        while (!sink.engineRequests.empty()) {
            Json request = std::move(sink.engineRequests.front());
            sink.engineRequests.pop_front();
            // A token's work begins with its first $/progress, so window/workDoneProgress/create needs only its answer.
            const Json& id { request["id"] };
            if (!id.is_string()) continue;
            const auto key = parse_engine_request_key(id.get<std::string>());
            if (!key) continue;   // the workspace's own registrations need no answer here
            Json response { { "jsonrpc", "2.0" }, { "id", id }, { "result", answer_for(request) } };
            workspace->handle_client_response(*key, response);
        }
    }

    // One turn of the loop: an event or the next timer, whichever comes first before `until`.
    void pump(Clock::time_point until) {
        answer_engine_requests();
        const auto deadline = workspace->next_deadline();
        const auto wake = deadline && *deadline < until ? *deadline : until;
        if (auto event = events->pop_until(wake)) handle(*event);
        workspace->handle_timers();
        answer_engine_requests();
    }

    void send_change(const std::string& uri, OpenDocument& document, std::string text) {
        document.text = text;
        const std::int64_t version { ++document.version };
        Json params { { "textDocument", Json { { "uri", uri }, { "version", version } } }, { "contentChanges", Json::array({ Json { { "text", std::move(text) } } }) } };
        Json message = lsp::make_notification("textDocument/didChange", params);
        workspace->did_change(message, params);
        answer_engine_requests();
    }

    void close_document(const std::string& uri) {
        if (!documents.erase(uri)) return;
        Json params { { "textDocument", Json { { "uri", uri } } } };
        Json message = lsp::make_notification("textDocument/didClose", params);
        workspace->did_close(message, params);
        answer_engine_requests();
    }

    void close_least_recently_used() {
        while (documents.size() >= MAX_OPEN_DOCUMENTS) {
            auto oldest = std::ranges::min_element(documents, {}, [](const auto& entry) { return entry.second.lastUse; });
            close_document(std::string { oldest->first });
        }
    }

    std::string absolute(std::string_view path) const {
        if (path.starts_with("file:")) {
            if (auto named = base::uri_to_path(path)) return base::normalize_path(*named);
            return {};
        }
        if (base::is_absolute_path(path)) return base::normalize_path(path);
        return base::normalize_path(base::join_path(root, path));
    }
};

std::unique_ptr<Kernel> Kernel::start(const KernelOptions& options) {
    std::unique_ptr<Kernel> kernel { new Kernel };
    kernel->impl_ = std::make_unique<Impl>();
    Impl& impl = *kernel->impl_;
    impl.options = options;
    impl.root = platform::fs::canonical_path(options.root.empty() ? platform::fs::current_directory() : options.root);
    impl.toolWiring.emplace(impl.events, impl.options.session);

    const SessionOptions& session { options.session };
    engine::PayloadPaths payload { engine::resolve_payload(engine::PayloadRequest { session.payloadDirectory, session.clangd, session.kit, session.engine }) };
    bool payloadCorrupt { false };
    const std::string integrityCache { base::join_path(platform::dirs::cache_directory(), "payload-integrity.json") };
    for (const auto& problem : engine::verify_payload_integrity(payload, integrityCache)) {
        log::error("payload integrity: {} {}", problem.path, problem.reason);
        payloadCorrupt = true;
    }

    impl.workspace = std::make_unique<Workspace>(impl.root, impl.root, session, std::move(payload), payloadCorrupt, true, std::string {}, impl.events, impl.sink);
    impl.workspace->set_client_uri(base::path_to_uri(impl.root));
    impl.workspace->start(initialize_params(impl.root), true, true);
    impl.workspace->allow_status_notifications();
    return kernel;
}

Kernel::~Kernel() {
    if (impl_) shut_down();
}

Workspace& Kernel::workspace() { return *impl_->workspace; }

const std::string& Kernel::root() const { return impl_->root; }

std::optional<Json> Kernel::request(std::string_view method, Json params, std::chrono::milliseconds timeout) {
    Impl& impl = *impl_;
    const std::string id { std::format("k:{}", impl.nextRequest++) };
    Json message = lsp::make_request(id, method, std::move(params));
    impl.workspace->route_client_request(message);
    const auto until = Clock::now() + timeout;
    while (true) {
        if (auto found = impl.sink.responses.find(id); found != impl.sink.responses.end()) {
            Json response = std::move(found->second);
            impl.sink.responses.erase(found);
            return response;
        }
        if (Clock::now() >= until) break;
        impl.pump(until);
    }
    impl.workspace->cancel(Json(id));
    // An engine can still answer a request it was told to cancel; that answer is dropped when it comes,
    // so a long-lived session (the daemon's) does not keep one per timeout.
    impl.sink.responses.erase(id);
    if (impl.sink.abandoned.size() > 4096) impl.sink.abandoned.clear();
    impl.sink.abandoned.insert(id);
    return std::nullopt;
}

std::string Kernel::absolute_path(std::string_view path) const { return impl_->absolute(path); }

std::string Kernel::uri_of(std::string_view path) const { return base::path_to_uri(impl_->absolute(path)); }

void Kernel::open(std::string_view path) {
    Impl& impl = *impl_;
    const std::string uri { uri_of(path) };
    if (auto found = impl.documents.find(uri); found != impl.documents.end()) {
        found->second.lastUse = ++impl.useClock;
        return;
    }
    const std::string absolute { impl.absolute(path) };
    auto text = platform::fs::read_file(absolute);
    if (!text) return;
    impl.close_least_recently_used();
    impl.documents[uri] = Impl::OpenDocument { absolute, 1, *text, false, ++impl.useClock };
    impl.workspace->did_open(Json { { "textDocument", Json { { "uri", uri }, { "languageId", "cpp" }, { "version", 1 }, { "text", std::move(*text) } } } });
    impl.answer_engine_requests();
}

void Kernel::change(std::string_view path, std::string text) {
    Impl& impl = *impl_;
    const std::string uri { uri_of(path) };
    auto found = impl.documents.find(uri);
    if (found == impl.documents.end()) {
        impl.close_least_recently_used();
        impl.documents[uri] = Impl::OpenDocument { impl.absolute(path), 1, text, true, ++impl.useClock };
        impl.workspace->did_open(Json { { "textDocument", Json { { "uri", uri }, { "languageId", "cpp" }, { "version", 1 }, { "text", std::move(text) } } } });
        impl.answer_engine_requests();
        return;
    }
    found->second.overlay = true;
    found->second.lastUse = ++impl.useClock;
    impl.send_change(uri, found->second, std::move(text));
}

void Kernel::close(std::string_view path) {
    impl_->close_document(uri_of(path));
}

void Kernel::touch(std::string_view path) {
    Impl& impl = *impl_;
    const std::string uri { uri_of(path) };
    const auto found = impl.documents.find(uri);
    if (found == impl.documents.end()) return;
    // clangd does not build a document again for a new version with the same content: it is closed
    // and opened again, with what it had.
    // Versions keep growing, so diagnostics of the document before it was closed are never taken for its own.
    const Impl::OpenDocument document { found->second };
    const std::int64_t version { document.version + 1 };
    impl.close_document(uri);
    impl.documents[uri] = Impl::OpenDocument { document.path, version, document.text, document.overlay, ++impl.useClock };
    impl.workspace->did_open(Json { { "textDocument", Json { { "uri", uri }, { "languageId", "cpp" }, { "version", version }, { "text", document.text } } } });
    impl.answer_engine_requests();
}

void Kernel::revert(std::string_view path) {
    Impl& impl = *impl_;
    const auto found = impl.documents.find(uri_of(path));
    if (found == impl.documents.end() || !found->second.overlay) return;
    auto text = platform::fs::read_file(found->second.path);
    if (!text) {
        impl.close_document(std::string { found->first });
        return;
    }
    found->second.overlay = false;
    impl.send_change(found->first, found->second, std::move(*text));
}

bool Kernel::is_open(std::string_view path) const { return impl_->documents.contains(uri_of(path)); }

std::optional<std::int64_t> Kernel::version(std::string_view path) const {
    const auto found = impl_->documents.find(uri_of(path));
    if (found == impl_->documents.end()) return std::nullopt;
    return found->second.version;
}

std::vector<std::string> Kernel::refresh() {
    Impl& impl = *impl_;
    std::vector<std::string> changed;
    for (auto& [uri, document] : impl.documents) {
        auto text = platform::fs::read_file(document.path);
        if (!text) continue;   // a removed file keeps its last content until closed
        if (document.overlay) {
            // An overlay stays until its file is written with the same content (the agent saved it) or it is closed.
            if (*text == document.text) document.overlay = false;
            continue;
        }
        if (*text == document.text) continue;
        changed.push_back(document.path);
        impl.send_change(uri, document, std::move(*text));
    }
    return changed;
}

std::optional<std::string> Kernel::text(std::string_view path) const {
    if (auto found = impl_->documents.find(uri_of(path)); found != impl_->documents.end()) return found->second.text;
    auto text = platform::fs::read_file(impl_->absolute(path));
    if (!text) return std::nullopt;
    return std::move(*text);
}

std::vector<std::string> Kernel::overlays() const {
    std::vector<std::string> paths;
    for (const auto& [uri, document] : impl_->documents) {
        if (document.overlay) paths.push_back(document.path);
    }
    std::ranges::sort(paths);
    return paths;
}

Json Kernel::status() const { return impl_->sink.status; }

std::optional<Json> Kernel::diagnostics(std::string_view uri) const {
    const auto found = impl_->sink.diagnostics.find(std::string { uri });
    if (found == impl_->sink.diagnostics.end()) return std::nullopt;
    return found->second;
}

int Kernel::diagnostics_publishes(std::string_view uri) const {
    const auto found = impl_->sink.diagnosticsCounts.find(std::string { uri });
    return found == impl_->sink.diagnosticsCounts.end() ? 0 : found->second;
}

bool Kernel::indexing() const { return !impl_->sink.progress.empty(); }

int Kernel::progress_begun() const { return impl_->sink.progressBegun; }

bool Kernel::wait_until(const std::function<bool()>& done, std::chrono::milliseconds timeout) {
    const auto until = Clock::now() + timeout;
    while (!done()) {
        if (Clock::now() >= until) return false;
        impl_->pump(std::min(until, Clock::now() + std::chrono::milliseconds { 100 }));
    }
    return true;
}

bool Kernel::wait_settled(std::chrono::milliseconds timeout) {
    return wait_until([this] {
        const Json& status { impl_->sink.status };
        if (!status.is_object() || impl_->workspace->model_loading()) return false;
        const std::string state { status.value("state", std::string {}) };
        return state == "ready" || state == "degraded" || state == "error";
    }, timeout);
}

void Kernel::post_external(Json message) {
    if (message.is_null()) return;
    impl_->events->push(Event { EventKind::external, std::move(message) });
}

void Kernel::close_external() { impl_->events->push(Event { EventKind::external, Json(nullptr) }); }

std::optional<Json> Kernel::next_external(std::chrono::milliseconds timeout) {
    const auto until = Clock::now() + timeout;
    while (impl_->external.empty()) {
        if (impl_->externalClosed || Clock::now() >= until) return std::nullopt;
        impl_->pump(until);
    }
    Json message = std::move(impl_->external.front());
    impl_->external.pop_front();
    return message;
}

std::optional<Json> Kernel::take_external(const std::function<bool(const Json&)>& wanted, std::chrono::milliseconds timeout) {
    const auto until = Clock::now() + timeout;
    while (true) {
        const auto found = std::ranges::find_if(impl_->external, wanted);
        if (found != impl_->external.end()) {
            Json message = std::move(*found);
            impl_->external.erase(found);
            return message;
        }
        if (impl_->externalClosed || Clock::now() >= until) return std::nullopt;
        impl_->pump(until);
    }
}

bool Kernel::input_closed() const { return impl_->externalClosed && impl_->external.empty(); }

void Kernel::shut_down() {
    if (impl_->shutDown) return;
    impl_->shutDown = true;
    impl_->workspace->shut_down();
}

} // namespace mcppls::orchestrator
