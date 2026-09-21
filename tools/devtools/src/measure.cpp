module mcppls.devtools.measure;

import std;
import mcpplibs.cmdline;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.fs;
import mcppls.devtools.common;

namespace mcppls::devtools::measure {
namespace {

namespace fs = mcppls::platform::fs;

constexpr std::array<std::string_view, 3> FIELDS { "ready", "first-diagnostics", "first-navigation" };

double median_of(std::vector<double> values) {
    std::ranges::sort(values);
    const std::size_t n { values.size() };
    if (n % 2 == 1) return values[n / 2];
    return (values[n / 2 - 1] + values[n / 2]) / 2.0;
}

// The "cold" or "warm" this file's name starts with, or empty when the name has no '-' at all.
std::string_view kind_of(std::string_view stem) {
    const auto dash = stem.find('-');
    return dash == std::string_view::npos ? stem : stem.substr(0, dash);
}

using Runs = std::vector<std::pair<std::string, std::vector<nlohmann::json>>>;

// Insertion order matters: the table's rows are cold, then warm, in that order, and a kind
// with no files is skipped rather than printed empty -- the same as Python's dict iteration.
base::Result<Runs> load_runs(const std::string& directory) {
    if (!fs::is_directory(directory)) {
        return base::fail("measure-dir", std::format("{} is not a directory", directory));
    }
    Runs runs { { "cold", {} }, { "warm", {} } };

    // `list_directory` already returns each entry joined onto `directory` (mcppls.platform.fs's
    // documented shape), so `path` IS the entry -- joining it onto `directory` again would look
    // for a nested path that does not exist and silently skip every file.
    for (const auto& path : fs::list_directory(directory)) {
        if (base::extension(path) != ".json") continue;
        if (!fs::is_regular_file(path)) continue;
        const std::string_view name { base::file_name(path) };
        const std::string_view stem { name.substr(0, name.size() - std::string_view { ".json" }.size()) };
        const std::string_view kind { kind_of(stem) };
        auto found = std::ranges::find_if(runs, [&](const auto& entry) { return entry.first == kind; });
        if (found == runs.end()) continue;

        auto text = fs::read_file(path);
        if (!text) return std::unexpected { text.error() };
        try {
            found->second.push_back(nlohmann::json::parse(*text));
        } catch (const std::exception& error) {
            return base::fail("measure-json", std::format("{} is not valid JSON: {}", path, error.what()));
        }
    }
    return runs;
}

std::vector<double> values_of(const std::vector<nlohmann::json>& measured, std::string_view field) {
    std::vector<double> values;
    for (const auto& entry : measured) {
        if (entry.contains(field) && entry[field].is_number()) values.push_back(entry[field].get<double>());
    }
    return values;
}

long long failures_of(const std::vector<nlohmann::json>& measured) {
    long long failures { 0 };
    for (const auto& entry : measured) {
        if (entry.contains("failures") && entry["failures"].is_number()) failures += entry["failures"].get<long long>();
    }
    return failures;
}

} // namespace

base::Result<std::vector<std::string>> check_budgets(const std::string& directory, std::optional<double> coldSeconds,
                                                      std::optional<double> warmSeconds) {
    auto runs = load_runs(directory);
    if (!runs) return std::unexpected { runs.error() };
    std::vector<std::string> breaches;
    for (const auto& [kind, measured] : *runs) {
        if (const long long failures = failures_of(measured); failures != 0) {
            breaches.push_back(std::format("{} runs had {} failed checks", kind, failures));
        }
        const auto budget = kind == "cold" ? coldSeconds : warmSeconds;
        if (!budget) continue;
        const auto values = values_of(measured, "first-navigation");
        if (values.empty()) {
            breaches.push_back(std::format("no {} run measured first-navigation", kind));
            continue;
        }
        if (const double median = median_of(values); median > *budget) {
            breaches.push_back(std::format("{} first navigation took {:.2f} s (median of {}), over the {:.2f} s budget",
                                           kind, median, values.size(), *budget));
        }
    }
    return breaches;
}

base::Result<std::string> summarize(const std::string& directory) {
    auto loaded = load_runs(directory);
    if (!loaded) return std::unexpected { loaded.error() };
    const Runs& runs { *loaded };

    std::string out;
    out += "| start | " + base::join(std::vector<std::string> { FIELDS.begin(), FIELDS.end() }, " | ") + " | failures |\n";
    out += "|---|";
    for (std::size_t i { 0 }; i < FIELDS.size() + 1; ++i) out += "---|";
    out += "\n";

    for (const auto& [kind, measured] : runs) {
        if (measured.empty()) continue;
        std::vector<std::string> cells;
        for (const auto field : FIELDS) {
            const auto values = values_of(measured, field);
            if (values.empty()) {
                cells.push_back("n/a");
                continue;
            }
            std::string joined;
            for (std::size_t i { 0 }; i < values.size(); ++i) {
                if (i != 0) joined += ", ";
                joined += std::format("{:.2f}", values[i]);
            }
            cells.push_back(std::format("{:.2f} s ({})", median_of(values), joined));
        }
        const long long failures { failures_of(measured) };
        out += "| " + std::string { kind } + " | " + base::join(cells, " | ") + std::format(" | {} |\n", failures);
    }
    return out;
}

} // namespace mcppls::devtools::measure

