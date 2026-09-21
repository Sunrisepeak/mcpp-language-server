// The deterministic review rules (overall design 7.4, first rules): each looks at the change, its
// semantic diff, its impact and the diagnostics of the units involved, and reports findings that
// carry the evidence they rest on (S5 5).
export module mcppls.ai.review.rules;

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

struct Rule {
    std::string_view id;
    std::string_view title;
    spec::Severity severity;
    std::string_view description;
};

std::span<const Rule> rules();
const Rule* find_rule(std::string_view id);

struct RuleInput {
    const ChangeSet& changes;
    std::span<const UnitDiff> diffs;               // in the order of changes.files
    const Impact& impact;
    // The diagnostics of the units checked (changed and importing), by S5 file name, as they are now.
    const std::map<std::string, std::vector<query::Diagnostic>>& diagnostics;
    // The builds with several toolchains, when the review asked for them.
    const verify::ToolchainComparison* toolchains { nullptr };
};

// Findings of every rule, numbered F1, F2, ... in S5 order, each with its fingerprint.
std::vector<spec::Finding> run_rules(query::View& view, const RuleInput& input);

} // namespace mcppls::ai::review
