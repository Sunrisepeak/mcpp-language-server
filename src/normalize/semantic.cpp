module mcppls.normalize.semantic;

import std;
import mcppls.spec.database;

namespace mcppls::normalize {

namespace {

void append(std::vector<std::string>& into, const std::vector<std::string>& from) { into.insert(into.end(), from.begin(), from.end()); }

bool msvc_dialect(spec::Family family) { return family == spec::Family::msvc || family == spec::Family::clang_cl; }

// "c++23" as the family writes it: -std=c++23 or -std=gnu++23; /std:c++20, /std:c++latest past what cl.exe names.
std::string standard_argument(std::string_view standard, bool gnu, bool msvc) {
    if (msvc) return standard == "c++20" || standard == "c++17" || standard == "c++14" ? std::format("/std:{}", standard) : std::string { "/std:c++latest" };
    if (gnu && standard.starts_with("c++")) return std::format("-std=gnu++{}", standard.substr(3));
    return std::format("-std={}", standard);
}

} // namespace

std::optional<spec::SemanticOptions> effective_options(const std::optional<spec::SemanticOptions>& set,
                                                       const std::optional<spec::SemanticOptions>& unit) {
    if (!set) return unit;
    if (!unit) return set;
    spec::SemanticOptions merged { *set };
    if (unit->languageStandard) merged.languageStandard = unit->languageStandard;
    if (unit->languageExtensions) merged.languageExtensions = unit->languageExtensions;
    merged.macros.insert(merged.macros.end(), unit->macros.begin(), unit->macros.end());
    append(merged.includeDirectories.user, unit->includeDirectories.user);
    append(merged.includeDirectories.quote, unit->includeDirectories.quote);
    append(merged.includeDirectories.system, unit->includeDirectories.system);
    append(merged.includeDirectories.after, unit->includeDirectories.after);
    append(merged.forcedIncludes, unit->forcedIncludes);
    if (unit->exceptions) merged.exceptions = unit->exceptions;
    if (unit->rtti) merged.rtti = unit->rtti;
    for (const auto& [family, arguments] : unit->rawSemanticArguments) {
        const auto existing = std::ranges::find_if(merged.rawSemanticArguments, [&](const auto& entry) { return entry.first == family; });
        if (existing == merged.rawSemanticArguments.end()) merged.rawSemanticArguments.emplace_back(family, arguments);
        else append(existing->second, arguments);
    }
    return merged;
}

std::vector<std::string> options_arguments(const spec::SemanticOptions& options, spec::Family family, std::string_view driver,
                                           std::string_view source) {
    const bool msvc { msvc_dialect(family) };
    std::vector<std::string> arguments { std::string { driver } };
    if (options.languageStandard) arguments.push_back(standard_argument(*options.languageStandard, options.languageExtensions == "gnu", msvc));
    if (options.languageExtensions == "ms") arguments.emplace_back(msvc ? "/permissive" : "-fms-extensions");
    else if (msvc && options.languageExtensions == "none") arguments.emplace_back("/permissive-");
    for (const auto& macro : options.macros) {
        if (macro.undefine) {
            arguments.push_back((msvc ? "/U" : "-U") + macro.name);
        } else {
            arguments.push_back((msvc ? "/D" : "-D") + macro.name + (macro.value ? "=" + *macro.value : std::string {}));
        }
    }
    const auto& directories = options.includeDirectories;
    for (const auto& directory : directories.quote) {
        if (msvc) arguments.push_back("/I" + directory);
        else arguments.insert(arguments.end(), { "-iquote", directory });
    }
    for (const auto& directory : directories.user) arguments.push_back((msvc ? "/I" : "-I") + directory);
    for (const auto& directory : directories.system) {
        if (msvc) arguments.insert(arguments.end(), { "/external:I", directory });
        else arguments.insert(arguments.end(), { "-isystem", directory });
    }
    for (const auto& directory : directories.after) {
        if (msvc) arguments.push_back("/I" + directory);
        else arguments.insert(arguments.end(), { "-idirafter", directory });
    }
    for (const auto& header : options.forcedIncludes) {
        if (msvc) arguments.push_back("/FI" + header);
        else arguments.insert(arguments.end(), { "-include", header });
    }
    if (options.exceptions) {
        if (msvc) arguments.emplace_back(*options.exceptions ? "/EHsc" : "/EHs-c-");
        else if (!*options.exceptions) arguments.emplace_back("-fno-exceptions");
    }
    if (options.rtti && !*options.rtti) arguments.emplace_back(msvc ? "/GR-" : "-fno-rtti");
    for (const auto& [name, raw] : options.rawSemanticArguments) {
        if (spec::parse_family(name) == family) append(arguments, raw);
    }
    arguments.emplace_back(msvc ? "/c" : "-c");
    arguments.emplace_back(source);
    return arguments;
}

} // namespace mcppls::normalize
