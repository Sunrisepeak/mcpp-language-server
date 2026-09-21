module mcppls.normalize.plan;

import std;
import nlohmann.json;
import mcppls.os;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.fs;
import mcppls.spec.database;
import mcppls.spec.kit;
import mcppls.spec.metadata;
import mcppls.toolchain.probe;
import mcppls.project.scan;
import mcppls.project.compdb;
import mcppls.normalize.gnu;
import mcppls.normalize.msvc;
import mcppls.normalize.semantic;

namespace mcppls::normalize {

namespace {

struct Candidate {
    const spec::Set* set { nullptr };
    const spec::TranslationUnit* unit { nullptr };
    std::string source;
    spec::Role role { spec::Role::unknown };
    std::string provided;
    std::string module;                      // the module the unit is part of
    std::vector<std::string> required;
    std::vector<std::string> arguments;      // engine arguments without argv[0] and without the source
    std::string driver;
    bool usesKit { false };
    bool c { false };                        // a C source
    const toolchain::ToolchainFacts* facts { nullptr };
};

// A unit the build compiles as C: its arguments say so (-x c, /TC), or its source is a .c file.
bool compiled_as_c(std::span<const std::string> arguments, std::string_view source) {
    std::optional<bool> stated;
    for (std::size_t i { 0 }; i < arguments.size(); ++i) {
        const std::string_view argument { arguments[i] };
        if (argument == "-x" && i + 1 < arguments.size()) stated = arguments[i + 1] == "c";
        else if (argument.starts_with("-x") && argument.size() > 2) stated = argument.substr(2) == "c";
        else if (argument == "/TC" || argument == "-TC") stated = true;
        else if (argument == "/TP" || argument == "-TP") stated = false;
    }
    return stated.value_or(base::extension(source) == ".c");
}

// The arguments without `-x c++-module`, for a unit that is not a module interface.
std::vector<std::string> without_module_mode(std::vector<std::string> arguments) {
    for (std::size_t k { 0 }; k + 1 < arguments.size();) {
        if (arguments[k] == "-x" && arguments[k + 1] == "c++-module") arguments.erase(arguments.begin() + static_cast<std::ptrdiff_t>(k), arguments.begin() + static_cast<std::ptrdiff_t>(k + 2));
        else ++k;
    }
    return arguments;
}

// How many leading directories two paths share.
std::size_t shared_directories(std::string_view left, std::string_view right) {
    const auto parts = [](std::string_view path) {
        std::vector<std::string_view> result;
        for (std::size_t start { 0 }; start <= path.size();) {
            const std::size_t slash { path.find('/', start) };
            const std::string_view part { path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start) };
            if (!part.empty()) result.push_back(part);
            if (slash == std::string_view::npos) break;
            start = slash + 1;
        }
        return result;
    };
    const auto a = parts(left);
    const auto b = parts(right);
    std::size_t shared { 0 };
    while (shared < a.size() && shared < b.size() && a[shared] == b[shared]) ++shared;
    return shared;
}

// The C driver beside a Clang toolchain's own driver: the driver's directory decides where Clang
// looks for the toolchain's headers, and its name whether a source is read as C.
std::string c_driver_beside(std::string_view driver) {
    const std::string_view name { base::file_name(driver) };
    const std::string_view suffix { name.ends_with(".exe") ? std::string_view { ".exe" } : std::string_view {} };
    return base::join_path(base::parent_path(driver), std::string { ENGINE_CLANG_C_DRIVER } + std::string { suffix });
}

std::vector<std::size_t> context_sets(const spec::Database& database, std::string_view contextSet) {
    std::vector<std::size_t> indices;
    if (contextSet.empty()) {
        for (std::size_t i { 0 }; i < database.sets.size(); ++i) indices.push_back(i);
        return indices;
    }
    const auto selected = spec::find_set(database, contextSet);
    if (!selected) return context_sets(database, {});
    indices.push_back(*selected);
    for (const auto& visible : database.sets[*selected].visibleSets) {
        if (auto index = spec::find_set(database, visible); index && std::ranges::find(indices, *index) == indices.end()) {
            indices.push_back(*index);
        }
    }
    return indices;
}

const toolchain::ToolchainFacts* facts_for(const PlanInput& input, const spec::Set& set) {
    if (input.facts == nullptr || set.toolchain.empty()) return nullptr;
    const auto it = input.facts->find(set.toolchain);
    return it == input.facts->end() ? nullptr : &it->second;
}

bool usable(const toolchain::ToolchainFacts* facts) {
    return facts != nullptr && !facts->appleClang && facts->toolchain.family != spec::Family::other;
}

bool is_std_module(std::string_view name) { return name == "std" || name == "std.compat"; }

// The name a prime unit is written under: stable, file-system safe and unique per module.
std::string prime_file_name(std::size_t index, std::string_view module) {
    std::string name { std::format("{:04}-", index) };
    for (const char c : module) name += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_') ? c : '-';
    return name + ".cpp";
}

} // namespace

