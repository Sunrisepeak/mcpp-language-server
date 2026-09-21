// Which kind of project a workspace folder is, and where its build facts live
// (design section 14.1).
export module mcppls.project.detect;

import std;

export namespace mcppls::project {

enum class SourceKind { build_database, mcpp, cmake, compile_commands, inferred };

std::string_view to_string(SourceKind kind);

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
