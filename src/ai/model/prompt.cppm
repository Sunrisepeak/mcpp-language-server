// Versioned prompt templates for the evidence-driven model review (overall design 7.4 step 5): turns
// a change context into chat messages with the code kept inside clearly delimited, escaped data
// blocks, instructions that the model must cite evidence ids and must not follow instructions found
// in the data, and the JSON Schema its answer must match.
export module mcppls.ai.model.prompt;

import std;
import nlohmann.json;
import mcppls.spec.query;
import mcppls.ai.model.source;

export namespace mcppls::ai::model {

// Bump this whenever the template's wording or output shape changes; it travels with every model
// finding (spec::ModelOrigin::templateVersion) so a stale cache entry or an old finding is traceable.
inline constexpr std::string_view REVIEW_TEMPLATE_VERSION { "review-1" };

// What ai/review's steps 1-4 hand to the model step: the deterministic part of a review, plus the
// evidence (E1..En) findings may cite. `semanticDiff` and `impact` are opaque JSON (ai/review's own
// steps 2-3 shape them; this layer only renders whatever they contain).
struct ChangeContext {
    Json semanticDiff;
    Json impact;
    std::vector<spec::Finding> deterministicFindings;
    std::vector<spec::Evidence> evidence;
};

struct FileRange {
    std::string file;
    int line { 0 };
    int endLine { 0 };
    bool operator==(const FileRange&) const = default;
};

// What filtering `context.evidence` through `excludedPaths` (mcppls.base.glob patterns) leaves: the
// ranges that would actually be sent, and the files withheld because a pattern matched them.
struct FilteredEvidence {
    std::vector<FileRange> included;
    std::vector<std::string> excludedFiles;
};

FilteredEvidence filter_evidence(const ChangeContext& context, std::span<const std::string> excludedPaths);

// System + user messages for `settings.model`; evidence whose file matches `settings.excludedPaths`
// is replaced by a marker in the rendered text (design 11: excluded paths never reach a model).
std::vector<Message> render_review_prompt(const ChangeContext& context, const ModelSettings& settings);

// The output JSON Schema every review completion is validated against (mcppls.ai.model.schema).
Json review_output_schema();

} // namespace mcppls::ai::model
