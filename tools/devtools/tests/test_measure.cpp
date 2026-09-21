// timing_summary.py, ported: medians over cold-*.json / warm-*.json in a directory.
import std;
import mcppls.testing;
import mcppls.base.path;
import mcppls.platform.fs;
import mcppls.platform.dirs;
import mcppls.devtools.measure;

namespace measure = mcppls::devtools::measure;
namespace fs = mcppls::platform::fs;
namespace base = mcppls::base;

namespace {

std::string scratch(std::string_view name) {
    const std::string directory { base::join_path(
        mcppls::platform::dirs::temp_directory(),
        std::format("mcppls-test-measure-{}-{}", name, std::chrono::steady_clock::now().time_since_epoch().count())) };
    (void) fs::create_directories(directory);
    return directory;
}

} // namespace

int main() {
    using namespace mcppls::testing;

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
        (void) fs::write_file(base::join_path(directory, "cold-1.json"),
                              R"({"ready": 1.0, "first-diagnostics": 2.0, "first-navigation": 3.0, "failures": 0})");
        (void) fs::write_file(base::join_path(directory, "cold-2.json"),
                              R"({"ready": 3.0, "first-diagnostics": 4.0, "first-navigation": 5.0, "failures": 1})");
        (void) fs::write_file(base::join_path(directory, "warm-1.json"),
                              R"({"ready": 0.5, "first-diagnostics": 0.6, "first-navigation": 0.7, "failures": 0})");
        // not cold-*.json / warm-*.json: ignored, the same as timing_summary.py's `kind in runs` guard.
        (void) fs::write_file(base::join_path(directory, "notes.json"), R"({"ready": 99})");

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
        (void) fs::write_file(base::join_path(directory, "cold-1.json"), R"({"ready": 1.0})");
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
        (void) fs::write_file(base::join_path(directory, "cold-1.json"), R"({"first-navigation": 4.0, "failures": 0})");
        (void) fs::write_file(base::join_path(directory, "cold-2.json"), R"({"first-navigation": 9.0, "failures": 0})");
        (void) fs::write_file(base::join_path(directory, "cold-3.json"), R"({"first-navigation": 5.0, "failures": 0})");
        (void) fs::write_file(base::join_path(directory, "warm-1.json"), R"({"first-navigation": 1.0, "failures": 2})");

        auto within = measure::check_budgets(directory, 6.0, std::nullopt);   // median 5.0
        expect(within.has_value() && within->size() == 1);                   // only warm's failures
        if (within && !within->empty()) expect((*within)[0].contains("warm runs had 2 failed checks"));

        auto over = measure::check_budgets(directory, 4.5, 0.5);
        expect(over.has_value() && over->size() == 3);   // cold median, warm median, warm failures
        std::filesystem::remove_all(directory);
    };

    "a budget for a kind nothing measured is a breach"_test = [&] {
        const std::string directory { scratch("budget-empty") };
        (void) fs::write_file(base::join_path(directory, "cold-1.json"), R"({"first-navigation": 1.0})");
        auto breaches = measure::check_budgets(directory, 10.0, 10.0);
        expect(breaches.has_value() && breaches->size() == 1);
        std::filesystem::remove_all(directory);
    };

    return report();
}