EnginePlan plan_engine(const PlanInput& input) {
    EnginePlan plan;
    plan.contextSet = input.contextSet;
    if (input.database == nullptr) return plan;
    const spec::Database& database { *input.database };
    const std::string clangDriver { base::join_path(input.engineDriverDirectory, ENGINE_CLANG_DRIVER) };
    const std::string clangCDriver { base::join_path(input.engineDriverDirectory, ENGINE_CLANG_C_DRIVER) };

    // 0. The standard library's own module sources. A build that compiles them (CMake's
    //    import std writes std.ixx into the compile database) does not decide how the
    //    engine builds them: they are injected once from the manifest, step 5.
    std::set<std::string> standardSources;
    if (input.metadataReader) {
        for (const std::size_t setIndex : context_sets(database, input.contextSet)) {
            const toolchain::ToolchainFacts* facts { facts_for(input, database.sets[setIndex]) };
            if (facts == nullptr || !facts->toolchain.stdlib || facts->toolchain.stdlib->moduleMetadata.empty()) continue;
            for (const auto& entry : input.metadataReader(facts->toolchain.stdlib->moduleMetadata)) {
                if (is_std_module(entry.logicalName)) standardSources.insert(base::path_key(entry.source));
            }
        }
    }

    // 1. Candidates: every unit of the context, first occurrence of a source wins.
    std::vector<Candidate> candidates;
    std::set<std::string> seenSources;
    for (const std::size_t setIndex : context_sets(database, input.contextSet)) {
        const spec::Set& set { database.sets[setIndex] };
        const toolchain::ToolchainFacts* facts { facts_for(input, set) };
        for (const auto& unit : set.units) {
            Candidate candidate;
            candidate.set = &set;
            candidate.unit = &unit;
            candidate.source = spec::absolute_source(unit);
            if (standardSources.contains(base::path_key(candidate.source))) continue;
            if (!seenSources.insert(base::path_key(candidate.source)).second) continue;

            std::optional<project::ScanResult> scanned;
            auto scan = [&]() -> const project::ScanResult& {
                if (!scanned) scanned = input.scanner ? input.scanner(candidate.source) : project::ScanResult {};
                return *scanned;
            };
            candidate.role = unit.role.value_or(spec::Role::unknown);
            if (candidate.role == spec::Role::unknown) candidate.role = project::role_of(scan());
            if (!unit.providedModules.empty()) {
                candidate.provided = unit.providedModules.front().first;
            } else {
                candidate.provided = project::provided_name(scan());
            }
            candidate.required = unit.requiredModules;
            if (candidate.required.empty()) candidate.required = project::required_names(scan());
            if (!candidate.provided.empty()) {
                candidate.module = candidate.provided.substr(0, candidate.provided.find(':'));
            } else if (candidate.role == spec::Role::module_implementation && scan().declaration) {
                candidate.module = scan().declaration->module;
            }

            const bool importable { spec::is_importable(candidate.role) };
            const auto syntax { mcppls::os::FAMILY == mcppls::os::Family::windows ? project::CommandSyntax::windows
                                                                                  : project::CommandSyntax::posix };
            // S1 section 9 rule 1: options, when the producer stated them, decide the unit's semantics; they
            // are written in the toolchain's dialect and take the translation a build's arguments take.
            // Options the S1 library derived (spec::complete_options) only restate the arguments.
            const auto effective = effective_options(set.optionsDerived ? std::nullopt : set.options,
                                                     unit.optionsDerived ? std::nullopt : unit.options);
            const auto arguments = effective
                ? options_arguments(*effective, facts != nullptr ? facts->toolchain.family : spec::Family::clang,
                                    facts != nullptr ? facts->toolchain.driver : std::string_view { "clang++" }, candidate.source)
                : project::expand_response_files(unit.arguments, unit.workDirectory, syntax);
            candidate.facts = facts;
            candidate.c = compiled_as_c(arguments, candidate.source);
            // robustness design C5: when the engine could not build the toolchain's standard library, C++ units are
            // read with the semantic kit; C units, which import nothing, keep their toolchain.
            const bool toolchain { usable(facts) && !(input.preferKit && input.kit != nullptr && !candidate.c) };
            if (toolchain && (facts->toolchain.family == spec::Family::gcc || facts->toolchain.family == spec::Family::clang)) {
                candidate.arguments = translate_gnu(GnuInput { arguments, candidate.source, unit.workDirectory, facts, importable, input.noAlignedAllocationWithMsvcStl });
                if (facts->toolchain.family == spec::Family::gcc) candidate.driver = candidate.c ? clangCDriver : clangDriver;
                else candidate.driver = candidate.c ? c_driver_beside(facts->toolchain.driver) : facts->toolchain.driver;
            } else if (toolchain) {
                candidate.arguments = translate_msvc(MsvcInput { arguments, candidate.source, unit.workDirectory, facts, importable, input.noAlignedAllocationWithMsvcStl });
                candidate.driver = candidate.c ? clangCDriver : clangDriver;
            } else if (input.kit != nullptr) {
                candidate.usesKit = true;
                candidate.arguments = kit_arguments(*input.kit, language_standard_of(arguments), input.macosSdk);
                for (auto& argument : semantic_subset(arguments)) {
                    if (!argument.starts_with("-std=")) candidate.arguments.push_back(std::move(argument));
                }
                if (importable) {
                    candidate.arguments.emplace_back("-x");
                    candidate.arguments.emplace_back("c++-module");
                }
                candidate.driver = candidate.c ? clangCDriver : clangDriver;
            } else {
                plan.issues.push_back(PlanIssue { "toolchain-not-found",
                    std::format("no usable compiler or semantic kit for {}", base::file_name(candidate.source)), candidate.source, {} });
                continue;
            }
            candidates.push_back(std::move(candidate));
        }
    }

    // 1b. Files the editor has open that no set describes (a target of a feature not built, a scratch file). clangd
    //     guesses their commands and does not know their imports; one importing a module nothing provides made clang spin
    //     on a core for as long as clangd ran (xlings' apps/gui/main.cpp, robustness design C2). They take the arguments of
    //     the nearest C++ unit, and their imports resolve, or get stand-ins, like any unit's.
    for (const auto& openSource : input.openSources) {
        const std::string source { base::normalize_path(openSource) };
        const std::string key { base::path_key(source) };
        if (standardSources.contains(key) || seenSources.contains(key)) continue;
        std::optional<std::size_t> nearest;
        std::size_t best { 0 };
        const std::string directory { base::parent_path(base::path_key(source)) };
        for (std::size_t i { 0 }; i < candidates.size(); ++i) {
            if (candidates[i].c) continue;
            const std::size_t shared { shared_directories(base::parent_path(base::path_key(candidates[i].source)), directory) };
            if (!nearest || shared > best) {
                nearest = i;
                best = shared;
            }
        }
        if (!nearest) continue;
        seenSources.insert(key);
        const project::ScanResult scanned { input.scanner ? input.scanner(source) : project::ScanResult {} };
        Candidate candidate { candidates[*nearest] };
        candidate.source = source;
        candidate.role = project::role_of(scanned);
        candidate.provided = project::provided_name(scanned);
        candidate.required = project::required_names(scanned);
        candidate.module = scanned.declaration ? scanned.declaration->module : std::string {};
        candidate.arguments = without_module_mode(std::move(candidate.arguments));
        if (spec::is_importable(candidate.role)) {
            candidate.arguments.emplace_back("-x");
            candidate.arguments.emplace_back("c++-module");
        }
        candidates.push_back(std::move(candidate));
        plan.openSources.push_back(source);
    }

    // 2. Providers in the context, and the standard library manifest the context uses.
    std::map<std::string, std::vector<std::size_t>, std::less<>> providers;
    for (std::size_t i { 0 }; i < candidates.size(); ++i) {
        if (!candidates[i].provided.empty() && spec::is_importable(candidates[i].role)) providers[candidates[i].provided].push_back(i);
    }
    // The unit whose arguments the standard library's units take, and so the manifest they come from: a
    // C++ unit that imports the standard library (the arguments its BMI must agree with), one that is not a
    // module interface before one that is, the first of the best. Never a C unit: in a workspace whose first
    // set is a C library, std was built as C (-std=c11) and every unit importing std was then left out.
    auto manifest_of = [&](const Candidate& candidate) -> std::string {
        if (candidate.usesKit) return input.kit->moduleMetadata;
        return candidate.facts && candidate.facts->toolchain.stdlib ? candidate.facts->toolchain.stdlib->moduleMetadata : std::string {};
    };
    std::optional<std::size_t> templateIndex;
    int templateRank { 0 };
    for (std::size_t i { 0 }; i < candidates.size(); ++i) {
        const auto& candidate = candidates[i];
        if (candidate.c || manifest_of(candidate).empty()) continue;
        const int rank { 1 + (spec::is_importable(candidate.role) ? 0 : 1) + (std::ranges::any_of(candidate.required, is_std_module) ? 2 : 0) };
        if (rank > templateRank) {
            templateRank = rank;
            templateIndex = i;
            if (rank == 4) break;
        }
    }
    const std::string stdlibManifest { templateIndex ? manifest_of(candidates[*templateIndex]) : std::string {} };
    std::vector<spec::ModuleEntry> stdEntries;
    if (!stdlibManifest.empty() && input.metadataReader) stdEntries = input.metadataReader(stdlibManifest);
    auto std_provides = [&](std::string_view name) {
        return std::ranges::any_of(stdEntries, [&](const spec::ModuleEntry& entry) { return entry.logicalName == name; });
    };

    // 3. Admission (robustness design C2). clangd 23.1 deadlocks when it has to build a module whose
    //    imports cannot all be resolved, and the deadlock reaches other files (experiments S2, S6, S12).
    //    Nothing else stops it answering: a unit's own unresolved import (S10, S11), an import of a module
    //    that does not compile (S3), a module with two providers (S7). With a stand-in directory, a module
    //    nothing usable provides gets an empty unit (step 5b), every import resolves and no unit leaves;
    //    without one, the providers that cannot be built leave and every other unit stays.
    const bool standIns { !input.stubDirectory.empty() };
    std::set<std::string, std::less<>> stubbed;              // modules that get a stand-in
    std::set<std::string, std::less<>> unusableProviders;    // modules whose planned providers clangd cannot find
    std::vector<bool> excluded(candidates.size(), false);
    for (auto& [name, indices] : providers) {
        if (indices.size() < 2) continue;
        for (std::size_t k { 1 }; k < indices.size(); ++k) excluded[indices[k]] = true;
        plan.issues.push_back(PlanIssue { "ambiguous-module", std::format("module {} has {} providers; using {}", name, indices.size(),
            base::file_name(candidates[indices.front()].source)), candidates[indices[1]].source, name });
    }
    // usable plan W5.4: a semantic kit that requires the macOS SDK cannot build std without
    // one. Rather than send such a unit to clangd and wait out a request that never answers
    // (experiment E15), it leaves the engine database now, the same way a module clangd
    // reported it could not build does (step 3 above reuses that mechanism too).
    const bool sdkBlocksStd { input.kit != nullptr && spec::requires_macos_sdk(*input.kit) && input.macosSdk.empty() };
    bool anyStd { false };
    std::set<std::string> reported;
    for (std::size_t i { 0 }; i < candidates.size(); ++i) {
        for (const auto& name : candidates[i].required) {
            if (is_std_module(name)) anyStd = true;
            if (sdkBlocksStd && candidates[i].usesKit && is_std_module(name)) {
                excluded[i] = true;
                if (reported.insert("sdk-missing\n" + name).second) {
                    plan.issues.push_back(PlanIssue { "sdk-missing",
                        "the macOS SDK was not found; files that import the standard library cannot be built", candidates[i].source, name });
                }
                continue;
            }
            const bool provider { spec::is_importable(candidates[i].role) };
            if (const auto failed = input.unresolvedModules.find(name); failed != input.unresolvedModules.end()) {
                if (standIns) {
                    stubbed.insert(name);
                    unusableProviders.insert(name);
                } else if (provider) {
                    excluded[i] = true;
                }
                if (reported.insert(candidates[i].source + "\n" + name).second) {
                    plan.issues.push_back(PlanIssue { "module-build-failed", std::format("module {} could not be built: {}", name, failed->second),
                                                      candidates[i].source, name });
                }
                continue;
            }
            if (providers.contains(name) || std_provides(name)) continue;
            const auto setIndex = spec::find_set(database, candidates[i].set->name);
            bool resolved { false };
            if (setIndex && input.metadataReader) {
                const auto resolution = spec::resolve_module(database, *setIndex, name, input.metadataReader);
                resolved = resolution.from == spec::ResolvedFrom::module_metadata;
            }
            if (resolved) continue;
            // A provider with an import that cannot resolve cannot be built, and building it is what deadlocks.
            if (standIns) stubbed.insert(name);
            else if (input.excludeUnresolvedImports && provider) excluded[i] = true;
            if (reported.insert(candidates[i].source + "\n" + name).second) {
                plan.issues.push_back(PlanIssue { "unresolved-module", std::format("module {} cannot be resolved", name), candidates[i].source, name });
            }
        }
    }
    // The units the plan has for a module clangd cannot find are no use to it: they leave, and the stand-in
    // takes their place.
    for (const auto& name : unusableProviders) {
        if (const auto it = providers.find(name); it != providers.end()) {
            for (const std::size_t index : it->second) excluded[index] = true;
        }
    }
    auto required_by_a_unit = [&](std::string_view name) {
        for (std::size_t i { 0 }; i < candidates.size(); ++i) {
            if (!excluded[i] && std::ranges::find(candidates[i].required, name) != candidates[i].required.end()) return true;
        }
        return false;
    };
    // A module whose every provider left gets a stand-in too, when something imports it. Without stand-ins,
    // a provider importing only left-out providers cannot be built either; a unit that provides nothing is
    // never built by another unit, so it stays.
    if (standIns) {
        for (const auto& [name, indices] : providers) {
            if (std::ranges::all_of(indices, [&](std::size_t index) { return excluded[index]; }) && required_by_a_unit(name)) stubbed.insert(name);
        }
    }
    for (bool changed { !standIns }; changed;) {
        changed = false;
        for (std::size_t i { 0 }; i < candidates.size(); ++i) {
            if (excluded[i] || !spec::is_importable(candidates[i].role)) continue;
            for (const auto& name : candidates[i].required) {
                const auto it = providers.find(name);
                if (it == providers.end()) continue;
                if (std::ranges::all_of(it->second, [&](std::size_t provider) { return excluded[provider]; })) {
                    excluded[i] = true;
                    changed = true;
                    break;
                }
            }
        }
    }

    // 4. Entries.
    for (std::size_t i { 0 }; i < candidates.size(); ++i) {
        const auto& candidate = candidates[i];
        if (excluded[i]) {
            plan.excludedFiles.push_back(candidate.source);
            continue;
        }
        EngineEntry entry { candidate.unit->workDirectory, candidate.source, {} };
        entry.arguments.push_back(candidate.driver);
        entry.arguments.insert(entry.arguments.end(), candidate.arguments.begin(), candidate.arguments.end());
        entry.arguments.push_back(candidate.source);
        if (spec::is_importable(candidate.role)) entry.provides = candidate.provided;
        entry.module = candidate.module;
        entry.imports = candidate.required;
        plan.entries.push_back(std::move(entry));
    }

    // An importer's arguments for a module: a unit that imports it, else the provider's own without module mode.
    auto importer_of = [&](std::string_view module) -> std::optional<std::size_t> {
        std::optional<std::size_t> fallback;
        for (std::size_t i { 0 }; i < candidates.size(); ++i) {
            if (excluded[i] || std::ranges::find(candidates[i].required, module) == candidates[i].required.end()) continue;
            if (!spec::is_importable(candidates[i].role)) return i;
            if (!fallback) fallback = i;
        }
        return fallback;
    };
    auto add_module = [&](const std::string& name, std::vector<std::string> requires_, std::optional<std::size_t> importer,
                          const std::string& workDirectory) {
        PlannedModule module { name, std::move(requires_), {} };
        if (!input.primeDirectory.empty() && name.find(':') == std::string::npos && importer) {
            const auto& unit = candidates[*importer];
            module.primeFile = base::join_path(input.primeDirectory, prime_file_name(plan.modules.size(), name));
            EngineEntry entry { workDirectory, module.primeFile, {} };
            entry.arguments.push_back(unit.driver);
            for (auto& argument : without_module_mode(unit.arguments)) entry.arguments.push_back(std::move(argument));
            entry.arguments.push_back(module.primeFile);
            entry.imports.push_back(name);
            plan.entries.push_back(std::move(entry));
            plan.primeSources.emplace_back(module.primeFile, std::format("import {};\n", name));
        }
        plan.modules.push_back(std::move(module));
    };
    for (std::size_t i { 0 }; i < candidates.size(); ++i) {
        const auto& candidate = candidates[i];
        if (excluded[i] || candidate.provided.empty() || !spec::is_importable(candidate.role)) continue;
        const auto owners = providers.find(candidate.provided);
        if (owners == providers.end() || owners->second.front() != i) continue;   // the provider in use
        auto importer = importer_of(candidate.provided);
        if (!importer) importer = i;
        add_module(candidate.provided, candidate.required, importer, candidate.unit->workDirectory);
    }

    // 5. Standard library units, once for the context, with the arguments of a representative unit.
    //    Skipped when the SDK that a kit's std needs is missing (above): the units are excluded, so
    //    nothing imports std successfully, and clangd would otherwise still try to background-index it.
    if (anyStd && templateIndex && !stdEntries.empty() && !(sdkBlocksStd && candidates[*templateIndex].usesKit)) {
        const auto& representative = candidates[*templateIndex];
        std::vector<std::string> base { representative.arguments };
        for (std::size_t k { 0 }; k + 1 < base.size();) {
            if (base[k] == "-x" && base[k + 1] == "c++-module") base.erase(base.begin() + static_cast<std::ptrdiff_t>(k), base.begin() + static_cast<std::ptrdiff_t>(k + 2));
            else ++k;
        }
        const bool msvcStl { representative.facts != nullptr && representative.facts->toolchain.stdlib
                             && representative.facts->toolchain.stdlib->name == "msvc-stl" };
        for (const auto& module : stdEntries) {
            if (seenSources.contains(base::path_key(module.source))) continue;
            if (stubbed.contains(module.logicalName)) continue;   // clangd cannot find it as planned; a stand-in replaces it
            // A producer that describes where std comes from (a dependency package's std.cppm) is taken at its word.
            if (providers.contains(module.logicalName)) continue;
            EngineEntry entry { representative.unit->workDirectory, module.source, {} };
            entry.arguments.push_back(representative.driver);
            entry.arguments.insert(entry.arguments.end(), base.begin(), base.end());
            for (const auto& directory : module.systemIncludeDirectories) {
                entry.arguments.emplace_back("-isystem");
                entry.arguments.push_back(directory);
            }
            entry.arguments.emplace_back("-Wno-reserved-module-identifier");
            // The MSVC STL includes its headers inside the module purview (usable plan E7).
            if (msvcStl) entry.arguments.emplace_back("-Wno-include-angled-in-module-purview");
            entry.arguments.emplace_back("-x");
            entry.arguments.emplace_back("c++-module");
            entry.arguments.push_back(module.source);
            std::vector<std::string> requires_;
            if (module.logicalName == "std.compat") requires_.emplace_back("std");
            entry.provides = module.logicalName;
            entry.imports = requires_;
            plan.entries.push_back(std::move(entry));
            ++plan.stdUnits;
            add_module(module.logicalName, std::move(requires_), templateIndex, representative.unit->workDirectory);
        }
    } else if (anyStd && stdEntries.empty()) {
        // Units of the context may provide std themselves (a package's std.cppm).
        auto unprovided_std = [&](const std::string& name) { return is_std_module(name) && !providers.contains(name); };
        for (const auto& candidate : candidates) {
            if (std::ranges::any_of(candidate.required, unprovided_std)) {
                plan.issues.push_back(PlanIssue { "unresolved-module", "the standard library module manifest was not found", candidate.source, "std" });
                break;
            }
        }
    }

    // 5b. Stand-ins (robustness design C2): an empty unit for each module nothing usable provides, built with the
    //     arguments of a unit that imports it. Only the names that module would have declared are missing.
    for (const auto& name : stubbed) {
        const auto importer = importer_of(name);
        if (!importer) continue;
        const auto& unit = candidates[*importer];
        std::string fileName { std::format("{:04}-", plan.stubModules.size()) };
        for (const char c : name) fileName += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_') ? c : '-';
        const std::string file { base::join_path(input.stubDirectory, fileName + ".cppm") };
        EngineEntry entry { unit.unit->workDirectory, file, {} };
        entry.arguments.push_back(unit.driver);
        for (auto& argument : without_module_mode(unit.arguments)) entry.arguments.push_back(std::move(argument));
        entry.arguments.emplace_back("-x");
        entry.arguments.emplace_back("c++-module");
        entry.arguments.push_back(file);
        entry.provides = name;
        plan.entries.push_back(std::move(entry));
        plan.stubSources.emplace_back(file, std::format("export module {};\n", name));
        plan.stubModules.push_back(name);
        plan.modules.push_back(PlannedModule { name, {}, {} });
    }

    // 6. Module hints (usable plan W7). To find the unit that provides a module, clangd 23.1
    //    scans every file of the database, one after another, each time it prepares a file whose
    //    imports it has not looked up yet: seconds for a few hundred files, in every worker that
    //    starts at once. A producer's -fmodule-output=<path> and an importer's
    //    -fmodule-file=<name>=<path> name the unit instead, and clangd scans only that file to
    //    confirm it (ProjectModules.cpp, CompileCommandsProjectModules). It resolves every module
    //    a file reaches through that file's own command, so an entry names all of them. The
    //    paths are never written: clangd builds into its own cache.
    if (!input.moduleHintDirectory.empty()) {
        std::map<std::string_view, const std::vector<std::string>*, std::less<>> graph;
        for (const auto& entry : plan.entries) {
            if (!entry.provides.empty()) graph.emplace(entry.provides, &entry.imports);
        }
        auto hint_path = [&](std::string_view module) {
            std::string name { module };
            std::ranges::replace(name, ':', '-');   // a partition; ':' appears in no other module name
            return base::join_path(input.moduleHintDirectory, name + ".pcm");
        };
        for (auto& entry : plan.entries) {
            std::set<std::string_view, std::less<>> reached;
            std::vector<std::string_view> pending { entry.imports.begin(), entry.imports.end() };
            while (!pending.empty()) {
                const std::string_view name { pending.back() };
                pending.pop_back();
                const auto provider = graph.find(name);
                if (provider == graph.end() || !reached.insert(name).second) continue;
                for (const auto& required : *provider->second) pending.emplace_back(required);
            }
            for (const std::string_view name : reached) entry.moduleHints.push_back(std::format("-fmodule-file={}={}", name, hint_path(name)));
            if (!entry.provides.empty()) entry.moduleHints.push_back("-fmodule-output=" + hint_path(entry.provides));
        }
    }
    return plan;
}

nlohmann::json to_compile_commands(const EnginePlan& plan, bool moduleHints) {
    nlohmann::json entries = nlohmann::json::array();
    for (const auto& entry : plan.entries) {
        std::vector<std::string> arguments { entry.arguments };
        if (moduleHints && !entry.moduleHints.empty() && !arguments.empty()) {
            arguments.insert(arguments.end() - 1, entry.moduleHints.begin(), entry.moduleHints.end());
        }
        entries.push_back(nlohmann::json { { "directory", entry.directory }, { "file", entry.file }, { "arguments", std::move(arguments) } });
    }
    return entries;
}

base::Result<void> write_engine_database(std::string_view directory, const EnginePlan& plan) {
    if (auto created = platform::fs::create_directories(directory); !created) return created;
    return platform::fs::write_file_atomic(base::join_path(directory, "compile_commands.json"), to_compile_commands(plan).dump(1));
}

} // namespace mcppls::normalize
