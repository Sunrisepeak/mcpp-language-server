module mcppls.project.generated;

import std;
import mcppls.base.path;
import mcppls.spec.database;
import mcppls.project.scan;
import mcpplibs.os;

namespace os = mcpplibs::os;

namespace mcppls::project {

namespace {

constexpr std::array<std::string_view, 6> MODULE_EXTENSIONS { ".cppm", ".ccm", ".cxxm", ".ixx", ".mpp", ".mxx" };

bool has_module_extension(std::string_view path) {
    return std::ranges::any_of(MODULE_EXTENSIONS, [&](std::string_view extension) { return path.ends_with(extension); });
}

// Every `out/` directory of a `<base>/target/.build-mcpp/deps/<pkg>@<ver>/` layout, whatever
// packages and versions are there: only the shape is conventional, not the names.
std::vector<std::string> deps_out_directories(std::string_view base) {
    std::error_code ignored;
    std::vector<std::string> out;
    const std::string deps { base::join_path(base, "target/.build-mcpp/deps") };
    if (!std::filesystem::is_directory(deps, ignored)) return out;
    for (const auto& package : base::children(deps)) {
        if (const std::string candidate { base::join_path(package, "out") }; std::filesystem::is_directory(candidate, ignored)) out.push_back(candidate);
    }
    return out;
}

// Every file in one of `directories` that exports exactly `moduleName` (never a partition: a
// partition is never what an import of the bare module name needs).
std::vector<std::string> matches(std::string_view moduleName, std::span<const std::string> directories, const Scanner& scanner) {
    std::error_code ignored;
    std::vector<std::string> found;
    for (const auto& directory : directories) {
        for (const auto& entry : base::children(directory)) {
            if (std::filesystem::is_directory(entry, ignored) || !has_module_extension(entry)) continue;
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
        const auto stamp = base::file_stamp(candidate);
        const std::int64_t modified { stamp ? static_cast<std::int64_t>(stamp->modified_ns) : std::numeric_limits<std::int64_t>::min() };
        if (!best || modified > bestModified) {
            best = candidate;
            bestModified = modified;
        }
    }
    return best;
}

} // namespace

std::optional<std::string> find_generated_source(std::string_view moduleName, const GeneratedSourceOptions& options) {
    std::error_code ignored;
    if (!options.scanner) return std::nullopt;
    // The project's own build directory first: what this project's last build generated.
    if (auto own = newest(matches(moduleName, deps_out_directories(options.root), options.scanner))) return own;
    const std::string cacheRoot { base::join_path(options.homeDirectory, ".mcpp/cache/build-database") };
    if (!std::filesystem::is_directory(cacheRoot, ignored)) return std::nullopt;
    std::vector<std::string> cached;
    for (const auto& built : base::children(cacheRoot)) {
        if (!std::filesystem::is_directory(built, ignored)) continue;
        std::ranges::move(matches(moduleName, deps_out_directories(built), options.scanner), std::back_inserter(cached));
    }
    return newest(cached);
}

std::optional<std::string> private_target_relative(std::string_view path) {
    const std::string normalized { base::normalize_path(path) };
    constexpr std::string_view MARKER { "/cache/build-database/" };
    const std::size_t marker { normalized.find(MARKER) };
    if (marker == std::string::npos) return std::nullopt;
    const std::size_t key { marker + MARKER.size() };
    const std::size_t afterKey { normalized.find('/', key) };
    if (afterKey == std::string::npos || afterKey == key) return std::nullopt;
    constexpr std::string_view TARGET { "/target/" };
    if (normalized.compare(afterKey, TARGET.size(), TARGET) != 0) return std::nullopt;
    return normalized.substr(afterKey + TARGET.size());
}

namespace {

// Nothing a reader could use: absent, or only empty files (mcpp leaves an empty placeholder where an action's
// source output will go, so that the plan can name it).
bool holds_nothing(std::string_view path) {
    std::error_code ignored;
    if (std::filesystem::is_regular_file(path, ignored)) {
        const auto stamp = base::file_stamp(path);
        return !stamp || stamp->size == 0;
    }
    if (!std::filesystem::is_directory(path, ignored)) return true;
    return std::ranges::all_of(base::children(path), [](const std::string& entry) {
        std::error_code ignored;
        return !std::filesystem::is_directory(entry, ignored) && holds_nothing(entry);
    });
}

// The option spellings that name an include directory, as one argument (`-I/x`) or with the next one (`-I /x`).
constexpr std::array<std::string_view, 5> INCLUDE_OPTIONS { "-I", "-isystem", "-iquote", "-idirafter", "/I" };

} // namespace

GeneratedPaths use_project_build_output(spec::Database& database, std::string_view root) {
    GeneratedPaths paths;
    std::map<std::string, std::optional<std::string>, std::less<>> decided;   // private path -> its replacement, if any
    auto replacement = [&](const std::string& path, bool directory) -> std::optional<std::string> {
        std::error_code ignored;
        if (const auto known = decided.find(path); known != decided.end()) return known->second;
        std::optional<std::string> chosen;
        if (const auto relative = private_target_relative(path)) {
            const std::string counterpart { base::join_path(root, base::join_path("target", *relative)) };
            const bool exists { directory ? std::filesystem::is_directory(counterpart, ignored) : std::filesystem::is_regular_file(counterpart, ignored) };
            if (exists && !holds_nothing(counterpart)) {
                chosen = counterpart;
                paths.relocated.push_back(path);
            } else if (holds_nothing(path)) {
                paths.missing.push_back(path);
                paths.watch.push_back(base::join_path("target", directory ? base::join_path(*relative, "*") : *relative));
            }
        }
        decided.emplace(path, chosen);
        return chosen;
    };
    auto rewrite_arguments = [&](std::vector<std::string>& arguments) {
        for (std::size_t i { 0 }; i < arguments.size(); ++i) {
            for (const std::string_view option : INCLUDE_OPTIONS) {
                if (!arguments[i].starts_with(option)) continue;
                if (arguments[i].size() == option.size()) {
                    if (i + 1 < arguments.size()) {
                        if (auto chosen = replacement(arguments[i + 1], true)) arguments[i + 1] = *chosen;
                        ++i;
                    }
                } else if (auto chosen = replacement(arguments[i].substr(option.size()), true)) {
                    arguments[i] = std::string { option } + *chosen;
                }
                break;
            }
        }
    };
    for (auto& set : database.sets) {
        rewrite_arguments(set.baselineArguments);
        for (auto& unit : set.units) {
            rewrite_arguments(unit.arguments);
            rewrite_arguments(unit.localArguments);
            const std::string source { spec::absolute_source(unit) };
            if (auto chosen = replacement(source, false)) {
                // The command names the source too; it names the same file.
                for (auto& argument : unit.arguments) {
                    if (base::same_path(argument, source) || base::same_path(argument, unit.source)) argument = *chosen;
                }
                unit.source = *chosen;
            }
        }
    }
    return paths;
}

} // namespace mcppls::project
