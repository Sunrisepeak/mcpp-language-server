module mcppls.ai.verify.changes;

import std;
import nlohmann.json;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.base.uri;
import mcppls.platform.fs;
import mcppls.spec.database;
import mcppls.spec.query;
import mcppls.project.git;
import mcppls.project.scan;
import mcppls.engine.native.index;
import mcppls.orchestrator.kernel;
import mcppls.orchestrator.workspace;
import mcppls.ai.query.view;
import mcppls.ai.query.files;
import mcppls.ai.query.modules;

namespace mcppls::ai::verify {

namespace {

using Json = nlohmann::json;
using query::Clock;

std::chrono::milliseconds until(Clock::time_point deadline) {
    return std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()), std::chrono::milliseconds { 0 });
}

void count(std::map<std::string, int>& counts, const std::vector<query::Diagnostic>& diagnostics) {
    for (const std::string_view severity : { "error", "warning", "information", "hint" }) counts.try_emplace(std::string { severity }, 0);
    for (const auto& diagnostic : diagnostics) ++counts[std::string { spec::to_string(diagnostic.severity) }];
}

bool has_error(const std::vector<query::Diagnostic>& diagnostics) {
    return std::ranges::any_of(diagnostics, [](const query::Diagnostic& d) { return d.severity == spec::Severity::error; });
}

// The modules a unit provides, "m" or "m:p", as the module index has it now.
std::vector<std::string> provided_by(const index::ModuleIndex& moduleIndex, std::string_view path) {
    const auto* scan = moduleIndex.scan_of(path);
    if (scan == nullptr) return {};
    const std::string provided { project::provided_name(*scan) };
    if (provided.empty()) return {};
    return { provided };
}

} // namespace

query::Outcome<Verification> verify_changes(query::View& view, const ChangeOptions& options, Clock::time_point deadline) {
    auto& workspace = view.kernel().workspace();
    view.refresh();
    (void)view.settle(deadline);

    struct Changed {
        std::string path;
        int type { 2 };   // didChangeWatchedFiles: 1 created, 2 changed, 3 deleted
    };
    std::vector<Changed> changes;
    if (options.workingTree || !options.base.empty()) {
        if (!workspace.trusted()) return std::unexpected { query::Failure { "untrusted", "the workspace is not trusted, so git is not run", nullptr } };
        auto found = options.base.empty() ? project::git::working_tree_changes(view.root()) : project::git::changes_since(view.root(), options.base);
        if (!found) return std::unexpected { query::Failure { "unavailable", found.error().message, nullptr } };
        for (const auto& change : *found) {
            if (change.kind == project::git::ChangeKind::renamed && !change.oldPath.empty()) changes.push_back(Changed { change.oldPath, 3 });
            const int type { change.kind == project::git::ChangeKind::removed ? 3
                             : change.kind == project::git::ChangeKind::modified ? 2 : 1 };
            changes.push_back(Changed { change.path, type });
        }
    }
    for (const auto& file : options.files) {
        const std::string path { view.path_of(file) };
        changes.push_back(Changed { path, platform::fs::exists(path) ? 2 : 3 });
    }
    if (changes.empty()) return std::unexpected { query::invalid_arguments("no changed files: name files, or ask for the working tree's changes") };

    // What the changed files provided before this round, so the importers of a removed or renamed
    // module are checked too; then the workspace learns of the changes the way a file watch reports them.
    std::set<std::string> modules;
    for (const auto& change : changes) {
        for (auto& name : provided_by(workspace.module_index(), change.path)) modules.insert(std::move(name));
    }
    Json notification = Json::array();
    bool buildDescription { false };
    for (const auto& change : changes) {
        notification.push_back(Json { { "uri", base::path_to_uri(change.path) }, { "type", change.type } });
        buildDescription = buildDescription || orchestrator::is_build_file(base::file_name(change.path));
    }
    workspace.handle_watched_files(notification);
    view.refresh();
    if (buildDescription) {
        // A changed build description loads the model again once its debounce passes.
        (void)view.kernel().wait_until([&] { return workspace.model_loading(); }, std::min(until(deadline), std::chrono::milliseconds { 3000 }));
    }
    (void)view.settle(deadline);

    Verification verification;
    std::vector<std::pair<std::string, std::string>> toCheck;   // (path, why)
    std::set<std::string> seen;
    auto add = [&](const std::string& path, std::string_view why) {
        if (seen.insert(base::path_key(path)).second) toCheck.emplace_back(path, std::string { why });
    };
    for (const auto& change : changes) {
        if (change.type == 3) {
            verification.removed.push_back(view.display(change.path));
            continue;
        }
        verification.changed.push_back(view.display(change.path));
        if (project::is_cxx_source_name(change.path)) add(change.path, "changed");
        for (auto& name : provided_by(workspace.module_index(), change.path)) modules.insert(std::move(name));
    }
    // An interface that changed can break every unit that can use it.
    for (const auto& name : modules) {
        for (const auto& path : query::module_neighbourhood(view, name, options.budget * 4).files) {
            if (platform::fs::exists(path)) add(path, "importer");
        }
    }
    for (std::size_t i { 0 }; i < toCheck.size(); ++i) {
        if (i >= options.budget) verification.unchecked.push_back(view.display(toCheck[i].first));
    }
    if (toCheck.size() > options.budget) toCheck.resize(options.budget);

    for (const auto& [path, why] : toCheck) {
        // An importer the engines built before the change keeps that build until it is given a new version.
        if (why == "importer" && view.kernel().is_open(path)) view.kernel().touch(path);
        else view.open(path);
    }
    (void)view.kernel().wait_until([&] { return std::ranges::all_of(toCheck, [&](const auto& item) { return query::diagnostics_fresh(view, item.first).first; }); },
                                   until(deadline));
    bool errors { false };
    bool incomplete { !verification.unchecked.empty() };
    for (const auto& [path, why] : toCheck) {
        CheckedFile checked;
        checked.file = view.display(path);
        checked.why = why;
        auto [complete, reason] = query::diagnostics_fresh(view, path);
        checked.complete = complete;
        checked.reason = std::move(reason);
        checked.diagnostics = query::published_diagnostics(view, path);
        count(verification.counts, checked.diagnostics);
        errors = errors || has_error(checked.diagnostics);
        incomplete = incomplete || !complete;
        verification.checked.push_back(std::move(checked));
    }
    count(verification.counts, {});
    verification.verdict = errors ? "errors" : incomplete ? "incomplete" : "pass";
    verification.semanticSource = query::semantic_source(view);
    std::ranges::sort(verification.changed);
    std::ranges::sort(verification.removed);
    verification.snapshot = view.snapshot();
    return verification;
}

