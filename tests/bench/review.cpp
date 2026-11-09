// The review of a change (overall design 7.4, 10.3; README.md, "Review fixtures"): each fixture of
// review/ is a case -- the server reviews the fixture's change of a project, and its findings must
// include what the fixture's review.json requires and nothing it rules out, without the review
// changing the workspace -- and each rule's precision and recall over the fixtures are metrics.
import std;
import mcpplibs.mtest;
import mcpplibs.os;
import mcppls.harness;
import mcppls.harness.review;

using namespace mcpplibs::mtest;
using mcppls::harness::on;
namespace harness = mcppls::harness;
namespace os = mcpplibs::os;

namespace {

std::string absolute(const std::string& path) {
    return path.empty() ? path : os::fs::utf8(std::filesystem::absolute(os::fs::from_utf8(path)));
}

// mcppls-mock-model: MCPPLS_BENCH_MOCK_MODEL, else beside this program, else none.
std::string mock_model() {
    if (const auto named = harness::env_or("MCPPLS_BENCH_MOCK_MODEL", ""); !named.empty())
        return absolute(named);
    return harness::beside_self("mcppls-mock-model");
}

// A directory the fixtures share -- their workspaces and one cache, so the std module is built
// once -- removed with the case.
struct scratch {
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                std::format("mcppls-bench-review-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    scratch() { std::filesystem::create_directories(dir); }
    scratch(const scratch&) = delete;
    ~scratch() {
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
    }
};

// The fixtures of review/, by name.
std::vector<std::filesystem::path> fixtures() {
    std::vector<std::filesystem::path> out;
    const auto here = os::fs::from_utf8("tests/bench/review");
    std::error_code missing;
    for (const auto& entry : std::filesystem::directory_iterator(here, missing))
        if (std::filesystem::exists(entry.path() / "review.json"))
            out.push_back(entry.path());
    std::ranges::sort(out);
    return out;
}

Bench review{"review", on{"linux-x64", "darwin-arm64", "win32-x64"}, [] {
    const auto found = fixtures();
    if (found.empty())
        skip("tests/bench/review is not in the working directory");
    const harness::ReviewOptions options{
        .server = absolute(harness::env_or("MCPPLS_CTS_SERVER", "payload/bin/mcppls.okb")),
        .payload = absolute(harness::env_or("MCPPLS_CTS_PAYLOAD", "payload")),
        .mockModel = mock_model(),
    };
    const auto root = os::fs::utf8(std::filesystem::current_path());
    shared<scratch> work{"scratch"};
    std::vector<harness::FixtureOutcome> outcomes;
    for (const auto& fixture : found) {
        test(os::fs::utf8(fixture.filename())) = [&] {
            auto outcome = harness::run_review_fixture(os::fs::utf8(fixture), root, options, os::fs::utf8(work->dir));
            if (outcome.skipped)
                skip(*outcome.skipped);
            for (const auto& problem : outcome.problems)
                expect(false) << problem;
            metric("seconds", outcome.seconds, "s");
            metric("findings", static_cast<double>(outcome.findings.size()), "1");
            outcomes.push_back(std::move(outcome));
        };
    }
    // Over the fixtures that ran: what is found is what is there, and nothing more.
    "every rule's precision and recall"_test[at_least{"precision", 1}][at_least{"recall", 1}] = [&] {
        if (outcomes.empty())
            skip("no fixture ran");
        harness::RuleStats all;
        for (const auto& [rule, stats] : harness::review_rule_stats(outcomes)) {
            metric(std::format("precision.{}", rule), harness::review_precision(stats), "1", {}, "higher");
            metric(std::format("recall.{}", rule), harness::review_recall(stats), "1", {}, "higher");
            all.tp += stats.tp;
            all.fn += stats.fn;
            all.fp += stats.fp;
        }
        metric("precision", harness::review_precision(all), "1", {}, "higher");
        metric("recall", harness::review_recall(all), "1", {}, "higher");
    };
}};

}  // namespace
