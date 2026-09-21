// File queries (overall design 7.1, S5 3): a compact outline of a file, and its diagnostics, fresh
// when asked: computed by the core engine for the content the file has now.
export module mcppls.ai.query.files;

import std;
import nlohmann.json;
import mcppls.spec.query;
import mcppls.ai.query.view;

export namespace mcppls::ai::query {

struct OutlineEntry {
    std::string name;
    std::string kind;
    int line { 0 };
    std::string detail;
    std::vector<OutlineEntry> children;
};

struct Outline {
    spec::Snapshot snapshot;
    std::string file;
    std::string module;
    std::vector<OutlineEntry> symbols;
};

Outcome<Outline> outline_file(View& view, std::string_view file, Clock::time_point deadline);

struct RelatedInformation {
    spec::Location location;
    std::string message;
};

struct Diagnostic {
    spec::Severity severity { spec::Severity::error };
    std::string message;
    std::string code;
    std::string source;
    spec::Location location;
    std::vector<RelatedInformation> related;
};

struct FileDiagnostics {
    std::string file;
    bool complete { false };   // the core engine's diagnostics for this content are in
    std::string reason;        // why not complete
    std::vector<Diagnostic> diagnostics;
};

struct DiagnosticsReport {
    spec::Snapshot snapshot;
    std::vector<FileDiagnostics> files;
    std::map<std::string, int> counts;   // by severity
    std::string semanticSource;          // "build toolchain gcc 16.1.0" or "semantic kit libc++ 23.1.0"
};

// Opens the files, waits for fresh diagnostics until the deadline when `fresh`, and reads them.
Outcome<DiagnosticsReport> file_diagnostics(View& view, std::span<const std::string> files, bool fresh, Clock::time_point deadline);
// The diagnostics published for an open file, as S5 has them.
std::vector<Diagnostic> published_diagnostics(View& view, std::string_view path);
// Whether the core engine's diagnostics for the open file's current content are in, and if not why.
std::pair<bool, std::string> diagnostics_fresh(View& view, std::string_view path);
std::string semantic_source(View& view);

nlohmann::json to_json(const Outline& outline);
nlohmann::json to_json(const Diagnostic& diagnostic);
nlohmann::json to_json(const DiagnosticsReport& report);

} // namespace mcppls::ai::query
