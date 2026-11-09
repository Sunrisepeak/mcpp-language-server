// The start of a session (usable plan W7): the timing fixture opened cold, then warm on the cache
// the cold start left. A warm start reuses the std module the cold one built (the fixture's SC4,
// a check of its own); how soon each start is ready, has its first diagnostics and answers its
// first navigation are metrics, the median of the rounds a case makes. The first navigation of
// each start is bounded: measured first, 6.2 / 4.5 / 8.7 s cold and 1.8 / 1.9 / 3.5 s warm on the
// Linux, macOS and Windows runners (nightly medians 2026-09-14: 5.5 / 4.4 / 8.5 and 2.0 / 1.9 / 2.9).
import std;
import nlohmann.json;
import mcpplibs.mtest;
import mcppls.harness;

using namespace mcpplibs::mtest;
using mcppls::harness::on;

namespace {

double median(std::vector<double> values) {
    std::ranges::sort(values);
    const auto n = values.size();
    return n % 2 == 1 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2.0;
}

using timeline = std::map<std::string, std::vector<double>>;

// The cold and the warm start of `fixture` on one cache under `home`, their times added to `into`.
void cold_and_warm(std::string_view fixture, const std::filesystem::path& home, int timeout, timeline& into) {
    for (const bool warm : {false, true}) {
        const auto measured = mcppls::harness::run({.fixture = std::string(fixture), .warm = warm, .home = home, .timeout = timeout});
        for (const auto* field : {"ready", "first-diagnostics", "first-navigation"})
            if (measured.is_object() && measured.contains(field) && measured[field].is_number())
                into[std::format("{}.{}", warm ? "warm" : "cold", field)].push_back(measured[field].get<double>());
    }
}

// `rounds` cold and warm starts of the timing fixture, each pair on a cache of its own; the
// medians are the metrics.
void starts(int rounds) {
    timeline times;
    for (int round = 0; round < rounds; ++round)
        cold_and_warm("timing", temp_dir() / std::format("round-{}", round + 1), 120, times);
    for (const auto& [name, values] : times)
        metric(name, median(values), "s", values);
}

// One start of each kind: a regression that makes a start severe fails a pull request.
Bench startup{"startup", on{"linux-x64", "darwin-arm64", "win32-x64"},
              at_most{"cold.first-navigation", 15}, at_most{"warm.first-navigation", 5}, [] { starts(1); }};

// Five of each, for a release: the median decides, so one slow runner does not. 12 s cold and 5 s
// warm is room for a slow runner, not for a regression that doubles a start.
Bench startup_median{"startup, the median of five", on{"linux-x64", "darwin-arm64", "win32-x64"}, tag{"release"},
                     at_most{"cold.first-navigation", 12}, at_most{"warm.first-navigation", 5}, [] { starts(5); }};

// A project the size where module checks cost (0.0.14 Windows plan, phase 0): thirty module units
// with <windows.h>, built by MSVC (MCPPLS_CTS_MSVC_ENV), opened cold and then warm on one cache.
Bench large_modules{"a large module project", on{"win32-x64"}, tag{"release"}, [] {
    timeline times;
    cold_and_warm("ux-modules-windows-large", temp_dir(), 900, times);
    for (const auto& [name, values] : times)
        metric(name, values.front(), "s");
}};

}  // namespace
