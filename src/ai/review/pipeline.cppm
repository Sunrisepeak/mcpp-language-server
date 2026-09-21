// The review of a change (overall design 7.4): collect it, diff it semantically, find what it can
// break, have the core engine build the units involved, and run the rules. Deterministic from start
// to end; a model's judgement comes on top of it (ai/model), never instead of it.
export module mcppls.ai.review.pipeline;

import std;
import nlohmann.json;
import mcppls.spec.query;
import mcppls.ai.query.view;
import mcppls.ai.query.files;
import mcppls.ai.verify.toolchains;
import mcppls.ai.review.changes;
import mcppls.ai.review.semantic;
import mcppls.ai.review.impact;

export namespace mcppls::ai::review {

struct ReviewRequest {
    ChangeRequest changes;
    std::size_t budget { 32 };           // units the core engine builds at most
    bool build { true };                 // false: impact only, nothing built
    std::vector<std::string> toolchains; // two or more: the project built with each, for build/toolchain-divergence
};

struct ReviewResult {
    spec::Snapshot snapshot;
    ChangeSet changes;
    std::vector<UnitDiff> diffs;         // one per changed C++ file, in the order of changes.files
    Impact impact;
    std::map<std::string, std::vector<query::Diagnostic>> diagnostics;   // of the units built
    std::vector<std::string> unbuilt;    // beyond the budget, or not complete in time
    std::optional<verify::ToolchainComparison> toolchains;
    std::optional<query::Failure> toolchainFailure;
    std::vector<spec::Finding> findings;
    std::map<std::string, int> counts;   // findings by severity
};

// Steps 1 to 3: what changed and what it can break.
query::Outcome<ReviewResult> analyze_change(query::View& view, const ReviewRequest& request, query::Clock::time_point deadline);
// Steps 1 to 4, and 7 for S5: the findings of the rules.
query::Outcome<ReviewResult> review_change(query::View& view, const ReviewRequest& request, query::Clock::time_point deadline);

nlohmann::json impact_json(const ReviewResult& result);
nlohmann::json review_json(const ReviewResult& result);

} // namespace mcppls::ai::review
