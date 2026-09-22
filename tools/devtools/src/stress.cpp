module mcppls.devtools.stress;

import std;
import nlohmann.json;
import mcpplibs.cmdline;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.fs;
import mcppls.devtools.common;

namespace mcppls::devtools::stress {
namespace {

namespace fs = mcppls::platform::fs;
using Json = nlohmann::json;

// The fixture that exercises each real, pinned project (real-project stress testing design
// 2026-09-22 §5) -- only the ones this checkout actually carries a fixture for; `matrix` reports
// the rest as a project this checkout has no fixture for, not a silent no-op.
const std::map<std::string, std::string>& project_fixtures() {
    static const std::map<std::string, std::string> KNOWN {
        { "mcpp", "self-mcpp" },
        { "xlings", "real-xlings" },
        { "self", "self-mcppls" },
    };
    return KNOWN;
}

template <class T>
void append_unique(std::vector<T>& into, const T& value) {
    if (std::ranges::find(into, value) == into.end()) into.push_back(value);
}

} // namespace

base::Result<std::vector<RunSpec>> matrix(const std::vector<std::string>& fixtures, const std::vector<std::string>& projects,
                                          const std::vector<std::string>& clients) {
    std::vector<std::string> fixtureNames;
    for (const auto& fixture : fixtures) append_unique(fixtureNames, fixture);
    for (const auto& project : projects) {
        const auto found = project_fixtures().find(project);
        if (found == project_fixtures().end()) {
            return base::fail("stress-project", std::format("{} is not a project this checkout has a fixture for (mcpp, xlings, self)", project));
        }
        append_unique(fixtureNames, found->second);
    }
    if (fixtureNames.empty()) return base::fail("stress-matrix", "no --fixture and no --project: nothing to run");

    std::vector<std::string> clientNames { clients };
    if (clientNames.empty()) clientNames.push_back("none");
    std::vector<std::string> uniqueClients;
    for (const auto& client : clientNames) append_unique(uniqueClients, client);

    std::vector<RunSpec> specs;
    for (const auto& fixture : fixtureNames) {
        for (const auto& client : uniqueClients) specs.push_back({ fixture, client });
    }
    return specs;
}

base::Result<StressRow> extract_row(const std::string& fixture, const std::string& client, const Json& measured) {
    StressRow row { .fixture = fixture, .client = client };
    if (!measured.is_object() || !measured.contains("checks") || !measured["checks"].is_array()) {
        return base::fail("stress-measure", std::format("{} ({}): the --measure JSON has no \"checks\" array", fixture, client));
    }
    const Json* found { nullptr };
    for (const auto& check : measured["checks"]) {
        if (check.is_object() && check.value("kind", std::string {}) == "stress") {
            found = &check;
            break;
        }
    }
    if (found == nullptr) {
        return base::fail("stress-measure", std::format("{} ({}): its scenario has no \"stress\" check", fixture, client));
    }
    row.ok = found->value("ok", false);
    const Json summary = Json::parse(found->value("detail", std::string { "null" }), nullptr, false);
    if (!summary.is_object()) {
        row.note = "the stress check's detail is not the summary object (it may have failed before producing one)";
        return row;
    }
    // Structured bindings over nlohmann's `.items()` do not compile against `import std` here
    // (src/ai/model/schema.cpp hit the same thing): iterate the entries plainly instead.
    for (const auto& entry : summary.value("methods", Json::object()).items()) {
        const Json& counts { entry.value() };
        row.answered += counts.value("answered", 0);
        row.empty += counts.value("empty", 0);
        row.timeout += counts.value("timeout", 0);
        row.error += counts.value("error", 0);
    }
    row.p90 = summary.value("p90", 0.0);
    row.maxStallSeconds = summary.value("maxStallSeconds", 0.0);
    if (summary.contains("cpuSeconds") && summary["cpuSeconds"].is_number()) row.cpuSeconds = summary["cpuSeconds"].get<double>();
    if (summary.contains("rssMB") && summary["rssMB"].is_number()) row.rssMB = summary["rssMB"].get<double>();
    return row;
}

Json to_json(const StressRow& row) {
    Json value { { "fixture", row.fixture }, { "client", row.client }, { "ok", row.ok }, { "answered", row.answered },
                { "empty", row.empty }, { "timeout", row.timeout }, { "error", row.error }, { "p90", row.p90 },
                { "maxStallSeconds", row.maxStallSeconds } };
    value["cpuSeconds"] = row.cpuSeconds ? Json(*row.cpuSeconds) : Json(nullptr);
    value["rssMB"] = row.rssMB ? Json(*row.rssMB) : Json(nullptr);
    if (!row.note.empty()) value["note"] = row.note;
    return value;
}

base::Result<std::vector<StressRow>> rows_from_json(const Json& value) {
    if (!value.is_array()) return base::fail("stress-compare", "expected a JSON array of rows");
    std::vector<StressRow> rows;
    for (const auto& entry : value) {
        if (!entry.is_object()) return base::fail("stress-compare", "a row is not a JSON object");
        StressRow row;
        row.fixture = entry.value("fixture", std::string {});
        row.client = entry.value("client", std::string {});
        row.ok = entry.value("ok", false);
        row.answered = entry.value("answered", 0);
        row.empty = entry.value("empty", 0);
        row.timeout = entry.value("timeout", 0);
        row.error = entry.value("error", 0);
        row.p90 = entry.value("p90", 0.0);
        row.maxStallSeconds = entry.value("maxStallSeconds", 0.0);
        if (entry.contains("cpuSeconds") && entry["cpuSeconds"].is_number()) row.cpuSeconds = entry["cpuSeconds"].get<double>();
        if (entry.contains("rssMB") && entry["rssMB"].is_number()) row.rssMB = entry["rssMB"].get<double>();
        row.note = entry.value("note", std::string {});
        rows.push_back(std::move(row));
    }
    return rows;
}

namespace {

std::string number_or(std::optional<double> value, std::string_view unit) {
    return value ? std::format("{:.1f}{}", *value, unit) : std::string { "n/a" };
}

} // namespace

std::string render_table(const std::vector<StressRow>& rows) {
    std::string out { std::format("{:<28}{:<10}{:>4}{:>4}{:>4}{:>4}   {:>7}{:>9}{:>9}{:>9}\n", "fixture", "client", "ans",
                                  "emp", "to", "err", "p90(s)", "stall(s)", "cpu(s)", "rss(MB)") };
    for (const auto& row : rows) {
        if (!row.note.empty()) {
            out += std::format("{:<28}{:<10}{}\n", row.fixture, row.client, row.note);
            continue;
        }
        out += std::format("{:<28}{:<10}{:>4}{:>4}{:>4}{:>4}   {:>7.2f}{:>9.1f}{:>9}{:>9}\n", row.fixture, row.client, row.answered,
                           row.empty, row.timeout, row.error, row.p90, row.maxStallSeconds, number_or(row.cpuSeconds, ""),
                           number_or(row.rssMB, ""));
        if (!row.ok) out += "  (over budget)\n";
    }
    return out;
}

std::string render_comparison(const std::vector<StressRow>& baseline, const std::vector<StressRow>& current) {
    auto key = [](const StressRow& row) { return row.fixture + "\x1f" + row.client; };
    std::map<std::string, const StressRow*> before;
    for (const auto& row : baseline) before[key(row)] = &row;
    std::string out;
    std::set<std::string> seen;
    for (const auto& row : current) {
        seen.insert(key(row));
        const auto found = before.find(key(row));
        if (found == before.end()) {
            out += std::format("{:<28}{:<10}(new)\n", row.fixture, row.client);
            continue;
        }
        const StressRow& base { *found->second };
        out += std::format("{:<28}{:<10}p90 {:+.2f}s  stall {:+.1f}s  cpu {}  rss {}  timeouts {:+d}\n", row.fixture, row.client,
                           row.p90 - base.p90, row.maxStallSeconds - base.maxStallSeconds,
                           row.cpuSeconds && base.cpuSeconds ? std::format("{:+.1f}s", *row.cpuSeconds - *base.cpuSeconds) : std::string { "n/a" },
                           row.rssMB && base.rssMB ? std::format("{:+.0f}MB", *row.rssMB - *base.rssMB) : std::string { "n/a" },
                           row.timeout - base.timeout);
    }
    for (const auto& [k, row] : before) {
        if (!seen.contains(k)) out += std::format("{:<28}{:<10}(missing from this run)\n", row->fixture, row->client);
    }
    return out;
}

namespace {

std::string sibling_binary(const std::string& serverPath, std::string_view name) {
    return base::join_path(base::parent_path(serverPath), std::format("{}{}", name, base::extension(serverPath) == ".exe" ? ".exe" : ""));
}

} // namespace

} // namespace mcppls::devtools::stress

