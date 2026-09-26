module mcppls.devtools.bench.review;

import std;
import nlohmann.json;
import mcpplibs.cmdline;
import mcppls.os;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.sha256;
import mcppls.base.text;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.platform.process;
import mcppls.platform.toolrun;
import mcppls.devtools.common;

namespace mcppls::devtools::bench {
namespace {

namespace fs = mcppls::platform::fs;
namespace env = mcppls::platform::env;
namespace toolrun = mcppls::platform::toolrun;
namespace cmdline = mcpplibs::cmdline;

constexpr std::string_view REVIEW_SUBDIR { "tools/bench/review" };

std::string host_name() {
    switch (mcppls::os::FAMILY) {
    case mcppls::os::Family::windows: return "windows";
    case mcppls::os::Family::macos: return "macos";
    default: return "linux";
    }
}

base::Result<void> git_run(const std::string& workspace, std::vector<std::string> arguments) {
    auto git = env::find_executable("git");
    if (!git) return base::fail("bench-review-git", "git is not on PATH");
    const std::string joined { base::join(arguments, " ") };
    // No background maintenance or gc: a recent git detaches it after a commit, and its lock file coming and
    // going under .git/ read as the review having changed the workspace (CI, 2026-09-26).
    std::vector<std::string> argv { "-c", "user.name=mcppls", "-c", "user.email=mcppls@example.invalid",
                                    "-c", "commit.gpgsign=false", "-c", "maintenance.auto=false", "-c", "gc.auto=0" };
    std::ranges::move(arguments, std::back_inserter(argv));
    auto result = toolrun::run({
        .program = *git,
        .arguments = argv,
        .workDirectory = workspace,
        .purpose = "bench",
        .network = toolrun::Network::allowed,
        .bounds = mcppls::platform::RunBounds { .hard = std::chrono::seconds { 60 } },
    });
    if (!result) return std::unexpected { result.error() };
    if (result->exitCode != 0) {
        return base::fail("bench-review-git", std::format("git {} failed: {}", joined, base::trim(result->error)));
    }
    return {};
}

// A single file, byte for byte, through `mcppls.platform.fs` rather than
// `std::filesystem::copy_file`: openkal-musl does not implement whatever syscall the latter
// prefers for this (measured: ENOSYS, "Function not implemented", copying a fixture's `change/`
// file into a scratch workspace under target/), while plain read/write always works.
base::Result<void> copy_one_file(const std::filesystem::path& source, const std::filesystem::path& target) {
    auto content = fs::read_file(source.string());
    if (!content) return std::unexpected { content.error() };
    if (auto created = fs::create_directories(target.parent_path().string()); !created) {
        return std::unexpected { created.error() };
    }
    return fs::write_file(target.string(), *content);
}

// Copies `source` over `destination`, skipping any entry named `skipName` anywhere in the tree
// (review.py's `shutil.ignore_patterns("scenario.json")`).
base::Result<void> copy_tree_skip_named(const std::filesystem::path& source, const std::filesystem::path& destination,
                                        std::string_view skipName) {
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(source, error);
        it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (error) return base::fail("bench-review", std::format("walking {}: {}", source.string(), error.message()));
        if (it->path().filename() == skipName) {
            if (it->is_directory()) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file()) continue;
        const auto relative = std::filesystem::relative(it->path(), source);
        if (auto copied = copy_one_file(it->path(), destination / relative); !copied) {
            return base::fail("bench-review", std::format("copying {}: {}", it->path().string(), copied.error().message));
        }
    }
    return {};
}

base::Result<void> copy_tree_over(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(source, error);
        it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (error) return base::fail("bench-review", std::format("walking {}: {}", source.string(), error.message()));
        if (!it->is_regular_file()) continue;
        const auto relative = std::filesystem::relative(it->path(), source);
        if (auto copied = copy_one_file(it->path(), destination / relative); !copied) {
            return base::fail("bench-review", std::format("copying {}: {}", it->path().string(), copied.error().message));
        }
    }
    return {};
}

