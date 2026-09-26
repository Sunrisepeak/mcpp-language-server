// The engine abstraction (overall design 5.1): every semantic capability of the server comes from
// an engine. mcppls's own engine answers module-level requests in process; the clangd engine drives
// an external clangd for the core C++ semantics. An engine declares the methods it takes part in and
// how (answer, fallback, merge); the workspace routes each request by those declarations and merges
// the results. Engines never call each other: what one needs from the workspace or from another
// engine goes through its Host.
export module mcppls.engine;

import std;
import nlohmann.json;
import mcppls.normalize.plan;

export namespace mcppls::engine {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

enum class Role {
    answer,     // the first engine, by priority, that claims the request answers it
    fallback,   // answers when the engines asked before it gave no result
    merge,      // every engine that claims the request answers; the workspace merges the results
};

inline constexpr std::string_view EVERY_METHOD { "*" };

struct MethodCapability {
    std::string method;          // an LSP method, or EVERY_METHOD for whatever no other engine declares
    Role role { Role::answer };
    int priority { 0 };          // higher is asked first
};

// How an engine behaves, which decides what the server does for it (design 5.1, 5.4). Looked up by
// engine and version; a version not in the table gets the conservative row: every compensation on.
struct EngineTraits {
    bool importNavigation { false };                  // answers definition on `import M;` itself
    bool pushesDiagnostics { true };
    bool hangsOnUnresolvedImports { false };          // units whose imports cannot resolve stay out of its database
    bool needsModulePreparation { false };            // the server prepares modules in parallel for it
    bool needsModuleHints { false };                  // its database names the unit of each module
    bool msvcStlNeedsNoAlignedAllocation { false };   // MSVC STL contexts turn aligned allocation off
    bool hangsOnTrailingDotModuleName { false };      // `import a.` at the end of a line spins it; it is given `import a.;`
    bool misplacesDirectiveSemicolon { false };       // a directive missing its `;` is reported on the next line; moved back
    bool readsImportsFromDisk { false };              // an import only in an unsaved buffer is "not found"; told as information
    std::string kitStdlibVersion;                     // the libc++ version a semantic kit must have for it (S4-4-5); empty: any
    bool tested { false };                            // a version this server's conformance suite runs against
};

struct Issue {
    std::string code;
    std::string message;
    std::string command;   // a client command id, optional
    // Whose problem it is (S3 status issue category, import-hang plan §6): "engine" (the engine lost something),
    // "environment" (the machine or the payload), or "code" (the user's source; told as diagnostics, never a
    // degraded state).
    std::string category { "engine" };
};

struct EngineStatus {
    std::string name;
    std::string version;
    std::string role;        // core | modules
    std::string state;       // starting | ready | preparing | unavailable
    bool accepting { false };
    bool preparing { false };
    bool failed { false };   // only syntax-level features remain: a corrupt payload, repeated crashes
    std::size_t prepared { 0 };
    std::size_t toPrepare { 0 };
    std::vector<Issue> issues;
    std::vector<Issue> notices;
};

// What an engine reports for one request.
struct Answer {
    enum class Kind { result, unavailable, error, cancelled };
    Kind kind { Kind::unavailable };
    Json value;   // the result, or the error object
};

using Reply = std::function<void(Answer)>;

// A file an incident carries besides its description: a name inside the incident's directory, and its content.
struct IncidentFile {
    std::string name;
    std::string content;
};

struct DocumentView {
    std::string uri;          // the client's URI
    std::string path;         // canonical path; empty for a URI that names no file
    std::string languageId;
    std::int64_t version { 0 };
    std::string_view text;
};

enum class DocumentChange { opened, changed, closed, saved };

struct DocumentEvent {
    DocumentChange change { DocumentChange::opened };
    DocumentView document;
    const Json* message { nullptr };   // the client's own notification, for engines that forward it
};

struct RequestView {
    std::string_view method;
    const Json* params { nullptr };
    std::string_view path;   // canonical path of params.textDocument.uri; empty without one
    std::string_view text;   // that document's buffer; empty when it is not open
};

// What the workspace offers the engines that serve it. Everything runs on the event loop except
// the function event_sink returns, which an engine's own threads call.
class Host {
public:
    virtual ~Host() = default;
    virtual const std::string& root_directory() const = 0;
    virtual const std::string& cache_directory() const = 0;
    virtual const Json& client_initialize_params() const = 0;
    // Hands an event to the event loop, which passes it back to the engine's handle_event.
    virtual std::function<void(Json)> event_sink(std::string_view engineId) = 0;
    virtual void send_to_client(const Json& message) = 0;
    // The id a request an engine sends to the client travels under, and back (client_response).
    virtual std::string client_request_id(std::string_view engineId, int generation, const Json& engineRequestId) const = 0;
    // An engine's diagnostics for a document the client has open; the workspace merges and publishes them.
    // `version` is the document version the diagnostics were computed for, when the engine says.
    virtual void publish_engine_diagnostics(std::string_view engineId, const std::string& clientUri, Json diagnostics, std::optional<std::int64_t> version) = 0;
    // Drops what the engine published so far without publishing anything, as when it restarts.
    virtual void forget_engine_diagnostics(std::string_view engineId) = 0;
    // Once per start: the server capabilities the engine brings, or an empty object when it has none.
    virtual void engine_settled(std::string_view engineId, const Json& serverCapabilities) = 0;
    virtual void status_changed() = 0;
    virtual void request_replan() = 0;
    // Semantic tokens (design doc 2026-09-25 K/§7): an engine's next answer for a file may differ
    // from what it last gave (clangd (re)started and finished its handshake, a file was set aside
    // or handed back, module preparation finished). The workspace sends
    // `workspace/semanticTokens/refresh`, coalesced, when the client declared
    // `workspace.semanticTokens.refreshSupport`; otherwise this is a no-op.
    virtual void semantic_tokens_changed() {}
    virtual std::vector<DocumentView> documents() const = 0;
    virtual bool has_document(std::string_view clientUri) const = 0;
    // One file, one name (v1 design 14.3): the URI engines are given, and back to the client's.
    virtual std::string engine_uri(std::string_view clientUri) const = 0;
    virtual std::string client_uri(std::string_view engineUri) const = 0;
    virtual void client_view(Json& value) const = 0;
    virtual std::string path_of_uri(std::string_view uri) const = 0;
    // The modules a source imports, from the workspace's source index.
    virtual std::vector<std::string> imports_of(std::string_view path) const = 0;
    // A moment a report of a problem needs (robustness design O1): what happened, with its details.
    virtual void record_event(std::string_view kind, Json detail) {
        (void)kind;
        (void)detail;
    }
    // Something went wrong that someone will want to look at afterwards (fix plan F17.2): a crash, a
    // file set aside, a restart held back. The workspace writes it to its incidents directory with the
    // files given (the engine's own log, say), what led up to it, and, when `pid` names the engine's
    // process, what each of its threads was doing. Off the event loop; rate limits are the caller's.
    virtual void record_incident(std::string_view kind, Json detail, std::vector<IncidentFile> files = {},
                                 std::optional<std::int64_t> pid = std::nullopt) {
        (void)kind;
        (void)detail;
        (void)files;
        (void)pid;
    }
};

class Engine {
public:
    virtual ~Engine() = default;
    virtual std::string_view id() const = 0;
    virtual std::span<const MethodCapability> methods() const = 0;
    virtual EngineTraits traits() const = 0;
    virtual EngineStatus status() const = 0;

