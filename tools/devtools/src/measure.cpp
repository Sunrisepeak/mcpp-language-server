module mcppls.devtools.measure;

import std;
import mcpplibs.cmdline;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.devtools.common;
import mcpplibs.os;

namespace os = mcpplibs::os;

namespace mcppls::devtools::measure {
namespace {
double median_of(std::vector<double> values) {
    std::ranges::sort(values);
    const std::size_t n { values.size() };
    if (n % 2 == 1) return values[n / 2];
    return (values[n / 2 - 1] + values[n / 2]) / 2.0;
}

} // namespace

namespace {

// Every number in `value`, by its dotted path from the top (arrays are left out: their elements have no name).
void numbers_of(const nlohmann::json& value, const std::string& path, std::map<std::string, double>& into) {
    if (value.is_number() && !value.is_boolean()) {
        into[path] = value.get<double>();
    } else if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) numbers_of(it.value(), path.empty() ? it.key() : path + "." + it.key(), into);
    }
}

void measure_files(const std::string& directory, std::vector<std::string>& into) {
    std::error_code ignored;
    for (const auto& path : base::children(directory)) {
        if (std::filesystem::is_directory(path, ignored)) measure_files(path, into);
        else if (base::extension(path) == ".json" && std::filesystem::is_regular_file(path, ignored)) into.push_back(path);
    }
}

double percentile_of(std::vector<double> values, double fraction) {
    std::ranges::sort(values);
    const std::size_t index { std::min(values.size() - 1, static_cast<std::size_t>(fraction * static_cast<double>(values.size()))) };
    return values[index];
}

} // namespace

namespace {

using Samples = std::map<std::pair<std::string, std::string>, std::vector<double>>;   // (check, number) -> one per run

// Every number of every check's `measure` in the --measure files under `directory`, at any depth.
base::Result<Samples> collect(const std::string& directory) {
    std::error_code ignored;
    if (!std::filesystem::is_directory(directory, ignored)) return base::fail("measure-dir", std::format("{} is not a directory", directory));
    std::vector<std::string> files;
    measure_files(directory, files);
    Samples values;
    for (const auto& path : files) {
        auto text = os::fs::read(path);
        if (!text) return base::fail("read-file", text.error());
        nlohmann::json run;
        try {
            run = nlohmann::json::parse(*text);
        } catch (const std::exception& error) {
            return base::fail("measure-json", std::format("{} is not valid JSON: {}", path, error.what()));
        }
        if (!run.is_object() || !run.contains("checks") || !run["checks"].is_array()) continue;   // not a --measure file
        for (const auto& check : run["checks"]) {
            if (!check.is_object() || !check.contains("measure")) continue;
            std::map<std::string, double> numbers;
            numbers_of(check["measure"], "", numbers);
            for (const auto& [name, number] : numbers) values[{ check.value("id", std::string {}), name }].push_back(number);
        }
    }
    return values;
}

double deviation_of(const std::vector<double>& values) {
    if (values.size() < 2) return 0;
    double mean { 0 };
    for (const double value : values) mean += value;
    mean /= static_cast<double>(values.size());
    double squares { 0 };
    for (const double value : values) squares += (value - mean) * (value - mean);
    return std::sqrt(squares / static_cast<double>(values.size() - 1));
}

// The numbers a release is compared on: what a person waits for while typing and saving, how often the state
// changes, what an idle server costs, and requests that were never answered. Every other number is in `budgets`.
constexpr std::array<std::string_view, 9> COMPARED { "completion.p95", "completion.max", "importer.completion.max", "busiestMinuteChanges",
                                                     "rssGrowth", "idleCpuCores", "serverTimeouts", "clientTimeouts", "republishSeconds" };

} // namespace

base::Result<std::string> budgets(const std::string& directory) {
    auto values = collect(directory);
    if (!values) return std::unexpected { values.error() };
    std::string out { "| check | measure | runs | p50 | p95 | max | p95 x 1.3 |\n|---|---|---|---|---|---|---|\n" };
    for (const auto& [key, measured] : *values) {
        const double p95 { percentile_of(measured, 0.95) };
        out += std::format("| {} | {} | {} | {:.3g} | {:.3g} | {:.3g} | {:.3g} |\n", key.first, key.second, measured.size(), median_of(measured), p95,
                           std::ranges::max(measured), p95 * 1.3);
    }
    return out;
}

