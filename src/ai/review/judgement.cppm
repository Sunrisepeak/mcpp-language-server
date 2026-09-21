// A model's judgement on top of a review (overall design 7.4 step 5, 7.5): the deterministic review
// becomes the change context the model reads — every piece of evidence numbered once — and what the
// model answers comes back as findings that cite that evidence. With the `agent` source nothing is
// called: the calling agent gets the context and the instructions, and judges itself.
export module mcppls.ai.review.judgement;

import std;
import nlohmann.json;
import mcppls.spec.query;
import mcppls.ai.model.source;
import mcppls.ai.model.prompt;
import mcppls.ai.model.review;
import mcppls.ai.review.pipeline;

export namespace mcppls::ai::review {

struct JudgementOptions {
    model::ModelSettings settings;
    std::string cacheDirectory;           // where model answers are kept, by the hash of what was sent
    bool explainOnly { false };           // say what would be sent, and send nothing
    // Whether a fix a model proposes compiles (ai/verify's verify_fix); without it no fix is kept.
    std::function<bool(const nlohmann::json& fix)> verifyFix;
};

struct Judgement {
    std::string source;
    bool enabled { false };
    bool budgetExceeded { false };
    bool cacheHit { false };
    std::optional<std::string> error;
    std::vector<std::string> schemaErrors;
    std::vector<std::string> droppedReasons;
    std::string modelName;
    model::Usage usage;
    std::vector<spec::Finding> findings;  // origin model, numbered after the review's own
    std::optional<model::ContextExplanation> explanation;
    nlohmann::json agentContext;          // for the agent source: the context and the prompt it would have been sent
};

// The change context of a review: its semantic diff and impact, its findings, and every piece of
// evidence — the findings' and a `diff` item per changed hunk — with ids unique across the context.
model::ChangeContext change_context(const ReviewResult& result);

// `client` is null for the none and agent sources.
Judgement judge(const ReviewResult& result, const JudgementOptions& options, model::ModelClient* client);

nlohmann::json to_json(const Judgement& judgement);

} // namespace mcppls::ai::review
