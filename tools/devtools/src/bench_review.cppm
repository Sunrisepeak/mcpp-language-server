// `mcppls-devtools bench review`: whether `mcppls review` finds what a change breaks, and
// nothing a clean change does not. Ported from tools/bench/review.py; see bench/README.md
// "Review fixtures".
export module mcppls.devtools.bench.review;

import std;
import nlohmann.json;
import mcpplibs.cmdline;
import mcppls.base.error;

export namespace mcppls::devtools::bench {

// Whether `finding` (a review finding, as the server's `--format json` writes it) satisfies
// `entry` (one "must"/"must-not" item of a review.json): same "rule", and, for every field the
// entry gives, a match on it too ("origin", "file", "line", "evidence-file", "fixed").
bool review_finding_matches(const nlohmann::json& finding, const nlohmann::json& entry);

// Every file under `directory` with its content digest, relative paths as keys.
nlohmann::json review_snapshot(const std::string& directory);

struct FixtureOutcome {
    std::string id;
    bool ok { false };
    std::optional<std::string> skipped;
    double seconds { 0.0 };
    std::vector<nlohmann::json> findings;
    std::vector<std::string> problems;
    // For the per-rule precision/recall table: `meta["must"]` alongside whether each was matched,
    // and every finding that hit a "must-not" entry.
    nlohmann::json meta;
    std::vector<bool> matchedMust;
    std::vector<nlohmann::json> falsePositives;
    // Whether the run went all the way through (matches "matched"/"false-positives"/"complete"
    // being present in Python's outcome dict at all: absent for a skip or an early failure).
    bool full { false };
    bool complete { true };
};

struct ReviewOptions {
    std::string server;
    std::string payload;
    std::string mockModel;
    int timeoutSeconds { 300 };
};

// Runs one fixture (`fixtureDir` holding review.json and change/) under `scratch`, which the
// caller owns (created and removed around every fixture this call is part of).
FixtureOutcome run_review_fixture(const std::string& fixtureDir, const std::string& repositoryRoot,
                                  const ReviewOptions& options, const std::string& scratch);

// `mcppls-devtools bench review --server PATH [--payload DIR] [--mock-model PATH]
//     [--fixture ID ...] [--timeout SECONDS] [--keep] [--report FILE]`.
int command_review(const mcpplibs::cmdline::ParsedArgs& arguments);

// True positives (a "must" entry matched), false negatives (one that was not) and false
// positives (a finding that hit a "must-not" entry), by rule, across every fixture outcome.
struct RuleStats {
    int tp { 0 };
    int fn { 0 };
    int fp { 0 };
};
std::map<std::string, RuleStats> review_rule_stats(const std::vector<FixtureOutcome>& outcomes);
double review_precision(const RuleStats& stats);
double review_recall(const RuleStats& stats);

} // namespace mcppls::devtools::bench
