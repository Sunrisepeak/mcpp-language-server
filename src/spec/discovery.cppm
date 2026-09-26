// S2: the discovery command protocol (docs/specs/s2-discovery.md). In stream mode a
// producer is started with one JSON request on stdin and answers JSON lines on
// stdout; in single-document mode it prints one envelope with the database inline.
export module mcppls.spec.discovery;

import std;
import nlohmann.json;
import mcppls.base.error;

export namespace mcppls::spec {

struct DiscoveryRequest {
    std::string workspace;
    std::vector<std::string> files;
    std::string configuration;
    // S2 3.2: whether the consumer allows this run to reach the network. A producer may ignore it;
    // the consumer does not rely on it (design 4.4: mcpp is made offline through its own switch).
    bool network { false };
};

// How a producer is run: the bounds, whether it may reach the network, and what is written down
// about the run. Design 4.2; every field but the bounds is for the record and the status line.
struct RunContext {
    std::string purpose { "producer" };
    std::string root;
    bool offline { true };
    std::chrono::milliseconds hard { std::chrono::seconds { 60 } };
    std::chrono::milliseconds soft { std::chrono::seconds { 5 } };
    std::chrono::milliseconds environmentWait { 0 };
    std::function<void(std::chrono::milliseconds)> onSoftDeadline;
};

struct DiscoveryResult {
    std::string database;
    std::vector<std::string> watch;
    std::vector<std::string> progress;
};

// S2 0.2 single-document mode: a producer that follows a machine-output envelope
// (mcpp's wire protocol v1) prints one JSON document whose `data` carries the
// database inline, and advertises that it can with a protocol description.
struct EnvelopeDiagnostic {
    std::string code;
    std::string severity;   // error | warning | note
    std::string message;
    std::string path;       // S2 0.3.0: the file it concerns, relative to the workspace root; empty when it names none
};

struct DatabaseDocument {
    nlohmann::json database;                  // the S1 document
    std::vector<std::string> watch;           // absolute paths or LSP glob patterns relative to the workspace
    std::string inputsFingerprint;
    std::vector<std::string> effects;         // what running the producer did
    std::vector<EnvelopeDiagnostic> diagnostics;
    std::uint64_t runId { 0 };                // the tool-run record of the run that produced it
    bool networkObserved { false };           // the envelope's effects said the producer reached the network
};

// The diagnostic code mcpp 2026.9.16.1 answers with when planning offline needs a download
// (mcpp-community/mcpp#648 A1). Older mcpp has only the message text, which is matched instead.
inline constexpr std::string_view OFFLINE_DOWNLOAD_REQUIRED { "MCPP_OFFLINE_DOWNLOAD_REQUIRED" };
// The error code this server fails with when that happened: the model stays, the user gets an action.
inline constexpr std::string_view NEEDS_DOWNLOAD { "producer-needs-download" };

struct ProducerProtocol {
    std::map<std::string, int, std::less<>> kinds;                                  // kind -> version
    std::map<std::string, std::vector<std::string>, std::less<>> commandEffects;    // "emit build-database" -> effects
};

inline constexpr std::string_view BUILD_DATABASE_KIND_SUFFIX { ".build-database" };

// `<producer> --protocol-version`.
base::Result<ProducerProtocol> parse_producer_protocol(std::string_view output);
// Whether a consumer may run a command with these declared effects to describe a project (S2 3.4).
// Reading the project, the network, the producer's own caches and build scripts are what describing
// a build takes; writing into the project is what single-document mode exists to avoid.
bool effects_acceptable(std::span<const std::string> effects);
// An envelope of a `*.build-database` kind with `data.database`.
base::Result<DatabaseDocument> parse_database_envelope(std::string_view output);
// Runs a producer command without input and interprets its output as a database envelope.
base::Result<DatabaseDocument> run_database_command(std::span<const std::string> command, std::string_view workDirectory,
                                                    const RunContext& how);
// Whether these diagnostics say the run could not go on without downloading something, and what.
std::optional<std::string> download_required(std::span<const EnvelopeDiagnostic> diagnostics);

nlohmann::json make_discovery_request(const DiscoveryRequest& request);
// Interprets a producer's complete standard output.
base::Result<DiscoveryResult> parse_discovery_output(std::string_view output);
// Runs `command` (argv; absolute program first) in `workDirectory`.
base::Result<DiscoveryResult> run_discovery(std::span<const std::string> command, const DiscoveryRequest& request,
                                            std::string_view workDirectory, const RunContext& how);

} // namespace mcppls::spec
