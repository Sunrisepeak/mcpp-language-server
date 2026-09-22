// Which kind of project a workspace folder is, and where its build facts live
// (design section 14.1).
export module mcppls.project.detect;

import std;

export namespace mcppls::project {

enum class SourceKind { build_database, mcpp, cmake, compile_commands, inferred };

std::string_view to_string(SourceKind kind);

// The README's and design §2.1's L1..L4: which kind of source described the model, independent of
// S1's own 1..4 conformance level (how completely that source's *document* is structured). The two
// numbers happened to share a range and a user reading "level 2" could not tell which one it was;
// `project.tier` (S3) is this one, `project.level` stays S1's. build-database and mcpp are both the
// best case (a real build tool's or a hand-written database's own word); cmake without a database
// file falls back one tier below it, a bare compile_commands.json one more, and inferred (scanning,
// or an untrusted workspace, which is L4 by definition) is the last.
int tier_of(SourceKind kind);

struct Detection {
    SourceKind kind { SourceKind::inferred };
    std::string root;
    std::string manifest;          // mcpp.toml or CMakeLists.txt
    std::string buildDirectory;    // an existing CMake build directory
    std::string compileCommands;   // an existing compile_commands.json
    std::string buildDatabase;     // an existing P2977 / S1 document
};

Detection detect_project(std::string_view root, std::string_view configuredDatabase = {});

} // namespace mcppls::project