base::Result<Comparison> compare(const std::string& candidate, const std::string& baseline) {
    auto ours = collect(candidate);
    if (!ours) return std::unexpected { ours.error() };
    auto theirs = collect(baseline);
    if (!theirs) return std::unexpected { theirs.error() };
    Comparison result { "| check | measure | runs | median | max | baseline runs | baseline median | baseline max | |\n"
                        "|---|---|---|---|---|---|---|---|---|\n", {} };
    for (const auto& [key, measured] : *ours) {
        if (std::ranges::find(COMPARED, key.second) == COMPARED.end()) continue;
        const auto other = theirs->find(key);
        if (other == theirs->end()) continue;
        const double median { median_of(measured) }, otherMedian { median_of(other->second) };
        // Worse: a median above the baseline's by more than twice their pooled deviation, and by more than a tenth of it, so
        // that two counts that never vary (0 and 0) are the same and a noise-free 0 against 1 is not called noise.
        const double pooled { std::sqrt((std::pow(deviation_of(measured), 2) + std::pow(deviation_of(other->second), 2)) / 2) };
        const bool worse { median > otherMedian + std::max(2 * pooled, 0.1 * std::abs(otherMedian)) };
        if (worse) result.worse.push_back(std::format("{} {}: median {:.3g} against {:.3g}", key.first, key.second, median, otherMedian));
        result.table += std::format("| {} | {} | {} | {:.3g} | {:.3g} | {} | {:.3g} | {:.3g} | {} |\n", key.first, key.second, measured.size(), median,
                                    std::ranges::max(measured), other->second.size(), otherMedian, std::ranges::max(other->second),
                                    worse ? "worse" : "");
    }
    return result;
}

} // namespace mcppls::devtools::measure

namespace mcppls::devtools {
namespace {

namespace cmdline = mcpplibs::cmdline;

// See mcppls.devtools.check's `dispatch` for why this second level of dispatch is done by hand:
// `cmdline::App::run` calls only the immediate matched subcommand's action, and `measure` itself
// (the App main.cpp registers) has none of its own.
int command_measure_budgets(const cmdline::ParsedArgs& arguments) {
    const std::string directory { arguments.positional_or(0, "") };
    if (directory.empty()) {
        std::println(std::cerr, "mcppls-devtools: measure budgets needs a directory");
        return 2;
    }
    auto text = measure::budgets(directory);
    if (!text) {
        std::println(std::cerr, "mcppls-devtools: {}", text.error().message);
        return 1;
    }
    std::print("{}", *text);
    return 0;
}

int command_measure_compare(const cmdline::ParsedArgs& arguments) {
    const std::string candidate { arguments.positional_or(0, "") }, baseline { arguments.positional_or(1, "") };
    if (candidate.empty() || baseline.empty()) {
        std::println(std::cerr, "mcppls-devtools: measure compare needs two directories");
        return 2;
    }
    auto compared = measure::compare(candidate, baseline);
    if (!compared) {
        std::println(std::cerr, "mcppls-devtools: {}", compared.error().message);
        return 1;
    }
    std::print("{}", compared->table);
    for (const auto& worse : compared->worse) std::println(std::cerr, "mcppls-devtools: worse than the baseline: {}", worse);
    return arguments.is_flag_set("fail-worse") && !compared->worse.empty() ? 1 : 0;
}

int dispatch(bool& handled, int& status, std::string_view verb, const cmdline::ParsedArgs& inner) {
    handled = true;
    if (verb == "budgets") return command_measure_budgets(inner);
    if (verb == "compare") return command_measure_compare(inner);
    std::println(std::cerr, "mcppls-devtools: measure needs a verb: budgets or compare");
    return 2;
}

} // namespace

cmdline::App measure_command(bool& handled, int& status) {
    cmdline::App command { "measure" };
    (void) command.description("Summarize conformance --measure files: the scenarios' distributions for their budgets, and beside a baseline");
    (void) command.subcommand("budgets")
        .description("Each check's measured numbers over every --measure file under a directory: runs, p50, p95, max, p95 x 1.3")
        .arg("directory").required().help("Directory of --measure files, at any depth (nightly's ux artifacts)");
    (void) command.subcommand("compare")
        .description("The typing, save, state-change, idle and timeout numbers of two directories of --measure files, side by side")
        .option("fail-worse").help("Fail when a median is worse than the baseline's beyond their spread")
        .arg("candidate").required().help("Directory of the candidate's --measure files")
        .arg("baseline").required().help("Directory of the baseline's, measured on the same machines (nightly's ux-baseline)");
    (void) command.action([&handled, &status](const cmdline::ParsedArgs& arguments) {
        const auto sub = arguments.subcommand();
        static const cmdline::ParsedArgs EMPTY {};
        status = dispatch(handled, status, arguments.subcommand_name(), sub ? sub->get() : EMPTY);
    });
    return command;
}

} // namespace mcppls::devtools
