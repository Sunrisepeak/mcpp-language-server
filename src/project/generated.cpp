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

// The first file in one of `directories` that exports exactly `moduleName` (never a partition: a
// partition is never what an import of the bare module name needs).
std::optional<std::string> search(std::string_view moduleName, std::span<const std::string> directories, const Scanner& scanner) {
    for (const auto& directory : directories) {
        for (const auto& entry : platform::fs::list_directory(directory)) {
            if (platform::fs::is_directory(entry) || !has_module_extension(entry)) continue;
            const ScanResult scanned { scanner(entry) };
            if (scanned.declaration && scanned.declaration->isExported && scanned.declaration->partition.empty()
                && scanned.declaration->module == moduleName) {
                return entry;
            }
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<std::string> find_generated_source(std::string_view moduleName, const GeneratedSourceOptions& options) {
    if (!options.scanner) return std::nullopt;
    if (auto found = search(moduleName, deps_out_directories(options.root), options.scanner)) return found;
    const std::string cacheRoot { base::join_path(options.homeDirectory, ".mcpp/cache/build-database") };
    if (!platform::fs::is_directory(cacheRoot)) return std::nullopt;
    for (const auto& built : platform::fs::list_directory(cacheRoot)) {
        if (!platform::fs::is_directory(built)) continue;
        if (auto found = search(moduleName, deps_out_directories(built), options.scanner)) return found;
    }
    return std::nullopt;
}

} // namespace mcppls::project
