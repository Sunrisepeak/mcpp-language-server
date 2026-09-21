module mcppls.spec.options;

import std;
import mcppls.base.path;
import mcppls.spec.database;

namespace mcppls::spec {

namespace {

using Group = std::vector<std::string>;   // one option with the value it takes as the next argument

bool msvc_dialect(Family family) { return family == Family::msvc || family == Family::clang_cl; }

bool starts_with_any(std::string_view argument, std::span<const std::string_view> prefixes) {
    return std::ranges::any_of(prefixes, [&](std::string_view prefix) { return argument.starts_with(prefix); });
}

bool equals_any(std::string_view argument, std::span<const std::string_view> values) {
    return std::ranges::find(values, argument) != values.end();
}

// ---- GCC and Clang -------------------------------------------------------------------------------

constexpr std::array<std::string_view, 24> GNU_SEPARATE {
    "-o", "-MF", "-MT", "-MQ", "-x", "-Xclang", "-Xpreprocessor", "-D", "-U", "-I", "-iquote", "-isystem", "-idirafter", "-include",
    "-imacros", "-include-pch", "-isysroot", "--sysroot", "-target", "-iframework", "-F", "-iprefix", "-iwithprefix", "-arch",
};

// Build outputs, dependency files and module mechanics: a unit's role and its module graph say
// what these do for semantics, and a BMI location never belongs in options (S1-9-4).
constexpr std::array<std::string_view, 19> GNU_DROP_EXACT {
    "-c", "-S", "-E", "--precompile", "-M", "-MM", "-MD", "-MMD", "-MP", "-MG", "-fmodules-ts", "-fmodules", "-fno-modules",
    "-fno-modules-ts", "-fmodule-only", "-fmodule-lazy", "-fno-module-lazy", "-fmodules-reduced-bmi", "-fmodules-embed-all-files",
};
constexpr std::array<std::string_view, 17> GNU_DROP_PREFIX {
    "-fmodule-file=", "-fprebuilt-module-path=", "-fmodule-output", "-fmodule-mapper=", "-fdeps-", "-fmodule-header",
    "-flang-info-", "-fexperimental-modules-reduced-bmi", "-MF", "-MT", "-MQ",
    // Diagnostics as displayed, and what only code generation or tooling reads (S1-9-1).
    "-fdiagnostics-", "-fcolor-diagnostics", "-fno-color-diagnostics", "-fansi-escape-codes", "-fmessage-length=", "-ferror-limit=",
};

bool gnu_non_semantic(std::string_view argument) {
    if (equals_any(argument, GNU_DROP_EXACT) || starts_with_any(argument, GNU_DROP_PREFIX)) return true;
    if (argument.starts_with("-O") && !argument.starts_with("-ObjC")) return true;           // optimization
    if (argument.starts_with("-g") && !argument.starts_with("-gcc-toolchain")) return true;  // debug information
    if (argument.starts_with("-W") && !argument.starts_with("-Wp,")) return true;            // warnings, and -Wa, / -Wl,
    if (argument == "-w" || argument == "-pedantic" || argument == "-pedantic-errors") return true;
    if (argument.starts_with("-o") && argument.size() > 2) return true;                      // -o<file>
    if (argument.starts_with("-x") && argument.size() > 2) return true;                      // -x<language>
    if (argument.starts_with('@') && argument.ends_with(".modmap")) return true;             // CMake's module map
    return false;
}

void add_macro(SemanticOptions& options, std::string_view text, bool undefine) {
    if (undefine) {
        options.macros.push_back(Macro { std::string { text }, std::nullopt, true });
        return;
    }
    const std::size_t equals { text.find('=') };
    if (equals == std::string_view::npos) {
        options.macros.push_back(Macro { std::string { text }, std::nullopt, false });
    } else {
        options.macros.push_back(Macro { std::string { text.substr(0, equals) }, std::string { text.substr(equals + 1) }, false });
    }
}

// `-std=c++23` and `-std=gnu++23`; a standard of another language has no structured form.
bool structure_gnu_standard(SemanticOptions& options, std::string_view value) {
    if (value.starts_with("c++")) {
        options.languageStandard = std::string { value };
        options.languageExtensions = "none";
        return true;
    }
    if (value.starts_with("gnu++")) {
        options.languageStandard = std::format("c++{}", value.substr(5));
        options.languageExtensions = "gnu";
        return true;
    }
    return false;
}

// One group in the GNU dialect: true when structured or dropped, false when it stays raw.
bool structure_gnu(SemanticOptions& options, const Group& group) {
    const std::string_view flag { group.front() };
    const bool separate { group.size() == 2 };
    const std::string_view value { separate ? std::string_view { group[1] } : std::string_view {} };
    if (separate) {
        if (flag == "-o" || flag == "-MF" || flag == "-MT" || flag == "-MQ" || flag == "-x" || flag == "-include-pch") return true;
        if (flag == "-D" || flag == "-U") {
            add_macro(options, value, flag == "-U");
            return true;
        }
        if (flag == "-I") return options.includeDirectories.user.emplace_back(value), true;
        if (flag == "-iquote") return options.includeDirectories.quote.emplace_back(value), true;
        if (flag == "-isystem") return options.includeDirectories.system.emplace_back(value), true;
        if (flag == "-idirafter") return options.includeDirectories.after.emplace_back(value), true;
        if (flag == "-include") return options.forcedIncludes.emplace_back(value), true;
        return false;
    }
    if (gnu_non_semantic(flag)) return true;
    if (flag.starts_with("-std=")) return structure_gnu_standard(options, flag.substr(5));
    if (flag.starts_with("-D") && flag.size() > 2) return add_macro(options, flag.substr(2), false), true;
    if (flag.starts_with("-U") && flag.size() > 2) return add_macro(options, flag.substr(2), true), true;
    if (flag.starts_with("-I") && flag.size() > 2 && flag != "-I-") return options.includeDirectories.user.emplace_back(flag.substr(2)), true;
    if (flag.starts_with("-iquote") && flag.size() > 7) return options.includeDirectories.quote.emplace_back(flag.substr(7)), true;
    if (flag.starts_with("-isystem") && flag.size() > 8) return options.includeDirectories.system.emplace_back(flag.substr(8)), true;
    if (flag.starts_with("-idirafter") && flag.size() > 10) return options.includeDirectories.after.emplace_back(flag.substr(10)), true;
    if (flag == "-fexceptions" || flag == "-fno-exceptions") return options.exceptions = flag == "-fexceptions", true;
    if (flag == "-frtti" || flag == "-fno-rtti") return options.rtti = flag == "-frtti", true;
    return false;
}

// ---- cl.exe and clang-cl -------------------------------------------------------------------------

constexpr std::array<std::string_view, 23> MSVC_SEPARATE {
    "D", "U", "I", "FI", "external:I", "imsvc", "Tp", "Tc", "reference", "ifcOutput", "ifcSearchDir", "ifcMap", "headerUnit",
    "headerUnit:quote", "headerUnit:angle", "sourceDependencies", "sourceDependencies:directives", "scanDependencies", "Fo:", "Fe:",
    "Fd:", "Fp:", "Xclang",
};

constexpr std::array<std::string_view, 27> MSVC_DROP_EXACT {
    "c", "nologo", "showIncludes", "TP", "TC", "FS", "bigobj", "Gy", "Gy-", "GL", "GL-", "Gm", "Gm-", "interface", "internalPartition",
    "Z7", "Zi", "ZI", "Wall", "WX", "WX-", "w", "JMC", "JMC-", "FC", "utf-8-diagnostics", "MP",
};
constexpr std::array<std::string_view, 23> MSVC_DROP_PREFIX {
    "Fo", "Fd", "Fe", "Fa", "Fp", "Fm", "FR", "Fr", "Yc", "Yu", "wd", "we", "wo", "external:W", "diagnostics:", "errorReport:",
    "ifc", "reference", "headerUnit", "sourceDependencies", "scanDependencies", "Tp", "Tc",
};

bool msvc_non_semantic(std::string_view body) {
    if (equals_any(body, MSVC_DROP_EXACT) || starts_with_any(body, MSVC_DROP_PREFIX)) return true;
    if (body.size() >= 2 && body.front() == 'O') return true;                                   // optimization
    if (body.size() >= 2 && (body.front() == 'W' || body.front() == 'w') && std::isdigit(static_cast<unsigned char>(body[1]))) return true;
    if (body.starts_with("MP") && body.size() > 2 && std::isdigit(static_cast<unsigned char>(body[2]))) return true;
    return false;
}

std::optional<std::string_view> msvc_body(std::string_view argument) {
    if (argument.size() > 1 && (argument.front() == '/' || argument.front() == '-')) return argument.substr(1);
    return std::nullopt;
}

bool structure_msvc(SemanticOptions& options, const Group& group) {
    const auto body = msvc_body(group.front());
    if (!body) return false;
    if (group.size() == 2) {
        const std::string_view value { group[1] };
        if (*body == "D" || *body == "U") return add_macro(options, value, *body == "U"), true;
        if (*body == "I") return options.includeDirectories.user.emplace_back(value), true;
        if (*body == "external:I" || *body == "imsvc") return options.includeDirectories.system.emplace_back(value), true;
        if (*body == "FI") return options.forcedIncludes.emplace_back(value), true;
        return *body != "Xclang";   // sources, BMI references and outputs go; -Xclang's value stays with it
    }
    if (msvc_non_semantic(*body)) return true;
    if (body->starts_with("std:c++")) return options.languageStandard = std::string { body->substr(4) }, true;
    if (*body == "permissive-") return options.languageExtensions = "none", true;
    if (*body == "permissive") return options.languageExtensions = "ms", true;
    if (body->starts_with("EH")) return options.exceptions = !(body->ends_with('-') || *body == "EHs-c-"), true;
    if (*body == "GR" || *body == "GR-") return options.rtti = *body == "GR", true;
    if (body->starts_with("D") && body->size() > 1) return add_macro(options, body->substr(1), false), true;
    if (body->starts_with("U") && body->size() > 1) return add_macro(options, body->substr(1), true), true;
    if (body->starts_with("I") && body->size() > 1) return options.includeDirectories.user.emplace_back(body->substr(1)), true;
    if (body->starts_with("external:I") && body->size() > 10) return options.includeDirectories.system.emplace_back(body->substr(10)), true;
    if (body->starts_with("FI") && body->size() > 2) return options.forcedIncludes.emplace_back(body->substr(2)), true;
    return false;
}

// ---- both ------------------------------------------------------------------------------------------

bool takes_separate_value(std::string_view argument, Family family) {
    if (!msvc_dialect(family)) return equals_any(argument, GNU_SEPARATE);
    const auto body = msvc_body(argument);
    return body && equals_any(*body, MSVC_SEPARATE);
}

std::vector<Group> groups_of(std::span<const std::string> arguments, Family family) {
    std::vector<Group> groups;
    for (std::size_t i { 0 }; i < arguments.size(); ++i) {
        Group group { arguments[i] };
        if (takes_separate_value(arguments[i], family) && i + 1 < arguments.size()) group.push_back(arguments[++i]);
        groups.push_back(std::move(group));
    }
    return groups;
}

std::vector<std::string> flatten(std::span<const Group> groups) {
    std::vector<std::string> arguments;
    for (const auto& group : groups) arguments.insert(arguments.end(), group.begin(), group.end());
    return arguments;
}

bool empty_options(const SemanticOptions& options) {
    const auto& directories = options.includeDirectories;
    return !options.languageStandard && !options.languageExtensions && options.macros.empty() && directories.user.empty()
        && directories.quote.empty() && directories.system.empty() && directories.after.empty() && options.forcedIncludes.empty()
        && !options.exceptions && !options.rtti && options.rawSemanticArguments.empty();
}

// An operand rather than an option: the unit's source, or any other input the command names.
bool is_input(std::string_view argument, Family family, const TranslationUnit& unit, std::string_view source) {
    if (base::same_path(absolute_path_in_unit(unit, argument), source)) return true;
    if (argument.starts_with('@')) return false;   // a response file keeps its arguments' meaning
    return msvc_dialect(family) ? !msvc_body(argument).has_value() : !argument.starts_with('-');
}

// Groups without the unit's inputs. mcpp's local-arguments for a standard library unit name its source,
// as in `--precompile <source> -o <bmi>`.
std::vector<Group> without_inputs(std::vector<Group> groups, const TranslationUnit& unit, Family family) {
    const std::string source { absolute_source(unit) };
    std::erase_if(groups, [&](const Group& group) { return group.size() == 1 && is_input(group.front(), family, unit, source); });
    return groups;
}

// A unit's command line without its driver and its inputs.
std::vector<Group> unit_groups(const TranslationUnit& unit, Family family) {
    if (unit.arguments.empty()) return {};
    return without_inputs(groups_of(std::span { unit.arguments }.subspan(1), family), unit, family);
}

// The groups of `from` not matched, one for one, by a group of `minus`.
std::vector<Group> difference(std::vector<Group> from, std::span<const Group> minus) {
    for (const auto& group : minus) {
        if (const auto found = std::ranges::find(from, group); found != from.end()) from.erase(found);
    }
    return from;
}

// The groups every unit has, in the first unit's order.
std::vector<Group> shared_groups(const Set& set, Family family) {
    if (set.units.empty()) return {};
    std::vector<Group> shared { unit_groups(set.units.front(), family) };
    for (std::size_t i { 1 }; i < set.units.size() && !shared.empty(); ++i) {
        std::vector<Group> remaining { unit_groups(set.units[i], family) };
        std::vector<Group> kept;
        for (auto& group : shared) {
            if (const auto found = std::ranges::find(remaining, group); found != remaining.end()) {
                remaining.erase(found);
                kept.push_back(std::move(group));
            }
        }
        shared = std::move(kept);
    }
    return shared;
}

} // namespace

SemanticOptions structure_arguments(std::span<const std::string> arguments, Family family) {
    SemanticOptions options;
    std::vector<std::string> raw;
    for (const auto& group : groups_of(arguments, family)) {
        const bool handled { msvc_dialect(family) ? structure_msvc(options, group) : structure_gnu(options, group) };
        if (!handled) raw.insert(raw.end(), group.begin(), group.end());
    }
    if (!raw.empty()) options.rawSemanticArguments.emplace_back(std::string { to_string(family) }, std::move(raw));
    return options;
}

void complete_options(Database& database) {
    for (auto& set : database.sets) {
        const Toolchain* toolchain { find_toolchain(database, set.toolchain) };
        if (set.options || toolchain == nullptr || toolchain->family == Family::other) continue;
        const Family family { toolchain->family };
        const std::vector<Group> baseline { set.baselineArguments.empty() ? shared_groups(set, family) : groups_of(set.baselineArguments, family) };
        set.options = structure_arguments(flatten(baseline), family);
        set.optionsDerived = true;
        for (auto& unit : set.units) {
            if (unit.options) continue;
            const std::vector<std::string> local { unit.localArguments.empty() ? flatten(difference(unit_groups(unit, family), baseline))
                                                                                : flatten(without_inputs(groups_of(unit.localArguments, family), unit, family)) };
            if (local.empty() && unit.localArguments.empty()) continue;
            SemanticOptions delta { structure_arguments(local, family) };
            // S1 11.1: a unit whose local-arguments differ carries a delta, even one that changes nothing S1 structures.
            if (unit.localArguments.empty() && empty_options(delta)) continue;
            unit.options = std::move(delta);
            unit.optionsDerived = true;
        }
    }
}

} // namespace mcppls::spec
