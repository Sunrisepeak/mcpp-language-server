// timing_summary.py, ported: medians over cold-*.json / warm-*.json in a directory.
import std;
import mcppls.testing;
import mcppls.base.path;
import mcppls.devtools.measure;
import mcpplibs.os;

namespace os = mcpplibs::os;

namespace measure = mcppls::devtools::measure;
namespace base = mcppls::base;

namespace {

std::string scratch(std::string_view name) {
    std::error_code ignored;
    const std::string directory { base::join_path(
        base::spelled(os::dirs::temp()),
        std::format("mcppls-test-measure-{}-{}", name, std::chrono::steady_clock::now().time_since_epoch().count())) };
    (void)std::filesystem::create_directories(directory, ignored);
    return directory;
}

} // namespace

int main() {
    using namespace mcppls::testing;

    "the scenarios' numbers are gathered over every run, at any depth, for budgets (U-*)"_test = [&] {
        const std::string directory { scratch("budgets") };
        const auto write_run = [&](std::string_view sub, double p95, double share) {
            std::error_code ignored;
            const std::string folder { base::join_path(directory, sub) };
            (void)std::filesystem::create_directories(folder, ignored);
            const std::string run { std::format(R"({{"fixture":"ux","checks":[{{"id":"U6","kind":"typing","ok":true,)"
                                                R"("measure":{{"completion":{{"p95":{},"n":10}},"engineShare":{},"statesSeen":[],"file":"x"}}}},)"
                                                R"({{"id":"S1","kind":"status","ok":true}}]}})", p95, share) };
            (void) os::fs::write(base::join_path(folder, "ux-edits.json"), run);
        };
        write_run("round-1", 0.4, 0.8);
        write_run("round-2", 0.6, 0.9);
        (void) os::fs::write(base::join_path(directory, "notes.json"), R"({"not":"a measure file"})");
        auto text = measure::budgets(directory);
        expect(fatal(text.has_value()));
        expect(text->contains("| U6 | completion.p95 | 2 |")) << *text;
        expect(text->contains("| U6 | engineShare | 2 |")) << *text;
        expect(text->contains("| U6 | completion.n | 2 |"));
        expect(!text->contains("statesSeen") && !text->contains("| S1 |")) << "arrays and checks without a measure are left out";
        expect(text->contains("0.78 |")) << "1.3 x the 95th percentile of 0.4 and 0.6: " << *text;
        expect(!measure::budgets("/definitely/not/a/real/directory-mcppls-test").has_value());
    };

    "compare: the typing and idle numbers beside a baseline's, and only a difference beyond their spread is worse"_test = [&] {
        const std::string candidate { scratch("compare-candidate") }, baseline { scratch("compare-baseline") };
        const auto write_run = [&](const std::string& directory, int index, double max, double growth, int timeouts) {
            const std::string run { std::format(R"({{"fixture":"ux","checks":[)"
                                                R"({{"id":"U5","measure":{{"completion":{{"max":{},"n":10}},"serverTimeouts":{}}}}},)"
                                                R"({{"id":"U13","measure":{{"rssGrowth":{},"seconds":600}}}}]}})", max, timeouts, growth) };
            (void) os::fs::write(base::join_path(directory, std::format("ux-edits-{}.json", index)), run);
        };
        for (int i { 0 }; i != 4; ++i) {
            write_run(candidate, i, 0.80 + 0.01 * i, 0.30 + 0.01 * i, 0);
            write_run(baseline, i, 0.78 + 0.01 * i, -0.10 + 0.01 * i, 0);
        }
        auto compared = measure::compare(candidate, baseline);
        expect(fatal(compared.has_value()));
        expect(compared->table.contains("| U5 | completion.max | 4 |")) << compared->table;
        expect(compared->table.contains("| U13 | rssGrowth | 4 |")) << compared->table;
        expect(!compared->table.contains("completion.n") && !compared->table.contains("| seconds |")) << "only the compared numbers";
        expect(compared->worse.size() == 1u) << "0.02 s on a 0.8 s max is noise; idle growth +30% against -10% is not";
        expect(!compared->worse.empty() && compared->worse.front().starts_with("U13 rssGrowth")) << (compared->worse.empty() ? "" : compared->worse.front());
        expect(compared->table.contains("| U5 | serverTimeouts | 4 | 0 |")) << "two counts that never vary are the same";
        expect(!measure::compare(candidate, "/definitely/not/a/real/directory-mcppls-test").has_value());
        std::filesystem::remove_all(candidate);
        std::filesystem::remove_all(baseline);
    };

    "an empty directory renders the header alone"_test = [&] {
        const std::string directory { scratch("empty") };
        auto text = measure::summarize(directory);
        expect(text.has_value());
        if (!text) return;
        expect(text->contains("| start |"));
        expect(!text->contains("| cold |"));
        expect(!text->contains("| warm |"));
        std::filesystem::remove_all(directory);
    };

    "cold and warm rows show the median with every value, and summed failures"_test = [&] {
        const std::string directory { scratch("rows") };
        (void) os::fs::write(base::join_path(directory, "cold-1.json"),
                              R"({"ready": 1.0, "first-diagnostics": 2.0, "first-navigation": 3.0, "failures": 0})");
        (void) os::fs::write(base::join_path(directory, "cold-2.json"),
                              R"({"ready": 3.0, "first-diagnostics": 4.0, "first-navigation": 5.0, "failures": 1})");
        (void) os::fs::write(base::join_path(directory, "warm-1.json"),
                              R"({"ready": 0.5, "first-diagnostics": 0.6, "first-navigation": 0.7, "failures": 0})");
        // not cold-*.json / warm-*.json: ignored, the same as timing_summary.py's `kind in runs` guard.
        (void) os::fs::write(base::join_path(directory, "notes.json"), R"({"ready": 99})");

        auto text = measure::summarize(directory);
        expect(text.has_value());
        if (!text) return;
        // median(1.0, 3.0) == 2.0, with both values listed.
        expect(text->contains("| cold | 2.00 s (1.00, 3.00)"));
        expect(text->contains("| 1 |"));   // cold's summed failures
        expect(text->contains("| warm | 0.50 s (0.50)"));
        std::filesystem::remove_all(directory);
    };

    "a missing field in every run of a kind renders n/a for that column"_test = [&] {
        const std::string directory { scratch("na") };
        (void) os::fs::write(base::join_path(directory, "cold-1.json"), R"({"ready": 1.0})");
        auto text = measure::summarize(directory);
        expect(text.has_value());
        if (!text) return;
        expect(text->contains("n/a"));
        std::filesystem::remove_all(directory);
    };

    "not a directory is an error"_test = [&] {
        auto text = measure::summarize("/definitely/not/a/real/directory-mcppls-test");
        expect(!text.has_value());
    };

    "budgets: the median first navigation against each ceiling, and failures always count"_test = [&] {
        const std::string directory { scratch("budget") };
        (void) os::fs::write(base::join_path(directory, "cold-1.json"), R"({"first-navigation": 4.0, "failures": 0})");
        (void) os::fs::write(base::join_path(directory, "cold-2.json"), R"({"first-navigation": 9.0, "failures": 0})");
        (void) os::fs::write(base::join_path(directory, "cold-3.json"), R"({"first-navigation": 5.0, "failures": 0})");
        (void) os::fs::write(base::join_path(directory, "warm-1.json"), R"({"first-navigation": 1.0, "failures": 2})");

        auto within = measure::check_budgets(directory, 6.0, std::nullopt);   // median 5.0
        expect(within.has_value() && within->size() == 1);                   // only warm's failures
        if (within && !within->empty()) expect((*within)[0].contains("warm runs had 2 failed checks"));

        auto over = measure::check_budgets(directory, 4.5, 0.5);
        expect(over.has_value() && over->size() == 3);   // cold median, warm median, warm failures
        std::filesystem::remove_all(directory);
    };

    "a budget for a kind nothing measured is a breach"_test = [&] {
        const std::string directory { scratch("budget-empty") };
        (void) os::fs::write(base::join_path(directory, "cold-1.json"), R"({"first-navigation": 1.0})");
        auto breaches = measure::check_budgets(directory, 10.0, 10.0);
        expect(breaches.has_value() && breaches->size() == 1);
        std::filesystem::remove_all(directory);
    };

    return report();
}
