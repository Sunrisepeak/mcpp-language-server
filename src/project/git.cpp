module mcppls.project.git;

import std;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.platform.process;
import mcppls.platform.toolrun;

namespace mcppls::project::git {

namespace {

base::Result<platform::RunResult> run_git(std::string_view directory, std::vector<std::string> arguments) {
    const auto git = platform::env::find_executable("git");
    if (!git) return base::fail("git-not-found", "git is not on PATH");
    // Reading must not write: `git status` otherwise refreshes the index file of the repository.
    std::vector<std::string> argv { "--no-optional-locks" };
    std::ranges::move(arguments, std::back_inserter(argv));
    auto result = platform::toolrun::run({
        .program = *git,
        .arguments = std::move(argv),
        .workDirectory = std::string { directory },
        .purpose = "git",
        .bounds = platform::RunBounds { .hard = std::chrono::seconds { 60 } },
    });
    if (!result) return std::unexpected { result.error() };
    if (result->timedOut) return base::fail("git-timeout", "git did not finish in 60 seconds");
    return result;
}

std::string absolute_in(std::string_view repositoryRoot, std::string_view relative) {
    return base::normalize_path(base::join_path(repositoryRoot, relative));
}

std::vector<Change> within(std::vector<Change> changes, std::string_view directory) {
    std::erase_if(changes, [&](const Change& change) { return !base::is_within(change.path, directory); });
    std::ranges::sort(changes, {}, &Change::path);
    return changes;
}

} // namespace

std::string_view to_string(ChangeKind kind) {
    switch (kind) {
    case ChangeKind::added: return "added";
    case ChangeKind::modified: return "modified";
    case ChangeKind::removed: return "removed";
    case ChangeKind::renamed: return "renamed";
    case ChangeKind::untracked: return "untracked";
    }
    return "modified";
}

std::vector<Change> parse_status(std::string_view output, std::string_view repositoryRoot) {
    std::vector<Change> changes;
    const auto fields = base::split(output, '\0');
    for (std::size_t i { 0 }; i < fields.size(); ++i) {
        const std::string_view entry { fields[i] };
        if (entry.size() < 4) continue;
        const char index { entry[0] };
        const char tree { entry[1] };
        Change change;
        change.path = absolute_in(repositoryRoot, entry.substr(3));
        if (index == '?' && tree == '?') {
            change.kind = ChangeKind::untracked;
        } else if (index == 'R' || index == 'C') {
            change.kind = ChangeKind::renamed;
            // The source path of a rename or copy is the next field.
            if (i + 1 < fields.size()) change.oldPath = absolute_in(repositoryRoot, fields[++i]);
        } else if (index == 'D' || tree == 'D') {
            change.kind = ChangeKind::removed;
        } else if (index == 'A') {
            change.kind = ChangeKind::added;
        } else {
            change.kind = ChangeKind::modified;
        }
        changes.push_back(std::move(change));
    }
    return changes;
}

std::vector<Change> parse_name_status(std::string_view output, std::string_view repositoryRoot) {
    std::vector<Change> changes;
    const auto fields = base::split(output, '\0');
    for (std::size_t i { 0 }; i + 1 < fields.size(); ++i) {
        const std::string_view status { fields[i] };
        if (status.empty()) continue;
        Change change;
        switch (status.front()) {
        case 'A': change.kind = ChangeKind::added; break;
        case 'D': change.kind = ChangeKind::removed; break;
        case 'R':
        case 'C': change.kind = ChangeKind::renamed; break;
        default: change.kind = ChangeKind::modified; break;
        }
        if (change.kind == ChangeKind::renamed) {
            if (i + 2 >= fields.size()) break;
            change.oldPath = absolute_in(repositoryRoot, fields[i + 1]);
            change.path = absolute_in(repositoryRoot, fields[i + 2]);
            i += 2;
        } else {
            change.path = absolute_in(repositoryRoot, fields[i + 1]);
            i += 1;
        }
        changes.push_back(std::move(change));
    }
    return changes;
}

base::Result<std::string> repository_root(std::string_view directory) {
    auto result = run_git(directory, { "rev-parse", "--show-toplevel" });
    if (!result) return std::unexpected { result.error() };
    if (result->exitCode != 0) return base::fail("not-a-repository", std::format("{} is not in a git repository", directory));
    const std::string root { base::trim(result->output) };
    return platform::fs::exists(root) ? platform::fs::canonical_path(root) : base::normalize_path(root);
}

base::Result<std::vector<Change>> working_tree_changes(std::string_view directory) {
    auto root = repository_root(directory);
    if (!root) return std::unexpected { root.error() };
    auto result = run_git(*root, { "status", "--porcelain=v1", "-z", "--untracked-files=all" });
    if (!result) return std::unexpected { result.error() };
    if (result->exitCode != 0) return base::fail("git-failed", std::format("git status failed: {}", base::trim(result->error)));
    return within(parse_status(result->output, *root), directory);
}

base::Result<std::vector<Change>> changes_since(std::string_view directory, std::string_view base) {
    auto root = repository_root(directory);
    if (!root) return std::unexpected { root.error() };
    auto result = run_git(*root, { "diff", "--name-status", "-z", "-M", std::string { base } });
    if (!result) return std::unexpected { result.error() };
    if (result->exitCode != 0) return base::fail("git-failed", std::format("git diff {} failed: {}", base, base::trim(result->error)));
    auto changes = parse_name_status(result->output, *root);
    // Files git does not track yet are changes against every revision.
    if (auto status = run_git(*root, { "ls-files", "--others", "--exclude-standard", "-z" }); status && status->exitCode == 0) {
        for (auto path : base::split(status->output, '\0')) {
            if (!path.empty()) changes.push_back(Change { ChangeKind::untracked, absolute_in(*root, path), {} });
        }
    }
    return within(std::move(changes), directory);
}

base::Result<std::optional<std::string>> show(std::string_view repositoryRoot, std::string_view revision, std::string_view absolutePath) {
    auto relative = base::relative_path(absolutePath, repositoryRoot);
    if (!relative) return base::fail("outside-repository", std::format("{} is not in the repository", absolutePath));
    std::string name { *relative };
    std::ranges::replace(name, '\\', '/');
    auto result = run_git(repositoryRoot, { "show", std::format("{}:{}", revision, name) });
    if (!result) return std::unexpected { result.error() };
    if (result->exitCode != 0) return std::optional<std::string> {};
    return std::optional<std::string> { std::move(result->output) };
}

} // namespace mcppls::project::git
