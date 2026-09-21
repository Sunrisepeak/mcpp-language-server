// What every query sees of a headless session (overall design 7.1): files named the way S5 names
// them, LSP positions turned into S5 locations and back, the snapshot a result was computed from,
// and requests that wait, within a deadline, for the engines to be able to answer.
export module mcppls.ai.query.view;

import std;
import nlohmann.json;
import mcppls.base.text;
import mcppls.spec.query;
import mcppls.orchestrator.kernel;

export namespace mcppls::ai::query {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

// Why a query gave no result: S5 3.3's error codes.
struct Failure {
    std::string code;      // invalid-arguments | not-found | ambiguous | unavailable | timeout | untrusted
    std::string message;
    Json candidates;       // for ambiguous: what the caller can choose from
};

template <class T>
using Outcome = std::expected<T, Failure>;

Failure invalid_arguments(std::string message);
Failure not_found(std::string message);

// S5 2.4: at most `maxResults` items; `total` counts them all.
struct Limit {
    std::size_t maxResults { 50 };
};

class View {
public:
    explicit View(orchestrator::Kernel& kernel);

    orchestrator::Kernel& kernel() { return kernel_; }
    const std::string& root() const;

    // A file argument (relative to the root, absolute, or a file URI) as a canonical absolute path.
    std::string path_of(std::string_view file) const;
    // S5 2.1: relative to the root with '/' separators, absolute outside it.
    std::string display(std::string_view path) const;
    std::string text_of(std::string_view path) const;
    // The module a unit belongs to, "m" or "m:p"; empty for a unit that is not a module unit.
    std::string module_of(std::string_view path) const;

    spec::Location location(std::string_view path, const Json& range) const;
    // An LSP Location or LocationLink.
    std::optional<spec::Location> location(const Json& lspLocation) const;
    std::optional<std::string> path_of_uri(std::string_view uri) const;
    Json lsp_position(std::string_view path, int line, int column) const;
    Json text_document(std::string_view path) const;

    spec::Snapshot snapshot() const;

    // Brings the engines' documents up to date with the disk.
    void refresh();
    // Waits until the model and the core engine settled.
    bool settle(Clock::time_point deadline);
    // Waits until the core engine's index is built, when it builds one: a report that begins soon
    // after the engine settled, and ends.
    void settle_index(Clock::time_point deadline);
    // The result of an LSP request: null for an error response, nullopt when the deadline passed.
    std::optional<Json> request(std::string_view method, Json params, Clock::time_point deadline);
    void open(std::string_view path);
    bool has_core_engine() const;

private:
    orchestrator::Kernel& kernel_;
};

// LSP SymbolKind as S5 names it.
std::string_view symbol_kind_name(int kind);
std::optional<int> symbol_kind_from_name(std::string_view name);

nlohmann::json to_json(const Failure& failure);

} // namespace mcppls::ai::query