namespace mcppls::devtools {
namespace {

namespace cmdline = mcpplibs::cmdline;

int command_measure_summary(const cmdline::ParsedArgs& arguments) {
    const std::string directory { arguments.positional_or(0, "") };
    if (directory.empty()) {
        std::println(std::cerr, "mcppls-devtools: measure summary needs a directory");
        return 2;
    }
    auto text = measure::summarize(directory);
    if (!text) {
        std::println(std::cerr, "mcppls-devtools: {}", text.error().message);
        return 1;
    }
    std::print("{}", *text);

    const auto seconds = [&](std::string_view name) -> std::optional<double> {
        const auto given = arguments.value(name);
        if (!given || given->empty()) return std::nullopt;
        double value { 0 };
        const auto [end, error] = std::from_chars(given->data(), given->data() + given->size(), value);
        if (error != std::errc {} || end != given->data() + given->size()) return -1.0;
        return value;
    };
    const auto cold = seconds("max-cold");
    const auto warm = seconds("max-warm");
    if ((cold && *cold < 0) || (warm && *warm < 0)) {
        std::println(std::cerr, "mcppls-devtools: --max-cold and --max-warm take seconds");
        return 2;
    }
    if (!cold && !warm) return 0;
    auto breaches = measure::check_budgets(directory, cold, warm);
    if (!breaches) {
        std::println(std::cerr, "mcppls-devtools: {}", breaches.error().message);
        return 1;
    }
    for (const auto& breach : *breaches) std::println(std::cerr, "mcppls-devtools: over budget: {}", breach);
    return breaches->empty() ? 0 : 1;
}

// See mcppls.devtools.check's `dispatch` for why this second level of dispatch is done by hand:
// `cmdline::App::run` calls only the immediate matched subcommand's action, and `measure` itself
// (the App main.cpp registers) never had one -- only `summary` did, one level too deep for the
// library to ever reach.
int dispatch(bool& handled, int& status, std::string_view verb, const cmdline::ParsedArgs& inner) {
    handled = true;
    if (verb == "summary") return command_measure_summary(inner);
    std::println(std::cerr, "mcppls-devtools: measure needs a verb: summary");
    return 2;
}

} // namespace

cmdline::App measure_command(bool& handled, int& status) {
    cmdline::App command { "measure" };
    (void) command.description("Cold-start timing: summarize conformance --measure files");
    (void) command.subcommand("summary")
        .description("Medians of ready / first-diagnostics / first-navigation over a directory of runs")
        .option("max-cold").takes_value().help("Fail when the median cold first navigation takes longer (seconds), or any run failed a check")
        .option("max-warm").takes_value().help("The same for warm starts")
        .arg("directory").required().help("Directory of cold-<round>.json / warm-<round>.json");
    (void) command.action([&handled, &status](const cmdline::ParsedArgs& arguments) {
        const auto sub = arguments.subcommand();
        static const cmdline::ParsedArgs EMPTY {};
        status = dispatch(handled, status, arguments.subcommand_name(), sub ? sub->get() : EMPTY);
    });
    return command;
}

} // namespace mcppls::devtools
