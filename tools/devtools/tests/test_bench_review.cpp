// The review fixture matcher (review.py's `matches()`) and the workspace snapshot used to
// detect "the review changed the workspace" (tools/bench/README.md "Review fixtures").
import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.base.path;
import mcppls.platform.fs;
import mcppls.devtools.bench.review;

namespace bench = mcppls::devtools::bench;
namespace fs = mcppls::platform::fs;
namespace base = mcppls::base;

namespace {

std::string temp_dir(std::string_view name) {
    const auto root = std::filesystem::current_path() / ".test-scratch"
                      / std::format("bench-review-{}-{}", name, std::random_device {}());
    std::filesystem::create_directories(root);
    return root.string();
}

// Built on first use: a JSON value made by a dynamic initializer before main crashed test binaries
// on the macOS runner (exit 139, no output) -- test_gateway first, then this one.
const nlohmann::json& finding() {
    static const nlohmann::json value = nlohmann::json::parse(R"({
    "rule": "module/export-removed-in-use", "origin": "rule",
    "location": {"file": "src/greet/format.cppm", "line": 6},
    "evidence": [{"location": {"file": "src/greet/greet.cpp"}}]
})");
    return value;
}

} // namespace

int main() {
    using namespace mcppls::testing;

    "a finding matches an entry naming only its rule"_test = [] {
        expect(bench::review_finding_matches(finding(), nlohmann::json::parse(R"({"rule": "module/export-removed-in-use"})")));
    };

    "a finding does not match a different rule"_test = [] {
        expect(!bench::review_finding_matches(finding(), nlohmann::json::parse(R"({"rule": "module/partition-misuse"})")));
    };

    "file and line are checked when the entry gives them"_test = [] {
        expect(bench::review_finding_matches(finding(), nlohmann::json::parse(
            R"({"rule": "module/export-removed-in-use", "file": "src/greet/format.cppm", "line": 6})")));
        expect(!bench::review_finding_matches(finding(), nlohmann::json::parse(
            R"({"rule": "module/export-removed-in-use", "file": "src/greet/format.cppm", "line": 7})")));
    };

    "origin is checked when the entry gives it"_test = [] {
        expect(!bench::review_finding_matches(finding(), nlohmann::json::parse(
            R"({"rule": "module/export-removed-in-use", "origin": "model"})")));
    };

    "evidence-file is satisfied by any evidence entry's location"_test = [] {
        expect(bench::review_finding_matches(finding(), nlohmann::json::parse(
            R"({"rule": "module/export-removed-in-use", "evidence-file": "src/greet/greet.cpp"})")));
        expect(!bench::review_finding_matches(finding(), nlohmann::json::parse(
            R"({"rule": "module/export-removed-in-use", "evidence-file": "src/main.cpp"})")));
    };

    "fixed compares the presence of a non-null fix, not its content"_test = [] {
        auto withFix = finding();
        withFix["fix"] = nlohmann::json::object();
        expect(bench::review_finding_matches(withFix, nlohmann::json::parse(
            R"({"rule": "module/export-removed-in-use", "fixed": true})")));
        expect(!bench::review_finding_matches(finding(), nlohmann::json::parse(
            R"({"rule": "module/export-removed-in-use", "fixed": true})")));
        expect(bench::review_finding_matches(finding(), nlohmann::json::parse(
            R"({"rule": "module/export-removed-in-use", "fixed": false})")));
    };

    "the snapshot is a map of relative path to content digest"_test = [&] {
        const std::string dir { temp_dir("snap") };
        (void) fs::create_directories(base::join_path(dir, "src"));
        (void) fs::write_file(base::join_path(dir, "src/main.cpp"), "int main(){}");
        auto snapshot = bench::review_snapshot(dir);
        expect(snapshot.is_object());
        expect(snapshot.contains("src/main.cpp"));
        expect(snapshot.at("src/main.cpp").is_string());
        std::filesystem::remove_all(dir);
    };

    "two snapshots of the same unchanged tree are equal; a changed file differs"_test = [&] {
        const std::string dir { temp_dir("snap-eq") };
        (void) fs::write_file(base::join_path(dir, "f.txt"), "same");
        const auto before = bench::review_snapshot(dir);
        const auto unchanged = bench::review_snapshot(dir);
        expect(before == unchanged);
        (void) fs::write_file(base::join_path(dir, "f.txt"), "different");
        const auto after = bench::review_snapshot(dir);
        expect(!(before == after));
        std::filesystem::remove_all(dir);
    };

    "rule stats: a matched must is a true positive, an unmatched one a false negative"_test = [] {
        bench::FixtureOutcome outcome;
        outcome.meta = nlohmann::json::parse(R"({"must": [{"rule": "r1"}, {"rule": "r1"}, {"rule": "r2"}]})");
        outcome.matchedMust = { true, false, true };
        outcome.full = true;
        const auto stats = bench::review_rule_stats({ outcome });
        expect(stats.at("r1").tp == 1);
        expect(stats.at("r1").fn == 1);
        expect(stats.at("r2").tp == 1);
        expect(bench::review_recall(stats.at("r1")) == 0.5);
        expect(bench::review_precision(stats.at("r1")) == 1.0);
    };

    "rule stats: a false positive lowers precision but not recall"_test = [] {
        bench::FixtureOutcome outcome;
        outcome.meta = nlohmann::json::parse(R"({"must": [{"rule": "r1"}]})");
        outcome.matchedMust = { true };
        outcome.falsePositives = { nlohmann::json::parse(R"({"rule": "r1"})") };
        outcome.full = true;
        const auto stats = bench::review_rule_stats({ outcome });
        expect(stats.at("r1").tp == 1);
        expect(stats.at("r1").fp == 1);
        expect(bench::review_precision(stats.at("r1")) == 0.5);
        expect(bench::review_recall(stats.at("r1")) == 1.0);
    };

    "rule stats: no tp and no fp is perfect precision by convention"_test = [] {
        expect(bench::review_precision(bench::RuleStats {}) == 1.0);
        expect(bench::review_recall(bench::RuleStats {}) == 1.0);
    };

    return report();
}
