// The editor's open documents: text, version and language, with LSP content
// changes applied the way the client describes them (UTF-16 ranges).
export module mcppls.orchestrator.documents;

import std;
import nlohmann.json;
import mcppls.base.text;

export namespace mcppls::orchestrator {

struct Document {
    std::string uri;
    std::string path;          // empty for non-file URIs
    std::string languageId;
    std::int64_t version { 0 };
    std::string text;
};

class DocumentStore {
private:
    std::map<std::string, Document, std::less<>> documents_;

public:
    Document& open(std::string uri, std::string path, std::string languageId, std::int64_t version, std::string text);
    // Applies TextDocumentContentChangeEvent[]; returns false when the document is not open.
    bool change(std::string_view uri, std::int64_t version, const nlohmann::json& contentChanges);
    void close(std::string_view uri);
    const Document* find(std::string_view uri) const;
    const Document* find_by_path(std::string_view path) const;
    std::vector<const Document*> all() const;
};

// Applies one change: a range edit, or a full replacement when there is no range.
void apply_change(std::string& text, const nlohmann::json& change);

} // namespace mcppls::orchestrator