// toolrun needs an absolute program path; `--server`/`--payload`/`--mock-model` may be given
// relative to the caller's own working directory (Python's `Path(...).resolve()`).
std::string resolve_absolute(const std::string& path) {
    const std::string absolute { base::is_absolute_path(path) ? path : base::join_path(fs::current_directory(), path) };
    return base::normalize_path(absolute);
}

std::vector<std::string> environment_with(std::initializer_list<std::pair<std::string, std::string>> overrides) {
    auto variables = env::variables();
    for (const auto& [key, value] : overrides) {
        const std::string prefix { key + "=" };
        std::erase_if(variables, [&](const std::string& entry) { return entry.starts_with(prefix); });
        variables.push_back(key + "=" + value);
    }
    return variables;
}

} // namespace

bool review_finding_matches(const nlohmann::json& finding, const nlohmann::json& entry) {
    if (finding.value("rule", std::string {}) != entry.value("rule", std::string {})) return false;
    if (entry.contains("origin") && finding.value("origin", std::string {}) != entry.at("origin").get<std::string>()) return false;
    const auto location = finding.value("location", nlohmann::json::object());
    if (entry.contains("file") && location.value("file", std::string {}) != entry.at("file").get<std::string>()) return false;
    if (entry.contains("line")) {
        const auto wanted = entry.at("line").get<long long>();
        if (!location.contains("line") || location.at("line").get<long long>() != wanted) return false;
    }
    if (entry.contains("evidence-file")) {
        const std::string wanted { entry.at("evidence-file").get<std::string>() };
        const auto evidence = finding.value("evidence", nlohmann::json::array());
        const bool any = std::ranges::any_of(evidence, [&](const nlohmann::json& e) {
            return e.value("location", nlohmann::json::object()).value("file", std::string {}) == wanted;
        });
        if (!any) return false;
    }
    if (entry.contains("fixed")) {
        const bool wantFixed { entry.at("fixed").get<bool>() };
        const bool hasFix { finding.contains("fix") && !finding.at("fix").is_null() };
        if (hasFix != wantFixed) return false;
    }
    return true;
}

nlohmann::json review_snapshot(const std::string& directory) {
    nlohmann::json files = nlohmann::json::object();
    std::error_code error;
    std::vector<std::filesystem::path> paths;
    for (auto it = std::filesystem::recursive_directory_iterator(directory, error);
        !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (it->is_regular_file()) paths.push_back(it->path());
    }
    std::ranges::sort(paths);
    for (const auto& path : paths) {
        auto content = fs::read_file(path.string());
        if (!content) continue;
        const auto relative = std::filesystem::relative(path, directory).generic_string();
        files[relative] = base::sha256_hex(*content);
    }
    return files;
}