namespace mcppls::devtools {
namespace {

namespace fs = mcppls::platform::fs;
namespace cmdline = mcpplibs::cmdline;
using Json = nlohmann::json;

std::vector<std::string> multi_values(const cmdline::ParsedArgs& args, std::string_view name) {
    if (auto opt = args.option(name)) return opt->get().values;
    return {};
}

void run_matrix(const cmdline::ParsedArgs& args, bool& handled, int& status) {
    handled = true;
    std::vector<std::string> fixtures { multi_values(args, "fixture") };
    std::vector<std::string> projects { multi_values(args, "project") };
    std::vector<std::string> clients { multi_values(args, "client") };
    auto specs = stress::matrix(fixtures, projects, clients);
    if (!specs) {
        std::println(std::cerr, "stress: {}", specs.error().message);
        status = 2;
        return;
    }
    const std::string payload { args.value("payload").value_or("") };
    if (payload.empty() || !fs::is_directory(payload)) {
        std::println(std::cerr, "stress: --payload DIR is required (a directory from `mcpp run -p devtools -- payload`)");
        status = 2;
        return;
    }
    const std::string root { repository_root() };
    auto server = locate_server(root, { .profile = args.value("profile").value_or("release") });
    if (!server) {
        std::println(std::cerr, "stress: {}", server.error().message);
        status = 1;
        return;
    }
    const std::string conformance { stress::sibling_binary(*server, "mcppls-conformance") };
    if (!fs::is_regular_file(conformance)) {
        std::println(std::cerr, "stress: no mcppls-conformance beside {}", *server);
        status = 1;
        return;
    }
    const std::string measureDir { args.value("measure-dir").value_or("") };
    if (!measureDir.empty()) (void)fs::create_directories(measureDir);

    std::vector<stress::StressRow> rows;
    bool anyFailed { false };
    for (const auto& spec : *specs) {
        const std::string fixtureDir { base::join_path(root, "conformance/fixtures/" + spec.fixture) };
        if (!fs::is_directory(fixtureDir)) {
            rows.push_back({ .fixture = spec.fixture, .client = spec.client, .ok = false,
                            .note = std::format("no fixture at {}", fixtureDir) });
            anyFailed = true;
            continue;
        }
        const std::string measureFile { measureDir.empty()
                                       ? base::join_path(fs::current_directory(), std::format(".stress-{}-{}.json", spec.fixture, spec.client))
                                       : base::join_path(measureDir, std::format("{}-{}.json", spec.fixture, spec.client)) };
        std::vector<std::string> arguments { "run", "--server", *server, "--payload", payload, "--fixture", fixtureDir,
                                            "--measure", measureFile, "--timeout", args.value("timeout").value_or("180") };
        if (spec.client != "none") {
            arguments.push_back("--client");
            arguments.push_back(spec.client);
        }
        if (auto seed = args.value("seed")) {
            arguments.push_back("--stress-seed");
            arguments.push_back(*seed);
        }
        std::println("== {} ({}) ==", spec.fixture, spec.client);
        // mcppls-conformance exits 1 when ANY check in the fixture fails, the stress one or
        // another; it still writes --measure either way, so that exit code alone is not grounds
        // to throw the measurement away -- only a --measure file that truly was not written is.
        auto output = capture(conformance, arguments, root, std::chrono::minutes { 10 });
        auto measured = fs::read_file(measureFile);
        if (!measured) {
            rows.push_back({ .fixture = spec.fixture, .client = spec.client, .ok = false,
                            .note = output ? "no --measure file was written"
                                          : std::format("mcppls-conformance failed: {}", output.error().message) });
            anyFailed = true;
            continue;
        }
        const Json parsed = Json::parse(*measured, nullptr, false);
        auto row = stress::extract_row(spec.fixture, spec.client, parsed);
        if (!row) {
            rows.push_back({ .fixture = spec.fixture, .client = spec.client, .ok = false, .note = row.error().message });
            anyFailed = true;
        } else {
            if (!row->ok) anyFailed = true;
            rows.push_back(*row);
        }
        if (measureDir.empty()) (void)fs::remove_all(measureFile);
    }

    std::println("\n{}", stress::render_table(rows));

    if (!measureDir.empty()) {
        Json combined = Json::array();
        for (const auto& row : rows) combined.push_back(stress::to_json(row));
        (void)fs::write_file(base::join_path(measureDir, "summary.json"), combined.dump(2) + "\n");
    }

    if (auto compareWith = args.value("compare")) {
        auto text = fs::read_file(*compareWith);
        if (!text) {
            std::println(std::cerr, "stress: cannot read {}: {}", *compareWith, text.error().message);
            status = anyFailed ? 1 : status;
            return;
        }
        const Json baselineJson = Json::parse(*text, nullptr, false);
        auto baseline = stress::rows_from_json(baselineJson);
        if (!baseline) {
            std::println(std::cerr, "stress: {}: {}", *compareWith, baseline.error().message);
        } else {
            std::println("compared with {}:\n{}", *compareWith, stress::render_comparison(*baseline, rows));
        }
    }

    status = anyFailed ? 1 : 0;
}

} // namespace

cmdline::App stress_command(bool& handled, int& status) {
    cmdline::App command { "stress" };
    (void)command.description("Real-project stress testing: run the conformance runner's stress check over a matrix of fixtures and client profiles");
    (void)command.option("fixture").takes_value().multiple().help("A fixture under conformance/fixtures to stress; repeatable");
    (void)command.option("project").takes_value().multiple().help("xlings, mcpp or self: the pinned real-project fixture for it, when this checkout has one; repeatable");
    (void)command.option("client").takes_value().multiple().help("vscode, neovim, zed or plain; repeatable. Omitted: this runner's own default profile");
    (void)command.option("seed").takes_value().help("Overrides every stressed fixture's own stress-check seed, for a reproducible matrix run");
    (void)command.option("payload").takes_value().help("A payload directory (mcpp run -p devtools -- payload)");
    (void)command.option("profile").takes_value().help("The server build profile to stress (default: release)");
    (void)command.option("timeout").takes_value().help("Seconds each fixture's checks may take (default: 180)");
    (void)command.option("measure-dir").takes_value().help("Directory to write each run's --measure JSON and a combined summary.json into");
    (void)command.option("compare").takes_value().help("A summary.json from an earlier run: prints deltas against it");
    (void)command.action([&handled, &status](const cmdline::ParsedArgs& args) { run_matrix(args, handled, status); });
    return command;
}

} // namespace mcppls::devtools
