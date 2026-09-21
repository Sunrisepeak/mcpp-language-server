// What verification and review ask git (overall design 7.3, 7.4): the repository a workspace is in,
// the files changed in its working tree, and a file's content at a revision. git runs only in a
// trusted workspace; callers check that.
export module mcppls.project.git;

import std;
import mcppls.base.error;

export namespace mcppls::project::git {

enum class ChangeKind { added, modified, removed, renamed, untracked };
std::string_view to_string(ChangeKind kind);

struct Change {
    ChangeKind kind { ChangeKind::modified };
    std::string path;       // absolute, normalized
    std::string oldPath;    // for a rename, absolute
};

// The top-level directory of the repository containing `directory`.
base::Result<std::string> repository_root(std::string_view directory);
// Changes of the working tree and the index against HEAD, untracked files included, under `within`.
base::Result<std::vector<Change>> working_tree_changes(std::string_view within);
// Changes between `base` (a revision) and the working tree, under `within`.
base::Result<std::vector<Change>> changes_since(std::string_view within, std::string_view base);
// A file's content at a revision; nullopt when the file did not exist there.
base::Result<std::optional<std::string>> show(std::string_view repositoryRoot, std::string_view revision, std::string_view absolutePath);

// Parses `git status --porcelain=v1 -z` output; paths are relative to `repositoryRoot`.
std::vector<Change> parse_status(std::string_view output, std::string_view repositoryRoot);
// Parses `git diff --name-status -z` output.
std::vector<Change> parse_name_status(std::string_view output, std::string_view repositoryRoot);

} // namespace mcppls::project::git
