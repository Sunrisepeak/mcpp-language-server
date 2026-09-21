// Verification after an edit (overall design 7.3, S5 3.7): the files an agent changed, and the
// units those changes can break — importers of a changed interface — built by the core engine with
// the content they have now, within a budget; and a candidate snippet checked in place before any
// file is written.
export module mcppls.ai.verify.changes;

import std;
import nlohmann.json;
import mcppls.spec.query;
import mcppls.ai.query.view;
import mcppls.ai.query.files;

export namespace mcppls::ai::verify {

struct ChangeOptions {
    std::vector<std::string> files;   // named changes
    bool workingTree { false };       // the working tree's changes (git)
    std::string base;                 // the changes since this revision (git)
    std::size_t budget { 32 };        // files checked at most, changed files first
};

struct CheckedFile {
    std::string file;
    std::string why;                  // changed | importer
    bool complete { false };
    std::string reason;
    std::vector<query::Diagnostic> diagnostics;
};

struct Verification {
    spec::Snapshot snapshot;
    std::string verdict;              // pass | errors | incomplete
    std::vector<std::string> changed;
    std::vector<std::string> removed;
    std::vector<CheckedFile> checked;
    std::vector<std::string> unchecked;
    std::map<std::string, int> counts;
    std::string semanticSource;
};

query::Outcome<Verification> verify_changes(query::View& view, const ChangeOptions& options, query::Clock::time_point deadline);

struct SnippetOptions {
    std::string file;
    int line { 0 };                   // from 1: the snippet goes before this line
    int replaceLines { 0 };           // lines of the file it replaces
    std::string code;
};

struct SnippetVerification {
    spec::Snapshot snapshot;
    std::string verdict;              // pass | errors | incomplete
    std::string file;
    int line { 0 };
    int endLine { 0 };                // the last line of the snippet in the candidate file
    bool complete { false };
    std::vector<query::Diagnostic> inSnippet;
    std::vector<query::Diagnostic> introduced;   // elsewhere in the file, and not there before
    std::map<std::string, int> counts;
};

// The file with the snippet in place is given to the engines as unsaved content, checked, and
// given back its own content; the file on disk is never written.
query::Outcome<SnippetVerification> verify_snippet(query::View& view, const SnippetOptions& options, query::Clock::time_point deadline);

// A proposed fix (S5 5.1: {"description", "edits": [{file, line, column, endLine, endColumn, newText}]})
// applied as unsaved content to the files it edits, checked, and taken back: whether it compiles
// without an error the files did not have before (overall design 7.4 step 6).
struct FixVerification {
    bool passes { false };
    bool complete { false };
    std::vector<query::Diagnostic> introduced;
    std::string reason;
};

query::Outcome<FixVerification> verify_fix(query::View& view, const nlohmann::json& fix, query::Clock::time_point deadline);

nlohmann::json to_json(const Verification& verification);
nlohmann::json to_json(const SnippetVerification& verification);

} // namespace mcppls::ai::verify