query::Outcome<SnippetVerification> verify_snippet(query::View& view, const SnippetOptions& options, Clock::time_point deadline) {
    view.refresh();
    const std::string path { view.path_of(options.file) };
    const auto original = view.kernel().text(path);
    if (!original) return std::unexpected { query::invalid_arguments(std::format("no such file: {}", options.file)) };
    std::vector<std::string> lines;
    for (auto line : base::split(*original, '\n')) lines.emplace_back(line);
    const bool endsWithNewline { !original->empty() && original->back() == '\n' };
    if (endsWithNewline) lines.pop_back();
    const int lineCount { static_cast<int>(lines.size()) };
    if (options.line < 1 || options.line > lineCount + 1) {
        return std::unexpected { query::invalid_arguments(std::format("line {} is outside {} (1 to {})", options.line, options.file, lineCount + 1)) };
    }
    if (options.replaceLines < 0 || options.line - 1 + options.replaceLines > lineCount) {
        return std::unexpected { query::invalid_arguments("replaceLines goes past the end of the file") };
    }
    (void)view.settle(deadline);

    // The file as it is: what is wrong with it already is not the snippet's doing.
    view.open(path);
    (void)view.kernel().wait_until([&] { return query::diagnostics_fresh(view, path).first; }, until(deadline));
    std::set<std::pair<std::string, std::string>> before;
    for (const auto& diagnostic : query::published_diagnostics(view, path)) {
        before.emplace(diagnostic.message, std::string { base::trim(diagnostic.location.text) });
    }

    std::vector<std::string> snippet;
    for (auto line : base::split(options.code, '\n')) snippet.emplace_back(line);
    if (!options.code.empty() && options.code.back() == '\n') snippet.pop_back();
    std::vector<std::string> candidate(lines.begin(), lines.begin() + (options.line - 1));
    candidate.insert(candidate.end(), snippet.begin(), snippet.end());
    candidate.insert(candidate.end(), lines.begin() + (options.line - 1 + options.replaceLines), lines.end());
    std::string text { base::join(candidate, "\n") };
    if (endsWithNewline || !candidate.empty()) text += "\n";

    SnippetVerification verification;
    verification.file = view.display(path);
    verification.line = options.line;
    verification.endLine = options.line + std::max(static_cast<int>(snippet.size()), 1) - 1;
    view.kernel().change(path, std::move(text));
    (void)view.kernel().wait_until([&] { return query::diagnostics_fresh(view, path).first; }, until(deadline));
    verification.complete = query::diagnostics_fresh(view, path).first;
    for (auto& diagnostic : query::published_diagnostics(view, path)) {
        const bool inside { diagnostic.location.line >= verification.line && diagnostic.location.line <= verification.endLine };
        if (inside) {
            verification.inSnippet.push_back(std::move(diagnostic));
        } else if (!before.contains({ diagnostic.message, std::string { base::trim(diagnostic.location.text) } })) {
            verification.introduced.push_back(std::move(diagnostic));
        }
    }
    count(verification.counts, verification.inSnippet);
    count(verification.counts, verification.introduced);
    verification.snapshot = view.snapshot();
    view.kernel().revert(path);
    const bool errors { has_error(verification.inSnippet) || has_error(verification.introduced) };
    verification.verdict = errors ? "errors" : verification.complete ? "pass" : "incomplete";
    return verification;
}