FixtureOutcome run_review_fixture(const std::string& fixtureDir, const std::string& repositoryRoot,
                                  const ReviewOptions& options, const std::string& scratch) {
    FixtureOutcome outcome;
    const std::string reviewJson { base::join_path(fixtureDir, "review.json") };
    auto metaText = fs::read_file(reviewJson);
    if (!metaText) {
        outcome.id = fixtureDir;
        outcome.ok = false;
        outcome.problems.push_back(std::format("cannot read {}: {}", reviewJson, metaText.error().message));
        return outcome;
    }
    nlohmann::json meta;
    try {
        meta = nlohmann::json::parse(*metaText);
    } catch (const std::exception& error) {
        outcome.id = fixtureDir;
        outcome.ok = false;
        outcome.problems.push_back(std::format("{}: invalid JSON ({})", reviewJson, error.what()));
        return outcome;
    }
    outcome.meta = meta;
    outcome.id = meta.value("id", std::string {});
    const std::string workspace { base::join_path(scratch, outcome.id) };

    const auto model = meta.value("model", nlohmann::json {});
    if (!model.is_null() && options.mockModel.empty()) {
        outcome.ok = true;
        outcome.skipped = "needs --mock-model";
        return outcome;
    }
    if (meta.contains("hosts")) {
        const auto hosts = meta.at("hosts");
        const std::string here { host_name() };
        const bool listed = std::ranges::any_of(hosts, [&](const nlohmann::json& h) { return h.get<std::string>() == here; });
        if (!listed) {
            std::vector<std::string> names;
            for (const auto& h : hosts) names.push_back(h.get<std::string>());
            outcome.ok = true;
            outcome.skipped = std::format("runs on {}", base::join(names, ", "));
            return outcome;
        }
    }

    const std::string projectSource { base::join_path(repositoryRoot, meta.at("project").get<std::string>()) };
    fs::remove_all(workspace);
    if (auto copied = copy_tree_skip_named(projectSource, workspace, "scenario.json"); !copied) {
        outcome.ok = false;
        outcome.problems.push_back(copied.error().message);
        return outcome;
    }
    if (auto result = git_run(workspace, { "init", "-q" }); !result) { outcome.ok = false; outcome.problems.push_back(result.error().message); return outcome; }
    if (auto result = git_run(workspace, { "add", "-A" }); !result) { outcome.ok = false; outcome.problems.push_back(result.error().message); return outcome; }
    if (auto result = git_run(workspace, { "commit", "-qm", "base" }); !result) { outcome.ok = false; outcome.problems.push_back(result.error().message); return outcome; }

    const std::string changeDir { base::join_path(fixtureDir, "change") };
    if (fs::is_directory(changeDir)) {
        if (auto copied = copy_tree_over(changeDir, workspace); !copied) {
            outcome.ok = false;
            outcome.problems.push_back(copied.error().message);
            return outcome;
        }
    }
    for (const auto& removed : meta.value("remove", nlohmann::json::array())) {
        const std::string path { base::join_path(workspace, removed.get<std::string>()) };
        if (fs::is_directory(path)) fs::remove_all(path);
        else if (fs::exists(path)) { std::error_code error; std::filesystem::remove(path, error); }
    }

    auto variables = environment_with({ { "MCPPLS_CACHE_DIR", base::join_path(scratch, "cache") } });
    std::vector<std::string> command { options.server, "review", "--format", "json", "--root", workspace,
                                       "--timeout", std::to_string(options.timeoutSeconds) };
    if (!options.payload.empty()) { command.push_back("--payload"); command.push_back(options.payload); }
    if (meta.contains("toolchains")) {
        std::vector<std::string> chains;
        for (const auto& t : meta.at("toolchains")) chains.push_back(t.get<std::string>());
        command.push_back("--toolchains");
        command.push_back(base::join(chains, ","));
    }
    std::string modelScriptPath;
    if (!model.is_null()) {
        modelScriptPath = base::join_path(scratch, outcome.id + "-model.json");
        (void) fs::write_file(modelScriptPath, model.value("script", nlohmann::json::object()).dump());
        variables = environment_with({ { "MCPPLS_CACHE_DIR", base::join_path(scratch, "cache") },
                                       { "MCPPLS_MOCK_MODEL_SCRIPT", modelScriptPath } });
        command.push_back("--model"); command.push_back("gateway");
        command.push_back("--model-gateway"); command.push_back(options.mockModel);
        command.push_back("--model-name");
        command.push_back(model.value("script", nlohmann::json::object()).value("model", std::string { "mock-model" }));
    }

    const auto before = review_snapshot(workspace);
    const auto started = std::chrono::steady_clock::now();
    auto run = toolrun::run({
        .program = command.front(),
        .arguments = { command.begin() + 1, command.end() },
        .workDirectory = workspace,
        .purpose = "bench",
        .network = toolrun::Network::allowed,
        .environment = std::move(variables),
        .bounds = mcppls::platform::RunBounds { .hard = std::chrono::seconds { options.timeoutSeconds + 60 } },
    });
    outcome.seconds = std::chrono::duration<double> { std::chrono::steady_clock::now() - started }.count();
    if (!run) {
        outcome.ok = false;
        outcome.problems.push_back(std::format("the server could not start: {}", run.error().message));
        return outcome;
    }
    const auto after = review_snapshot(workspace);

    nlohmann::json result;
    try {
        result = nlohmann::json::parse(run->output);
    } catch (const std::exception&) {
        outcome.ok = false;
        outcome.problems.push_back(std::format("the output is not JSON (exit {}): {} {}", run->exitCode,
                                                run->output.substr(0, 300), mcppls::platform::last_lines(run->error, 20)));
        return outcome;
    }
    if (result.contains("error")) {
        outcome.ok = false;
        outcome.problems.push_back(std::format("review failed: {}", result.at("error").dump()));
        return outcome;
    }
    const auto findingsArray = result.value("findings", nlohmann::json::array());
    outcome.findings.assign(findingsArray.begin(), findingsArray.end());
    outcome.complete = result.value("complete", true);
    if (after != before) {
        std::vector<std::string> changed;
        for (auto it = after.begin(); it != after.end(); ++it) {
            if (!before.contains(it.key())) changed.push_back("+" + it.key());
            else if (before.at(it.key()) != it.value()) changed.push_back("~" + it.key());
        }
        for (auto it = before.begin(); it != before.end(); ++it) {
            if (!after.contains(it.key())) changed.push_back("-" + it.key());
        }
        outcome.problems.push_back(std::format("the review changed the workspace: {}", base::join(changed, ", ")));
    }
    if (!outcome.complete && !meta.value("incomplete", false)) {
        outcome.problems.push_back(std::format("the review is incomplete: {}",
            result.value("unbuilt", nlohmann::json::array()).dump().substr(0, 200)));
    }
    for (const auto& entry : meta.value("must", nlohmann::json::array())) {
        const bool hit = std::ranges::any_of(outcome.findings, [&](const nlohmann::json& f) { return review_finding_matches(f, entry); });
        outcome.matchedMust.push_back(hit);
        if (!hit) outcome.problems.push_back(std::format("missing {}", entry.dump()));
    }
    for (const auto& entry : meta.value("must-not", nlohmann::json::array())) {
        for (const auto& finding : outcome.findings) {
            if (review_finding_matches(finding, entry)) {
                outcome.falsePositives.push_back(finding);
                outcome.problems.push_back(std::format("unexpected {} at {}:{}: {}",
                    finding.value("rule", std::string {}),
                    finding.value("location", nlohmann::json::object()).value("file", std::string {}),
                    finding.value("location", nlohmann::json::object()).value("line", 0LL),
                    finding.value("message", std::string {})));
            }
        }
    }
    outcome.ok = outcome.problems.empty();
    outcome.full = true;
    return outcome;
}

