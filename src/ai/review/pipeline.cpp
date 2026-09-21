module mcppls.ai.review.pipeline;

import std;
import nlohmann.json;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.uri;
import mcppls.platform.fs;
import mcppls.spec.query;
import mcppls.project.git;
import mcppls.project.scan;
import mcppls.orchestrator.kernel;
import mcppls.orchestrator.workspace;
import mcppls.ai.query.view;
import mcppls.ai.query.files;
import mcppls.ai.verify.toolchains;
import mcppls.ai.review.changes;
import mcppls.ai.review.semantic;
import mcppls.ai.review.impact;
import mcppls.ai.review.rules;

namespace mcppls::ai::review {

namespace {

using Json = nlohmann::json;
using query::Clock;

std::chrono::milliseconds until(Clock::time_point deadline) {
    return std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()), std::chrono::milliseconds { 0 });
}

// The workspace learns of the change as a file watch would report it, and settles.
void announce(query::View& view, const ChangeSet& changes, Clock::time_point deadline) {
    auto& workspace = view.kernel().workspace();
    Json notification = Json::array();
    bool buildDescription { false };
    for (const auto& file : changes.files) {
        const int type { !file.head ? 3 : !file.base ? 1 : 2 };
        notification.push_back(Json { { "uri", base::path_to_uri(file.path) }, { "type", type } });
        if (!file.oldPath.empty()) notification.push_back(Json { { "uri", base::path_to_uri(file.oldPath) }, { "type", 3 } });
        buildDescription = buildDescription || orchestrator::is_build_file(base::file_name(file.path));
    }
    if (!notification.empty()) workspace.handle_watched_files(notification);
    view.refresh();
    if (buildDescription) (void)view.kernel().wait_until([&] { return workspace.model_loading(); }, std::min(until(deadline), std::chrono::milliseconds { 3000 }));
    (void)view.settle(deadline);
}

} // namespace

query::Outcome<ReviewResult> analyze_change(query::View& view, const ReviewRequest& request, Clock::time_point deadline) {
    const auto started = Clock::now();
    const auto stage = [&](std::string_view what) {
        base::log::info("review: {} after {} ms", what, std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
    };
    view.refresh();
    (void)view.settle(deadline);
    stage("settled");
    auto changes = collect_changes(view, request.changes);
    if (!changes) return std::unexpected { changes.error() };
    stage("changes collected");
    announce(view, *changes, deadline);
    stage("changes announced");
    ReviewResult result;
    // Only C++ sources have a semantic diff; other files (the build description) stay in the change.
    std::vector<FileChange> sources;
    std::vector<FileChange> others;
    for (auto& file : changes->files) {
        if (project::is_cxx_source_name(file.path)) sources.push_back(std::move(file));
        else others.push_back(std::move(file));
    }
    for (const auto& file : sources) result.diffs.push_back(semantic_diff(view.display(file.path), file.base, file.head));
    changes->files = std::move(sources);
    std::ranges::move(others, std::back_inserter(changes->files));
    result.changes = std::move(*changes);
    result.impact = analyze_impact(view, result.changes, result.diffs, request.budget, deadline);
    stage("impact analyzed");
    result.snapshot = view.snapshot();
    return result;
}

query::Outcome<ReviewResult> review_change(query::View& view, const ReviewRequest& request, Clock::time_point deadline) {
    auto analyzed = analyze_change(view, request, deadline);
    if (!analyzed) return analyzed;
    ReviewResult& result { *analyzed };
    if (request.build) {
        // The changed units first, then the units that can use a changed interface.
        std::vector<std::pair<std::string, bool>> units;   // (path, importer)
        std::set<std::string> seen;
        for (const auto& file : result.changes.files) {
            if (file.head && project::is_cxx_source_name(file.path) && seen.insert(base::path_key(file.path)).second) units.emplace_back(file.path, false);
        }
        for (const auto& display : result.impact.files) {
            const std::string path { view.path_of(display) };
            if (platform::fs::exists(path) && seen.insert(base::path_key(path)).second) units.emplace_back(path, true);
        }
        for (std::size_t i { request.budget }; i < units.size(); ++i) result.unbuilt.push_back(view.display(units[i].first));
        if (units.size() > request.budget) units.resize(request.budget);
        for (const auto& [path, importer] : units) {
            if (importer && view.kernel().is_open(path)) view.kernel().touch(path);
            else view.open(path);
        }
        (void)view.kernel().wait_until([&] { return std::ranges::all_of(units, [&](const auto& unit) { return query::diagnostics_fresh(view, unit.first).first; }); },
                                       until(deadline));
        for (const auto& [path, importer] : units) {
            if (!query::diagnostics_fresh(view, path).first) result.unbuilt.push_back(view.display(path));
            result.diagnostics[view.display(path)] = query::published_diagnostics(view, path);
        }
        base::log::info("review: {} unit(s) built, {} not", units.size(), result.unbuilt.size());
    }
    if (request.toolchains.size() >= 2) {
        auto compared = verify::compare_toolchains(view, request.toolchains, deadline);
        if (compared) result.toolchains = std::move(*compared);
        else result.toolchainFailure = compared.error();
        base::log::info("review: {} toolchain(s) compared", request.toolchains.size());
    }
    result.findings = run_rules(view, RuleInput { result.changes, result.diffs, result.impact, result.diagnostics, result.toolchains ? &*result.toolchains : nullptr });
    base::log::info("review: {} finding(s)", result.findings.size());
    for (const std::string_view severity : { "error", "warning", "information", "hint" }) result.counts[std::string { severity }] = 0;
    for (const auto& finding : result.findings) ++result.counts[std::string { spec::to_string(finding.severity) }];
    result.snapshot = view.snapshot();
    return analyzed;
}

Json impact_json(const ReviewResult& result) {
    Json files = Json::array();
    for (const auto& file : result.changes.files) {
        Json hunks = Json::array();
        for (const auto& hunk : file.hunks) hunks.push_back(to_json(hunk));
        std::string display { file.path };
        if (auto relative = base::relative_path(file.path, result.changes.repositoryRoot)) display = *relative;
        files.push_back(Json { { "path", display }, { "change", std::string { project::git::to_string(file.kind) } }, { "hunks", std::move(hunks) } });
    }
    Json diffs = Json::array();
    for (const auto& diff : result.diffs) diffs.push_back(to_json(diff));
    return Json { { "snapshot", spec::to_json(result.snapshot) }, { "base", result.changes.base }, { "files", std::move(files) }, { "diffs", std::move(diffs) },
                  { "impact", to_json(result.impact) } };
}

Json review_json(const ReviewResult& result) {
    Json value = impact_json(result);
    Json findings = Json::array();
    for (const auto& finding : result.findings) findings.push_back(spec::to_json(finding));
    value["findings"] = std::move(findings);
    value["counts"] = result.counts;
    value["unbuilt"] = result.unbuilt;
    value["complete"] = result.unbuilt.empty() && result.impact.unsearched.empty() && !result.toolchainFailure;
    if (result.toolchains) value["toolchains"] = verify::to_json(*result.toolchains);
    if (result.toolchainFailure) value["toolchainFailure"] = query::to_json(*result.toolchainFailure);
    return value;
}

} // namespace mcppls::ai::review
