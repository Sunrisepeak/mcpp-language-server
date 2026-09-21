// ai/review step 5 (overall design 7.4, 7.5): renders the prompt, enforces the token budget, consults
// a content-addressed cache, calls the model, validates its answer against the schema, and turns a
// validated answer into spec::Finding — dropping anything whose evidence does not check out. M6:
// nothing is rendered or sent unless `settings.explicitlyEnabled`, and `explain_context` previews
// what would be sent without ever calling a model.
export module mcppls.ai.model.review;

import std;
import mcppls.spec.query;
import mcppls.ai.model.source;
import mcppls.ai.model.prompt;

export namespace mcppls::ai::model {

// A small cache of prior model answers, keyed by the caller (sha256 of template version + model +
// rendered messages), stored as one JSON file per key in `directory`. A missing or unreadable entry
// is a plain cache miss, never an error.
class Cache {
public:
    explicit Cache(std::string directory);

    std::optional<Json> get(std::string_view key) const;
    void put(std::string_view key, const Json& value) const;

private:
    std::string directory_;
};

// sha256 hex of the template version, the model name and every message's role and content.
std::string review_cache_key(std::string_view templateVersion, std::string_view model, const std::vector<Message>& messages);

struct ReviewResult {
    bool enabled { false };            // false: settings.explicitlyEnabled was false, nothing was sent
    bool budgetExceeded { false };     // the rendered prompt exceeded settings.tokenBudget; nothing was sent
    bool cacheHit { false };
    std::size_t estimatedTokens { 0 };
    std::vector<std::string> schemaErrors;    // non-empty: the model's output failed schema validation and every finding was dropped
    std::vector<std::string> droppedReasons;  // one entry per finding dropped for empty/unresolved evidence
    std::optional<std::string> error;         // the client's own error (transport, timeout, gateway exit, ...)
    std::string modelName;
    Usage usage;
    std::vector<spec::Finding> findings;
};

// Decides whether a model-proposed fix is kept (spec::Finding::fix) once verified elsewhere (design
// 7.4 step 6: `verify.snippet` or `verify.changed`). Without one, every fix is dropped; the finding
// that proposed it is kept regardless.
using VerifyFix = std::function<bool(const Json& fix)>;

ReviewResult model_findings(ModelClient& client, const ChangeContext& context, const ModelSettings& settings, Cache& cache, VerifyFix verifyFix = {});

struct ContextExplanation {
    bool enabled { false };
    std::vector<FileRange> included;
    std::vector<std::string> excludedFiles;
};

Json to_json(const ContextExplanation& explanation);

// What `model_findings` would send if it ran now, without calling a model or touching the cache.
ContextExplanation explain_context(const ChangeContext& context, const ModelSettings& settings);

} // namespace mcppls::ai::model
