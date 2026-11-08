// The scenarios' numbers in directories of --measure files: their distributions, and a candidate's
// beside a baseline's.
import std;
import mcpplibs.mtest;
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

using namespace mcpplibs::mtest;

UTest measure_tests{"measure", [] {

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

}};
