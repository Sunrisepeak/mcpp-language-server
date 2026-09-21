// What a review looks at (overall design 7.4 step 1): the files a change touches, each with its
// content before and after and the lines that differ, from git or from files named directly.
export module mcppls.ai.review.changes;

import std;
import nlohmann.json;
import mcppls.project.git;
import mcppls.ai.query.view;

export namespace mcppls::ai::review {

// Lines from 1; a count of 0 is a pure insertion (base) or deletion (head) before the start line.
struct Hunk {
    int baseStart { 1 };
    int baseCount { 0 };
    int headStart { 1 };
    int headCount { 0 };
};

// The differing line ranges of two texts (Myers' algorithm on lines).
std::vector<Hunk> line_hunks(std::string_view base, std::string_view head);

struct FileChange {
    std::string path;                        // absolute; the new path of a rename
    std::string oldPath;                     // the base path of a rename
    project::git::ChangeKind kind { project::git::ChangeKind::modified };
    std::optional<std::string> base;         // nullopt: the file is new
    std::optional<std::string> head;         // nullopt: the file is removed
    std::vector<Hunk> hunks;
    // Whether a line of the head version is one the change added or altered.
    bool changed_head_line(int line) const;
};

struct ChangeRequest {
    std::string base { "HEAD" };             // the revision the change is against
    bool workingTree { true };               // the working tree's changes, untracked files included
    std::vector<std::string> files;          // or just these files, against `base`
};

struct ChangeSet {
    std::string base;
    std::string repositoryRoot;
    std::vector<FileChange> files;
};

// Needs a trusted workspace in a git repository.
query::Outcome<ChangeSet> collect_changes(query::View& view, const ChangeRequest& request);

nlohmann::json to_json(const Hunk& hunk);

} // namespace mcppls::ai::review
