module mcppls.project.generated;

import std;
import mcppls.base.path;
import mcppls.platform.fs;
import mcppls.project.scan;

namespace mcppls::project {

namespace {

constexpr std::array<std::string_view, 6> MODULE_EXTENSIONS { ".cppm", ".ccm", ".cxxm", ".ixx", ".mpp", ".mxx" };

bool has_module_extension(std::string_view path) {
    return std::ranges::any_of(MODULE_EXTENSIONS, [&](std::string_view extension) { return path.ends_with(extension); });
}

// Every `out/` directory of a `<base>/target/.build-mcpp/deps/<pkg>@<ver>/` layout, whatever
// packages and versions are there: only the shape is conventional, not the names.
std::vector<std::string> deps_out_directories(std::string_view base) {
    std::vector<std::string> out;
    const std::string deps { base::join_path(base, "target/.build-mcpp/deps") };
    if (!platform::fs::is_directory(deps)) return out;
    for (const auto& package : platform::fs::list_directory(deps)) {
        if (const std::string candidate { base::join_path(package, "out") }; platform::fs::is_directory(candidate)) out.push_back(candidate);
    }
    return out;
}

// Every file in one of `directories` that exports exactly `moduleName` (never a partition: a
// partition is never what an import of the bare module name needs).
std::vector<std::string> matches(std::string_view moduleName, std::span<const std::string> directories, const Scanner& scanner) {
    std::vector<std::string> found;
    for (const auto& directory : directories) {
        for (const auto& entry : platform::fs::list_directory(directory)) {
            if (platform::fs::is_directory(entry) || !has_module_extension(entry)) continue;
            const ScanResult scanned { scanner(entry) };
            if (scanned.declaration && scanned.declaration->isExported && scanned.declaration->partition.empty()
                && scanned.declaration->module == moduleName) {
                found.push_back(entry);
            }
        }
    }
    return found;
}

// The most recently written of `candidates`: several cached builds (of several versions of a
// package) may each have generated the module, and the latest build is the best guess at the one
// this project uses. Ties and unreadable stamps keep the earlier candidate, so the choice is stable.
std::optional<std::string> newest(const std::vector<std::string>& candidates) {
    std::optional<std::string> best;
    std::int64_t bestModified { std::numeric_limits<std::int64_t>::min() };
    for (const auto& candidate : candidates) {
        const auto stamp = platform::fs::stamp(candidate);
        const std::int64_t modified { stamp ? stamp->modified : std::numeric_limits<std::int64_t>::min() };
        if (!best || modified > bestModified) {
            best = candidate;
            bestModified = modified;
        }
    }
    return best;
}

} // namespace

std::optional<std::string> find_generated_source(std::string_view moduleName, const GeneratedSourceOptions& options) {
    if (!options.scanner) return std::nullopt;
    // The project's own build directory first: what this project's last build generated.
    if (auto own = newest(matches(moduleName, deps_out_directories(options.root), options.scanner))) return own;
    const std::string cacheRoot { base::join_path(options.homeDirectory, ".mcpp/cache/build-database") };
    if (!platform::fs::is_directory(cacheRoot)) return std::nullopt;
    std::vector<std::string> cached;
    for (const auto& built : platform::fs::list_directory(cacheRoot)) {
        if (!platform::fs::is_directory(built)) continue;
        std::ranges::move(matches(moduleName, deps_out_directories(built), options.scanner), std::back_inserter(cached));
    }
    return newest(cached);
}

} // namespace mcppls::project
