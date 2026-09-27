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
    case SourceKind::xmake: return "xmake";
    case SourceKind::meson: return "meson";
    case SourceKind::compile_commands: return "compile-commands";
    case SourceKind::inferred: return "inferred";
    }
    return "inferred";
}

int tier_of(SourceKind kind) {
    switch (kind) {
    case SourceKind::build_database: return 1;
    case SourceKind::mcpp: return 1;
    case SourceKind::cmake: return 2;
    case SourceKind::xmake: return 3;
    case SourceKind::meson: return 3;
    case SourceKind::compile_commands: return 3;
    case SourceKind::inferred: return 4;
    }
    return 4;
}

SourceKind kind_of(std::string_view providerId) {
    if (providerId == "mcpp") return SourceKind::mcpp;
    if (providerId == "cmake") return SourceKind::cmake;
    if (providerId == "xmake") return SourceKind::xmake;
    if (providerId == "meson") return SourceKind::meson;
    if (providerId == "compile-commands") return SourceKind::compile_commands;
    return SourceKind::inferred;
}

Detection detect_project(std::string_view rootInput, std::string_view configuredDatabase,
                         std::span<BuildSystemProvider* const> providers, bool buildDiscovery) {
    Detection detection;
    detection.root = base::normalize_path(rootInput);
    const std::string& root { detection.root };
    if (!configuredDatabase.empty()) {
        detection.kind = SourceKind::build_database;
        detection.buildDatabase = base::join_path(root, configuredDatabase);
        return detection;
    }
    if (buildDiscovery) {
        for (BuildSystemProvider* provider : providers) {
            const auto claim = provider->detect(root);
            if (!claim) continue;
            detection.kind = kind_of(provider->id());
            detection.manifest = claim->manifest;
            detection.buildDirectory = claim->buildDirectory;
            detection.compileCommands = claim->compileCommands;
            detection.buildDatabase = claim->buildDatabase;
            return detection;
        }
    }
    detection.kind = SourceKind::inferred;
    return detection;
}

} // namespace mcppls::project