std::map<std::string, RuleStats> review_rule_stats(const std::vector<FixtureOutcome>& outcomes) {
    std::map<std::string, RuleStats> perRule;
    for (const auto& outcome : outcomes) {
        const auto must = outcome.meta.value("must", nlohmann::json::array());
        for (std::size_t i { 0 }; i < must.size() && i < outcome.matchedMust.size(); ++i) {
            const std::string rule { must[i].value("rule", std::string {}) };
            auto& stats = perRule[rule];
            if (outcome.matchedMust[i]) ++stats.tp; else ++stats.fn;
        }
        for (const auto& finding : outcome.falsePositives) {
            ++perRule[finding.value("rule", std::string {})].fp;
        }
    }
    return perRule;
}

double review_precision(const RuleStats& stats) {
    return (stats.tp + stats.fp) ? static_cast<double>(stats.tp) / (stats.tp + stats.fp) : 1.0;
}

double review_recall(const RuleStats& stats) {
    return (stats.tp + stats.fn) ? static_cast<double>(stats.tp) / (stats.tp + stats.fn) : 1.0;
}

int command_review(const cmdline::ParsedArgs& arguments) {
    const std::string root { repository_root() };
    const auto serverOpt = arguments.value("server");
    if (!serverOpt || serverOpt->empty()) {
        std::println(std::cerr, "mcppls-devtools: bench review needs --server PATH");
        return 2;
    }
    ReviewOptions options;
    options.server = resolve_absolute(*serverOpt);
    if (auto payload = arguments.value("payload"); payload && !payload->empty()) options.payload = resolve_absolute(*payload);
    if (auto mock = arguments.value("mock-model"); mock && !mock->empty()) options.mockModel = resolve_absolute(*mock);
    if (auto timeout = arguments.value("timeout"); timeout && !timeout->empty()) options.timeoutSeconds = std::stoi(*timeout);
    const bool keep { arguments.is_flag_set("keep") };
    const std::string reportPath { arguments.value("report").value_or("") };

    std::vector<std::string> only;
    if (auto opt = arguments.option("fixture")) only = opt->get().values;

    const std::string fixturesDir { base::join_path(root, std::string { REVIEW_SUBDIR }) };
    std::vector<std::string> fixtureDirs;
    for (const auto& child : fs::list_directory(fixturesDir)) {
        if (fs::is_regular_file(base::join_path(child, "review.json"))) fixtureDirs.push_back(child);
    }
    std::ranges::sort(fixtureDirs);
    if (!only.empty()) {
        std::erase_if(fixtureDirs, [&](const std::string& d) {
            return std::ranges::find(only, base::file_name(base::normalize_path(d))) == only.end();
        });
    }

    const std::string scratch { base::join_path(root, "target/bench-review") };
    fs::remove_all(scratch);
    (void) fs::create_directories(scratch);

    std::vector<FixtureOutcome> results;
    for (const auto& fixtureDir : fixtureDirs) {
        auto outcome = run_review_fixture(fixtureDir, root, options, scratch);
        if (outcome.skipped) {
            std::println("SKIP {} ({})", outcome.id, *outcome.skipped);
        } else {
            std::println("{} {} ({:.1f}s, {} finding(s))", outcome.ok ? "PASS" : "FAIL", outcome.id,
                         outcome.seconds, outcome.findings.size());
            for (const auto& problem : outcome.problems) std::println("  {}", problem);
        }
        results.push_back(std::move(outcome));
    }
    if (!keep) fs::remove_all(scratch);

    const auto perRule = review_rule_stats(results);
    std::println("");
    std::println("rule                                  precision  recall   tp fn fp");
    for (const auto& [rule, stats] : perRule) {
        std::println("{:<38}{:>8.0f}%{:>7.0f}%  {:>3}{:>3}{:>3}", rule, review_precision(stats) * 100,
                     review_recall(stats) * 100, stats.tp, stats.fn, stats.fp);
    }

    std::vector<std::string> failed;
    for (const auto& outcome : results) if (!outcome.skipped && !outcome.ok) failed.push_back(outcome.id);
    std::println("");
    std::println("{}/{} review fixtures passed", results.size() - failed.size(), results.size());

    if (!reportPath.empty()) {
        nlohmann::ordered_json report;
        report["fixtures"] = nlohmann::json::array();
        for (const auto& outcome : results) {
            nlohmann::ordered_json record;
            record["id"] = outcome.id;
            record["ok"] = outcome.ok;
            if (outcome.skipped) record["skipped"] = *outcome.skipped;
            record["seconds"] = outcome.seconds;
            record["problems"] = outcome.problems;
            record["findings"] = outcome.findings;
            if (outcome.full) {
                record["matched"] = outcome.matchedMust;
                record["false-positives"] = outcome.falsePositives;
                record["complete"] = outcome.complete;
            }
            report["fixtures"].push_back(std::move(record));
        }
        nlohmann::ordered_json rules;
        for (const auto& [rule, stats] : perRule) {
            nlohmann::ordered_json entry;
            entry["tp"] = stats.tp;
            entry["fn"] = stats.fn;
            entry["fp"] = stats.fp;
            entry["precision"] = review_precision(stats);
            entry["recall"] = review_recall(stats);
            rules[rule] = std::move(entry);
        }
        report["rules"] = rules;
        (void) fs::write_file(reportPath, report.dump(2));
    }
    return failed.empty() ? 0 : 1;
}

} // namespace mcppls::devtools::bench
