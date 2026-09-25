// The diagnostic bundle (issue #23 fix plan F18): one zip with what a report of a problem needs --
// the server's report, the environment, the logs of the last sessions, the incidents the engines
// wrote down, the engine databases -- redacted by one set of rules and checked before it is written.
// Nothing is uploaded: the bundle is a file on this machine, for its user to attach or not.
//
//   manifest.json      what is inside, sizes and digests, the redaction rules applied and how often
//   report.json        cxxModules/report
//   environment.json   system, editor, extension, settings, payload, versions, probed toolchains,
//                      a whitelist of environment variables
//   logs/              the server's logs of the last sessions; the client's own log, when it sent one
//   incidents/root-N/  <workspace cache>/incidents/** of each root
//   engine/root-N/     each root's engine database and plan
//   dumps/             crash dumps, only when asked for: memory cannot be redacted
export module mcppls.bundle.writer;

import std;
import nlohmann.json;

export namespace mcppls::bundle {

inline constexpr int FORMAT_VERSION { 1 };
inline constexpr std::uint64_t DEFAULT_SIZE_CAP { std::uint64_t { 25 } * 1024 * 1024 };
inline constexpr std::size_t BUNDLES_KEPT { 5 };

struct BundleOptions {
    std::string output;               // the zip to write; empty: <cache>/bundles/mcppls-bundle-<UTC time>.zip, the newest kept
    bool redact { true };             // false only from the command line (--no-redact), to look at a problem locally
    bool hideProjectPaths { false };  // workspace roots become <workspace>, <workspace-2>, ...
    bool sourceExcerpts { true };     // the lines of source an incident carries
    bool includeDumps { false };
    std::uint64_t sizeCap { DEFAULT_SIZE_CAP };
};

// What the bundle is made of besides the files it reads itself.
struct BundleInput {
    nlohmann::json report;                                     // cxxModules/report, not redacted
    nlohmann::json client = nlohmann::json::object();          // extension, other C++ extensions, settings: what the client sent
    nlohmann::json initializationOptions = nlohmann::json::object();
    std::string clientLog;                                     // the client's own log, when it sent one
};

struct BundleResult {
    std::string path;
    std::uint64_t bytes { 0 };
    nlohmann::json redactions = nlohmann::json::object();   // rule -> replacements
    nlohmann::json manifest;
};

struct BundleFailure {
    std::string message;
    nlohmann::json residue = nlohmann::json::array();       // [{file, rule, offset}]: where the check found what redaction left
};

// Reads, redacts, checks and writes. Blocking: file reads and a few bounded system queries, so a
// server runs it off its event loop.
std::expected<BundleResult, BundleFailure> write_bundle(const BundleInput& input, const BundleOptions& options);

// A report as a client may show it: redacted under this process's identity.
nlohmann::json redact_report(const nlohmann::json& report);

} // namespace mcppls::bundle
