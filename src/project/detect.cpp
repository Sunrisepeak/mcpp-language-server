module mcppls.project.detect;

import std;
import mcppls.base.path;
import mcppls.platform.fs;

namespace mcppls::project {

namespace fs = platform::fs;

std::string_view to_string(SourceKind kind) {
    switch (kind) {
    case SourceKind::build_database: return "build-database";
    case SourceKind::mcpp: return "mcpp";
    case SourceKind::cmake: return "cmake";
    case SourceKind::compile_commands: return "compile-commands";
    case SourceKind::inferred: return "inferred";
    }
    return "inferred";
}

namespace {

std::vector<std::string> cmake_build_directories(std::string_view root) {
    std::vector<std::string> found;
    auto consider = [&](const std::string& directory) {
        if (fs::is_regular_file(base::join_path(directory, "CMakeCache.txt"))) found.push_back(directory);
    };
    for (std::string_view name : { "build", "out/build", "cmake-build-debug", "cmake-build-release", "builddir" }) {
        const std::string directory { base::join_path(root, name) };
        consider(directory);
        if (fs::is_directory(directory)) {
            for (const auto& child : fs::list_directory(directory)) {
                if (fs::is_directory(child)) consider(child);
            }
        }
    }
    for (const auto& child : fs::list_directory(root)) {
        const std::string_view name { base::file_name(child) };
        if ((name.starts_with("build-") || name.starts_with("cmake-build-")) && fs::is_directory(child)) consider(child);
    }
    std::ranges::sort(found);
    found.erase(std::unique(found.begin(), found.end()), found.end());
    return found;
}

} // namespace

Detection detect_project(std::string_view rootInput, std::string_view configuredDatabase) {
    Detection detection;
    detection.root = base::normalize_path(rootInput);
    const std::string& root { detection.root };
    if (!configuredDatabase.empty()) {
        detection.kind = SourceKind::build_database;
        detection.buildDatabase = base::join_path(root, configuredDatabase);
        return detection;
    }
    if (const std::string manifest { base::join_path(root, "mcpp.toml") }; fs::is_regular_file(manifest)) {
        detection.kind = SourceKind::mcpp;
        detection.manifest = manifest;
        if (const std::string commands { base::join_path(root, "compile_commands.json") }; fs::is_regular_file(commands)) {
            detection.compileCommands = commands;
        }
        return detection;
    }
    if (const std::string manifest { base::join_path(root, "CMakeLists.txt") }; fs::is_regular_file(manifest)) {
        detection.kind = SourceKind::cmake;
        detection.manifest = manifest;
        for (const auto& directory : cmake_build_directories(root)) {
            const std::string database { base::join_path(directory, "build_database.json") };
            const std::string commands { base::join_path(directory, "compile_commands.json") };
            if (fs::is_regular_file(database) && detection.buildDatabase.empty()) {
                detection.buildDirectory = directory;
                detection.buildDatabase = database;
            }
            if (fs::is_regular_file(commands) && detection.compileCommands.empty()) {
                if (detection.buildDirectory.empty()) detection.buildDirectory = directory;
                detection.compileCommands = commands;
            }
        }
        if (detection.buildDirectory.empty()) {
            if (const auto directories = cmake_build_directories(root); !directories.empty()) detection.buildDirectory = directories.front();
        }
        return detection;
    }
    for (std::string_view candidate : { "compile_commands.json", "build/compile_commands.json" }) {
        if (const std::string commands { base::join_path(root, candidate) }; fs::is_regular_file(commands)) {
            detection.kind = SourceKind::compile_commands;
            detection.compileCommands = commands;
            return detection;
        }
    }
    detection.kind = SourceKind::inferred;
    return detection;
}

} // namespace mcppls::project