    // Starts serving. `host` outlives the engine. The engine calls host.engine_settled once it
    // knows the capabilities it brings: at once, or after an external engine's handshake.
    virtual void start(Host& host) = 0;
    virtual void shut_down() = 0;

    // Planning: the engine sets the plan inputs its traits call for (driver directory, prime and
    // module hint directories, what to leave out), then receives each new plan; nullptr means serving
    // without one, when the project model did not load in time.
    virtual void configure_plan(normalize::PlanInput& input) const = 0;
    virtual void apply(const normalize::EnginePlan* plan) = 0;

    virtual void document(const DocumentEvent& event) = 0;
    virtual void notify(const Json& message) = 0;   // any other client notification
    virtual void sources_changed() = 0;             // sources or build descriptions changed on disk

    virtual bool claims(const RequestView& request) const = 0;
    // Answers the client's request `message`; `reply` is called exactly once, on the event loop.
    virtual void request(const RequestView& request, const Json& message, Reply reply) = 0;
    virtual void cancel(const Json& clientRequestId) = 0;
    // The client's response to a request this engine sent it (Host::client_request_id).
    virtual void client_response(int generation, const Json& engineRequestId, const Json& response) = 0;

    virtual void handle_event(const Json& event) = 0;
    virtual std::optional<Clock::time_point> next_deadline() const = 0;
    virtual void handle_timers() = 0;

    // Whether the engine has the file and is still working on what it needs to answer for it (its
    // modules are being built), so nothing it answers now means "there is nothing here". A person
    // hovering meanwhile is told that, rather than shown silence (real-project plan RP1.1).
    virtual bool busy_with(std::string_view path) const {
        (void)path;
        return false;
    }

    // What this engine knows that a report of a problem needs (robustness design O3).
    virtual Json report() const { return Json::object(); }

    // The person asked for the engine to start over (`workspace/executeCommand mcppls.restartEngine`, fix plan F14): at once,
    // whatever its restart budget says, and not counted in it. False when there is nothing to restart.
    virtual bool restart_on_request() { return false; }
};

} // namespace mcppls::engine
