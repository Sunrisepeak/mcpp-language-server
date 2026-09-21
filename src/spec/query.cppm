// S5: semantic queries and review (docs/specs/s5-semantic-query.md) — the data every query, verification
// and review result is made of: locations an agent can read without counting columns, the snapshot a
// result was computed from, and findings with the evidence behind them.
export module mcppls.spec.query;

import std;
import nlohmann.json;
import mcppls.base.text;

export namespace mcppls::spec {

inline constexpr std::string_view S5_VERSION { "0.1.0" };

// S5 2.1: lines and columns count from 1, a column counts Unicode scalar values, and the text of the
// line comes along. `file` is relative to the workspace root with '/' separators, absolute outside it.
struct Location {
    std::string file;
    int line { 0 };
    int column { 0 };
    std::string text;
    std::optional<int> endLine;
    std::optional<int> endColumn;
    bool operator==(const Location&) const = default;
};

// S5 2.2: what a result was computed from.
struct Snapshot {
    std::uint64_t generation { 0 };
    std::vector<std::string> overlays;   // files whose unsaved content differs from the disk
    bool preparing { false };            // modules were still being prepared
    bool indexing { false };             // the core engine's index was still being built
};

enum class Severity { error, warning, information, hint };
std::string_view to_string(Severity severity);
std::optional<Severity> parse_severity(std::string_view text);
// LSP DiagnosticSeverity 1..4.
int lsp_severity(Severity severity);
Severity severity_from_lsp(int severity);

// S5 5: a fact a finding rests on.
struct Evidence {
    std::string id;                      // "E1", unique within its finding
    std::string kind;                    // reference | diff | diagnostic | module-graph | import | test
    Location location;
    std::string detail;
};

struct ModelOrigin {
    std::string source;                  // agent | mcp-sampling | client | gateway
    std::string name;                    // the model's own name
    std::string templateVersion;
};

struct Finding {
    std::string id;                      // "F1", unique within its review
    std::string rule;                    // module/export-removed-in-use, ...
    Severity severity { Severity::warning };
    std::string message;
    Location location;
    std::vector<Evidence> evidence;
    std::string origin { "rule" };       // rule | model
    nlohmann::json fix;                  // null, or a verified change: {"description", "edits": [{"file", "range", "newText"}]}
    std::string fingerprint;             // "sha256:<hex>"
    std::optional<ModelOrigin> model;
    std::optional<double> confidence;
};

// Position arithmetic between LSP (0-based lines, UTF-16 code units) and S5.
std::string line_of(std::string_view text, int zeroBasedLine);
int column_from_utf16(std::string_view lineText, int character);   // 1-based
int utf16_from_column(std::string_view lineText, int column);       // 0-based
// The S5 location of an LSP position (and optional end) in `text`, the content of `file`.
Location location_in(std::string_view text, std::string file, base::Position start, std::optional<base::Position> end = std::nullopt);
// The LSP position of an S5 line and column in `text`.
base::Position lsp_position(std::string_view text, int line, int column);

// A finding's fingerprint: the rule, the file and the normalized text of the location and of the
// evidence, so it survives lines moving above it.
std::string fingerprint_of(const Finding& finding);

nlohmann::json to_json(const Location& location);
nlohmann::json to_json(const Snapshot& snapshot);
nlohmann::json to_json(const Evidence& evidence);
nlohmann::json to_json(const Finding& finding);
std::optional<Location> location_from_json(const nlohmann::json& value);
std::optional<Finding> finding_from_json(const nlohmann::json& value);

} // namespace mcppls::spec
