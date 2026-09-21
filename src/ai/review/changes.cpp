module mcppls.ai.review.changes;

import std;
import nlohmann.json;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.fs;
import mcppls.project.git;
import mcppls.project.scan;
import mcppls.orchestrator.kernel;
import mcppls.orchestrator.workspace;
import mcppls.ai.query.view;

namespace mcppls::ai::review {

namespace {

using Json = nlohmann::json;

// Past this many lines between the common prefix and suffix, the middle is one hunk: an exact
// script would cost more than a review gains from it.
constexpr std::size_t MAX_DIFFED_LINES { 20000 };

std::vector<std::string_view> lines_of(std::string_view text) {
    std::vector<std::string_view> lines;
    if (text.empty()) return lines;
    for (auto line : base::split(text, '\n')) {
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.push_back(line);
    }
    if (text.back() == '\n') lines.pop_back();
    return lines;
}

enum class Edit { same, removed, added };

// Myers' O((N+M)D) shortest edit script.
std::vector<Edit> edit_script(std::span<const std::string_view> a, std::span<const std::string_view> b) {
    const int n { static_cast<int>(a.size()) };
    const int m { static_cast<int>(b.size()) };
    const int max { n + m };
    std::vector<int> v(static_cast<std::size_t>(2 * max + 2), 0);
    std::vector<std::vector<int>> trace;
    const auto at = [&](int k) -> int& { return v[static_cast<std::size_t>(k + max + 1)]; };
    for (int d { 0 }; d <= max; ++d) {
        trace.push_back(v);
        for (int k { -d }; k <= d; k += 2) {
            int x { (k == -d || (k != d && at(k - 1) < at(k + 1))) ? at(k + 1) : at(k - 1) + 1 };
            int y { x - k };
            while (x < n && y < m && a[static_cast<std::size_t>(x)] == b[static_cast<std::size_t>(y)]) {
                ++x;
                ++y;
            }
            at(k) = x;
            if (x >= n && y >= m) {
                std::vector<Edit> edits;
                int cx { n };
                int cy { m };
                for (int step { d }; step > 0; --step) {
                    const auto& previous = trace[static_cast<std::size_t>(step)];
                    const auto was = [&](int kk) { return previous[static_cast<std::size_t>(kk + max + 1)]; };
                    const int ck { cx - cy };
                    const int pk { (ck == -step || (ck != step && was(ck - 1) < was(ck + 1))) ? ck + 1 : ck - 1 };
                    const int px { was(pk) };
                    const int py { px - pk };
                    while (cx > px && cy > py) {
                        edits.push_back(Edit::same);
                        --cx;
                        --cy;
                    }
                    if (cx == px) {
                        edits.push_back(Edit::added);
                        --cy;
                    } else {
                        edits.push_back(Edit::removed);
                        --cx;
                    }
                }
                while (cx > 0 && cy > 0) {
                    edits.push_back(Edit::same);
                    --cx;
                    --cy;
                }
                std::ranges::reverse(edits);
                return edits;
            }
        }
    }
    return {};
}

std::string text_of(query::View& view, const std::string& path) {
    return view.kernel().text(path).value_or(std::string {});
}

} // namespace

std::vector<Hunk> line_hunks(std::string_view baseText, std::string_view headText) {
    const auto a = lines_of(baseText);
    const auto b = lines_of(headText);
    std::size_t prefix { 0 };
    while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix]) ++prefix;
    std::size_t suffix { 0 };
    while (suffix < a.size() - prefix && suffix < b.size() - prefix && a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix]) ++suffix;
    const std::span<const std::string_view> middleA { a.data() + prefix, a.size() - prefix - suffix };
    const std::span<const std::string_view> middleB { b.data() + prefix, b.size() - prefix - suffix };
    std::vector<Hunk> hunks;
    if (middleA.empty() && middleB.empty()) return hunks;
    if (middleA.size() + middleB.size() > MAX_DIFFED_LINES) {
        hunks.push_back(Hunk { static_cast<int>(prefix) + 1, static_cast<int>(middleA.size()), static_cast<int>(prefix) + 1, static_cast<int>(middleB.size()) });
        return hunks;
    }
    int baseLine { static_cast<int>(prefix) + 1 };
    int headLine { static_cast<int>(prefix) + 1 };
    std::optional<Hunk> open;
    for (const Edit edit : edit_script(middleA, middleB)) {
        if (edit == Edit::same) {
            if (open) hunks.push_back(*std::exchange(open, std::nullopt));
            ++baseLine;
            ++headLine;
            continue;
        }
        if (!open) open = Hunk { baseLine, 0, headLine, 0 };
        if (edit == Edit::removed) {
            ++open->baseCount;
            ++baseLine;
        } else {
            ++open->headCount;
            ++headLine;
        }
    }
    if (open) hunks.push_back(*open);
    return hunks;
}

bool FileChange::changed_head_line(int line) const {
    if (!base) return true;
    if (!head) return false;
    return std::ranges::any_of(hunks, [&](const Hunk& hunk) { return line >= hunk.headStart && line < hunk.headStart + hunk.headCount; });
}

query::Outcome<ChangeSet> collect_changes(query::View& view, const ChangeRequest& request) {
    auto& workspace = view.kernel().workspace();
    if (!workspace.trusted()) return std::unexpected { query::Failure { "untrusted", "the workspace is not trusted, so git is not run", nullptr } };
    auto root = project::git::repository_root(view.root());
    if (!root) return std::unexpected { query::Failure { "unavailable", root.error().message, nullptr } };
    ChangeSet changes;
    changes.base = request.base.empty() ? std::string { "HEAD" } : request.base;
    changes.repositoryRoot = *root;

    std::vector<project::git::Change> found;
    if (!request.files.empty()) {
        for (const auto& file : request.files) {
            const std::string path { view.path_of(file) };
            found.push_back(project::git::Change { platform::fs::exists(path) ? project::git::ChangeKind::modified : project::git::ChangeKind::removed, path, {} });
        }
    } else {
        auto listed = project::git::changes_since(view.root(), changes.base);
        if (!listed) return std::unexpected { query::Failure { "unavailable", listed.error().message, nullptr } };
        found = std::move(*listed);
    }
    for (auto& change : found) {
        FileChange file;
        file.kind = change.kind;
        file.path = platform::fs::exists(change.path) ? platform::fs::canonical_path(change.path) : change.path;
        file.oldPath = change.oldPath;
        const std::string basePath { change.oldPath.empty() ? change.path : change.oldPath };
        if (change.kind != project::git::ChangeKind::added && change.kind != project::git::ChangeKind::untracked) {
            auto shown = project::git::show(*root, changes.base, basePath);
            if (!shown) return std::unexpected { query::Failure { "unavailable", shown.error().message, nullptr } };
            file.base = std::move(*shown);
        }
        if (change.kind != project::git::ChangeKind::removed) file.head = text_of(view, file.path);
        // A file named directly that git does not know at the base is new.
        if (file.kind == project::git::ChangeKind::modified && !file.base) file.kind = project::git::ChangeKind::added;
        if (file.base && file.head && *file.base == *file.head) continue;
        file.hunks = line_hunks(file.base.value_or(std::string {}), file.head.value_or(std::string {}));
        changes.files.push_back(std::move(file));
    }
    std::ranges::sort(changes.files, {}, &FileChange::path);
    return changes;
}

Json to_json(const Hunk& hunk) {
    return Json { { "baseStart", hunk.baseStart }, { "baseCount", hunk.baseCount }, { "headStart", hunk.headStart }, { "headCount", hunk.headCount } };
}

} // namespace mcppls::ai::review