query::Outcome<FixVerification> verify_fix(query::View& view, const Json& fix, Clock::time_point deadline) {
    const Json edits = fix.is_object() ? fix.value("edits", Json::array()) : Json::array();
    if (!edits.is_array() || edits.empty()) return std::unexpected { query::invalid_arguments("a fix has no edits") };
    view.refresh();
    (void)view.settle(deadline);
    // Edits by file, each file's from the end backwards so earlier positions stay where they are.
    std::map<std::string, std::vector<Json>> byFile;
    for (const auto& edit : edits) {
        if (!edit.is_object() || !edit.contains("file")) return std::unexpected { query::invalid_arguments("an edit names no file") };
        byFile[view.path_of(edit.value("file", std::string {}))].push_back(edit);
    }
    std::map<std::string, std::string> candidates;
    for (auto& [path, fileEdits] : byFile) {
        auto text = view.kernel().text(path);
        if (!text) return std::unexpected { query::invalid_arguments(std::format("no such file: {}", view.display(path))) };
        std::ranges::sort(fileEdits, [](const Json& a, const Json& b) {
            return std::pair { a.value("line", 0), a.value("column", 0) } > std::pair { b.value("line", 0), b.value("column", 0) };
        });
        std::string candidate { *text };
        for (const auto& edit : fileEdits) {
            const auto start = base::offset_at(candidate, spec::lsp_position(candidate, edit.value("line", 1), edit.value("column", 1)));
            const auto end = base::offset_at(candidate, spec::lsp_position(candidate, edit.value("endLine", edit.value("line", 1)), edit.value("endColumn", edit.value("column", 1))));
            if (!start || !end || *end < *start) return std::unexpected { query::invalid_arguments(std::format("an edit of {} is outside the file", view.display(path))) };
            candidate.replace(*start, *end - *start, edit.value("newText", std::string {}));
        }
        candidates.emplace(path, std::move(candidate));
    }

    // What the files report before the fix is not the fix's doing.
    std::set<std::pair<std::string, std::string>> before;
    for (const auto& [path, candidate] : candidates) view.open(path);
    (void)view.kernel().wait_until([&] { return std::ranges::all_of(candidates, [&](const auto& item) { return query::diagnostics_fresh(view, item.first).first; }); },
                                   until(deadline));
    for (const auto& [path, candidate] : candidates) {
        for (const auto& diagnostic : query::published_diagnostics(view, path)) before.emplace(diagnostic.message, std::string { base::trim(diagnostic.location.text) });
    }
    for (auto& [path, candidate] : candidates) view.kernel().change(path, candidate);
    (void)view.kernel().wait_until([&] { return std::ranges::all_of(candidates, [&](const auto& item) { return query::diagnostics_fresh(view, item.first).first; }); },
                                   until(deadline));
    FixVerification verification;
    verification.complete = std::ranges::all_of(candidates, [&](const auto& item) { return query::diagnostics_fresh(view, item.first).first; });
    for (const auto& [path, candidate] : candidates) {
        for (auto& diagnostic : query::published_diagnostics(view, path)) {
            if (diagnostic.severity != spec::Severity::error) continue;
            if (!before.contains({ diagnostic.message, std::string { base::trim(diagnostic.location.text) } })) verification.introduced.push_back(std::move(diagnostic));
        }
        view.kernel().revert(path);
    }
    verification.passes = verification.complete && verification.introduced.empty();
    if (!verification.complete) verification.reason = "the core engine did not check the fixed files in time";
    else if (!verification.introduced.empty()) verification.reason = std::format("the fix introduces {} error(s)", verification.introduced.size());
    return verification;
}

Json to_json(const Verification& verification) {
    Json checked = Json::array();
    for (const auto& file : verification.checked) {
        Json diagnostics = Json::array();
        for (const auto& diagnostic : file.diagnostics) diagnostics.push_back(query::to_json(diagnostic));
        Json value { { "file", file.file }, { "why", file.why }, { "complete", file.complete }, { "diagnostics", std::move(diagnostics) } };
        if (!file.reason.empty()) value["reason"] = file.reason;
        checked.push_back(std::move(value));
    }
    return Json { { "snapshot", spec::to_json(verification.snapshot) }, { "verdict", verification.verdict }, { "changed", verification.changed },
                  { "removed", verification.removed }, { "checked", std::move(checked) }, { "unchecked", verification.unchecked },
                  { "counts", verification.counts }, { "semanticSource", verification.semanticSource } };
}

Json to_json(const SnippetVerification& verification) {
    auto list = [](const std::vector<query::Diagnostic>& diagnostics) {
        Json values = Json::array();
        for (const auto& diagnostic : diagnostics) values.push_back(query::to_json(diagnostic));
        return values;
    };
    return Json { { "snapshot", spec::to_json(verification.snapshot) }, { "verdict", verification.verdict }, { "file", verification.file },
                  { "line", verification.line }, { "endLine", verification.endLine }, { "complete", verification.complete },
                  { "inSnippet", list(verification.inSnippet) }, { "introduced", list(verification.introduced) }, { "counts", verification.counts } };
}

} // namespace mcppls::ai::verify
