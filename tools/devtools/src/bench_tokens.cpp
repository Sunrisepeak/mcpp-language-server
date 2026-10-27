module mcppls.devtools.bench.tokens;

import std;
import nlohmann.json;
import mcpplibs.cmdline;
import mcppls.base.path;
import mcppls.platform.dirs;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.devtools.common;

namespace mcppls::devtools::bench {

namespace {

namespace fs = mcppls::platform::fs;
using Json = nlohmann::json;

std::optional<double> median(std::vector<double> values) {
    if (values.empty()) return std::nullopt;
    std::ranges::sort(values);
    const std::size_t n { values.size() };
    return n % 2 == 1 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2.0;
}

std::string shown(const Json& value, std::string_view metric) {
    if (!value.is_number()) return "-";
    const double number { value.get<double>() };
    if (metric.ends_with("Bytes")) return std::format("{:.0f}", number);
    if (metric == "refreshes" || metric == "polls" || metric == "missedRefreshes") return std::format("{:.0f}", number);
    return std::format("{:.2f} s", number);
}

// The tokens-latency check's measure in a conformance --measure file, or null.
Json tokens_measure(const Json& measured) {
    if (!measured.is_object()) return nullptr;
    for (const auto& check : measured.value("checks", Json::array())) {
        if (check.is_object() && check.contains("measure") && check["measure"].is_object() && check["measure"].contains("firstTokens")) return check["measure"];
    }
    return nullptr;
}

} // namespace

Json token_medians(const Json& rounds) {
    Json medians = Json::object();
    for (const std::string stage : { "cold", "warm" }) {
        Json one = Json::object();
        for (const auto metric : TOKEN_METRICS) {
            std::vector<double> values;
            for (const auto& round : rounds) {
                if (!round.is_object()) continue;
                const auto at = round.find(stage);
                const Json* measure { at == round.end() ? nullptr : &*at };
                if (measure == nullptr || !measure->is_object()) continue;
                const auto found = measure->find(std::string { metric });
                if (found != measure->end() && found->is_number()) values.push_back(found->get<double>());
            }
            const auto value = median(std::move(values));
            one[std::string { metric }] = value ? Json(*value) : Json(nullptr);
        }
        medians[stage] = std::move(one);
    }
    return medians;
}

std::string token_table(const Json& medians, const Json& baseline) {
    const bool compared { baseline.is_object() };
    std::string out { compared ? "| metric | cold | cold (baseline) | warm | warm (baseline) |\n|---|---|---|---|---|\n"
                               : "| metric | cold | warm |\n|---|---|---|\n" };
    const auto cell = [](const Json& of, const std::string& stage, std::string_view metric) {
        if (!of.is_object()) return std::string { "-" };
        const auto at = of.find(stage);
        if (at == of.end() || !at->is_object()) return std::string { "-" };
        return shown(at->value(std::string { metric }, Json(nullptr)), metric);
    };
    for (const auto metric : TOKEN_METRICS) {
        if (compared) {
            out += std::format("| {} | {} | {} | {} | {} |\n", metric, cell(medians, "cold", metric), cell(baseline, "cold", metric),
                               cell(medians, "warm", metric), cell(baseline, "warm", metric));
        } else {
            out += std::format("| {} | {} | {} |\n", metric, cell(medians, "cold", metric), cell(medians, "warm", metric));
        }
    }
    return out;
}

int command_tokens(const mcpplibs::cmdline::ParsedArgs& args) {
    const std::string server { args.value("server").value_or("") };
    const std::string payload { args.value("payload").value_or("") };
    if (server.empty() || !fs::is_regular_file(server) || payload.empty() || !fs::is_directory(payload)) {
        std::println(std::cerr, "bench tokens: --server PATH and --payload DIR are required");
        return 2;
    }
    const std::string root { repository_root() };
    const std::string suffix { base::extension(server) == ".exe" ? ".exe" : "" };
    std::string conformance { args.value("conformance").value_or("") };
    if (conformance.empty()) {
        // Beside this program first (the build that has the runner), then beside the server.
        const auto self = mcppls::platform::env::self_command();
        if (!self.empty()) conformance = base::join_path(base::parent_path(self.back()), "mcppls-conformance" + suffix);
        if (!fs::is_regular_file(conformance)) conformance = base::join_path(base::parent_path(server), "mcppls-conformance" + suffix);
    }
    if (!fs::is_regular_file(conformance)) {
        std::println(std::cerr, "bench tokens: no mcppls-conformance (say --conformance PATH)");
        return 2;
    }
    const int rounds { std::max(1, std::atoi(args.value("rounds").value_or("3").c_str())) };
    const std::string fixture { base::join_path(root, "conformance/fixtures/tokens-large-file") };
    const std::string scratch { base::join_path(mcppls::platform::dirs::temp_directory(),
                                                std::format("mcppls-bench-tokens-{}", std::chrono::system_clock::now().time_since_epoch().count())) };
    Json measured = Json::array();
    std::vector<std::string> failed;
    for (int round { 0 }; round < rounds; ++round) {
        const std::string work { base::join_path(scratch, std::format("round-{}", round)) };
        (void)fs::create_directories(work);
        Json one = Json::object();
        for (const std::string_view stage : { "cold", "warm" }) {
            const std::string measureFile { base::join_path(work, std::format("{}.json", stage)) };
            std::vector<std::string> arguments { "run", "--server", server, "--payload", payload, "--fixture", fixture,
                                                 "--workspace-dir", base::join_path(work, "workspace"), "--cache-dir", base::join_path(work, "cache"),
                                                 "--stage", std::string { stage }, "--timeout", args.value("timeout").value_or("300"),
                                                 "--measure", measureFile };
            if (stage == "warm") arguments.push_back("--expect-warm");
            std::println("== round {} {} ==", round + 1, stage);
            // The runner exits 1 when a budget is over; the measure is written either way.
            (void)capture(conformance, arguments, root, std::chrono::minutes { 15 });
            auto text = fs::read_file(measureFile);
            const Json parsed = text ? Json::parse(*text, nullptr, false) : Json(nullptr);
            one[std::string { stage }] = tokens_measure(parsed);
            if (!parsed.is_object()) failed.push_back(std::format("round {} {}: no measure was written", round + 1, stage));
            for (const auto& check : parsed.is_object() ? parsed.value("checks", Json::array()) : Json::array()) {
                if (!check.value("ok", false)) {
                    failed.push_back(std::format("round {} {}: {} {}", round + 1, stage, check.value("id", std::string {}), check.value("detail", std::string {})));
                }
            }
        }
        measured.push_back(std::move(one));
    }
    const Json medians = token_medians(measured);
    Json baseline = nullptr;
    if (auto compareWith = args.value("compare")) {
        if (auto text = fs::read_file(*compareWith)) baseline = Json::parse(*text, nullptr, false).value("medians", Json(nullptr));
        else std::println(std::cerr, "bench tokens: cannot read {}", *compareWith);
    }
    std::println("\n{}", token_table(medians, baseline));
    if (auto out = args.value("out")) {
        (void)fs::write_file(*out, Json { { "server", server }, { "rounds", measured }, { "medians", medians } }.dump(2) + "\n");
    }
    if (!args.is_flag_set("keep")) (void)fs::remove_all(scratch);
    for (const auto& failure : failed) std::println(std::cerr, "bench tokens: {}", failure);
    return failed.empty() ? 0 : 1;
}

} // namespace mcppls::devtools::bench
