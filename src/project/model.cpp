module mcppls.project.model;

import std;
import mcppls.os;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.base.log;
import mcppls.platform.fs;
import mcppls.platform.dirs;
import mcppls.spec.database;
import mcppls.spec.kit;
import mcppls.spec.options;
import mcppls.toolchain.probe;
import mcppls.toolchain.discover;
import mcppls.project.detect;
import mcppls.project.compdb;
import mcppls.project.infer;
import mcppls.project.provider;
import mcppls.project.mcpp;
import mcppls.project.cmake;
import mcppls.project.generated;

namespace mcppls::project {

namespace {

std::string_view short_family(spec::Family family) {
    switch (family) {
    case spec::Family::gcc: return "gcc";
    case spec::Family::clang: return "clang";
    case spec::Family::msvc: return "msvc";
    case spec::Family::clang_cl: return "clang-cl";
    case spec::Family::other: return "compiler";
    }
    return "compiler";
}

bool usable_for_semantics(const toolchain::ToolchainFacts& facts) {
    if (facts.appleClang) return false;
    const bool msvc { facts.toolchain.family == spec::Family::msvc || facts.toolchain.family == spec::Family::clang_cl };
    if (facts.toolchain.family != spec::Family::gcc && facts.toolchain.family != spec::Family::clang && !msvc) return false;
    // The MSVC STL's semantics need the toolset and SDK paths the engine is given explicitly.
    if (msvc && !facts.msvc) return false;
    return facts.toolchain.stdlib && !facts.toolchain.stdlib->moduleMetadata.empty()
        && platform::fs::is_regular_file(facts.toolchain.stdlib->moduleMetadata);
}

} // namespace

std::optional<ModelIssue> visual_studio_notice(const toolchain::ToolchainFacts& facts) {
    if (!facts.msvc || usable_for_semantics(facts)) return std::nullopt;
    return ModelIssue { "msvc-without-std-module",
                        std::format("Visual Studio's MSVC {} has no std module (MSVC 14.38 and newer have one), so its semantics are not used",
                                    facts.msvc->toolsVersion) };
}

namespace {

void set_profile(ProjectModel& model, const spec::Kit* kit) {
    for (const auto& set : model.database.sets) {
        const auto it = model.facts.find(set.toolchain);
        if (it == model.facts.end() || it->second.appleClang) continue;
        const auto& toolchain = it->second.toolchain;
        model.profile.kind = "build-toolchain";
        model.profile.compiler = std::format("{} {}", short_family(toolchain.family), toolchain.version);
        model.profile.stdlib = toolchain.stdlib ? std::format("{} {}", toolchain.stdlib->name, toolchain.stdlib->version) : std::string {};
        if (model.profile.stdlib.ends_with(' ')) model.profile.stdlib.pop_back();
        model.profile.target = toolchain.target;
        return;
    }
    if (kit != nullptr) {
        model.usesKit = true;
        model.profile.kind = "semantic-kit";
        model.profile.compiler.clear();
        model.profile.stdlib = std::format("{} {}", kit->stdlibName, kit->stdlibVersion);
        model.profile.target = kit->target;
    } else {
        model.profile.kind = "semantic-kit";
        model.profile.stdlib = "none";
        model.issues.push_back(ModelIssue { "toolchain-not-found", "no usable compiler and no semantic kit" });
    }
}

} // namespace

std::string workspace_key(std::string_view root) {
    // FNV-1a over the normalized, case-folded path, plus the last component for humans.
    const std::string key { base::path_key(base::normalize_path(root)) };
    std::uint64_t hash { 1469598103934665603ull };
    for (const unsigned char c : key) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    std::string name;
    for (const char c : base::file_name(base::normalize_path(root))) {
        name += (base::is_identifier_char(c) || c == '-' || c == '.') ? c : '_';
    }
    return std::format("{}-{:016x}", name.empty() ? std::string { "root" } : name, hash);
}

std::vector<ModuleManifest> module_manifests(const ProjectModel& model, const spec::Kit* kit) {
    std::vector<ModuleManifest> manifests;
    auto add = [&](const std::string& path, std::string_view origin) {
        if (path.empty()) return;
        if (std::ranges::any_of(manifests, [&](const ModuleManifest& manifest) { return base::same_path(manifest.path, path); })) return;
        manifests.push_back(ModuleManifest { path, std::string { origin } });
    };
    for (const auto& [id, facts] : model.facts) {
        if (facts.toolchain.stdlib) add(facts.toolchain.stdlib->moduleMetadata, "stdlib");
    }
    if (model.usesKit && kit != nullptr) add(kit->moduleMetadata, "stdlib");
    for (const auto& set : model.database.sets) {
        for (const auto& manifest : set.moduleMetadata) add(manifest, "module-metadata");
    }
    return manifests;
}

ProjectModel load_project(std::string_view rootInput, const LoadOptions& options) {
    ProjectModel model;
    model.root = platform::fs::canonical_path(rootInput);
    const Detection detection { detect_project(model.root, options.configuredDatabase) };
    model.detected = detection.kind;
    const Scanner scanner { options.scanner ? options.scanner : file_scanner() };
    const Prober prober = [&](std::string_view driver, std::span<const std::string> relevant) -> std::optional<toolchain::ToolchainFacts> {
        if (!options.trusted || !options.runner) return std::nullopt;
        auto facts = toolchain::probe_cached(driver, relevant, options.runner, options.probeCache);
        if (!facts) {
            base::log::info("probe of {} failed: {}", driver, facts.error().message);
            return std::nullopt;
        }
        return *facts;
    };
    ProviderContext context { options.trusted, options.runner, scanner, prober };
    context.mcppExecutable = options.mcppExecutable;
    context.offline = options.offline;
    context.runBuildTool = options.runBuildTool;
    context.producerHard = options.producerHard;
    context.producerSoft = options.producerSoft;
    context.environmentWait = options.environmentWait;
    context.rootKey = options.rootKey;
    context.onSlow = options.onSlow;

    std::optional<InferredDatabase> loaded;
    auto accept = [&](base::Result<InferredDatabase> result, SourceKind kind) {
        if (result) {
            loaded = std::move(*result);
            model.source = kind;
            for (const auto& problem : loaded->problems) model.issues.push_back(ModelIssue { "toolchain-not-found", problem });
            for (const auto& [code, message] : loaded->notices) model.notices.push_back(ModelIssue { code, message });
            for (const auto& [code, message] : loaded->issues) model.issues.push_back(ModelIssue { code, message });
        } else {
            model.issues.push_back(ModelIssue { result.error().code, result.error().message });
        }
    };

    // design 2.1: an untrusted workspace is L4 by definition. No build tool and no compiler runs
    // (that guard is in the branches below and in the Prober), and, just as importantly, nothing a
    // *trusted* session once wrote or a build once produced is read either: a leftover
    // compile_commands.json or build database from before the workspace lost trust, or from a build
    // run outside mcppls entirely, is still a fact about the build, and reading it here would make
    // "untrusted" mean "untrusted, unless something is already sitting on disk" -- including an
    // explicit `mcppls.database` setting, which an untrusted workspace's own `.vscode/settings.json`
    // could otherwise use to point mcppls at a file of its choosing. Detection itself (stat calls for
    // well-known file names, no file content read) is not a build tool run and stays cheap either
    // way, so `model.detected` still says what kind of project this looks like. That fact is worth
    // an issue of its own: `mcppls check --untrusted` and other direct callers of load_project never
    // see workspace.cpp's own "untrusted-workspace" (it is added once, workspace-wide, by the
    // orchestrator), so this is the only place that says why a project came back inferred when the
    // workspace is untrusted.
    if (!options.trusted && detection.kind != SourceKind::inferred) {
        model.issues.push_back(ModelIssue { "untrusted-workspace",
            std::format("the workspace is not trusted, so the {} project's own data was not read; sources are scanned instead",
                        to_string(detection.kind)) });
    }
    switch (options.trusted ? detection.kind : SourceKind::inferred) {
    case SourceKind::build_database: {
        auto database = spec::load_database(detection.buildDatabase);
        if (database) {
            loaded = enrich_database(std::move(*database), scanner, prober);
            model.source = SourceKind::build_database;
        } else {
            model.issues.push_back(ModelIssue { database.error().code, database.error().message });
        }
        model.watch = { detection.buildDatabase };
        break;
    }
    case SourceKind::mcpp:
        accept(load_mcpp(detection, context), SourceKind::mcpp);
        model.producer = context.producerUsed;
        model.producerVersion = context.producerVersionUsed;
        model.watch = { "mcpp.toml", "mcpp.lock" };
        if (loaded) model.watch.insert(model.watch.end(), loaded->watch.begin(), loaded->watch.end());
        break;
    case SourceKind::cmake: {
        const std::string privateBuild { base::join_path(options.cacheDirectory, "cmake") };
        accept(load_cmake(detection, privateBuild, context), SourceKind::cmake);
        model.producer = context.producerUsed;
        model.producerVersion = context.producerVersionUsed;
        model.watch = { "**/CMakeLists.txt", "CMakePresets.json", "**/*.cmake" };
        if (!detection.compileCommands.empty()) model.watch.push_back(detection.compileCommands);
        break;
    }
    case SourceKind::compile_commands: {
        auto commands = read_compile_commands(detection.compileCommands);
        if (commands) {
            loaded = database_from_commands(*commands, base::file_name(model.root), scanner, prober);
            model.source = SourceKind::compile_commands;
            for (const auto& problem : loaded->problems) model.issues.push_back(ModelIssue { "toolchain-not-found", problem });
        } else {
            model.issues.push_back(ModelIssue { commands.error().code, commands.error().message });
        }
        model.watch = { detection.compileCommands };
        break;
    }
    case SourceKind::inferred: break;
    }

    // A database whose every unit names a file that no longer exists (real-project plan RP2.4: a
    // `target/` a build once wrote and the project later deleted, a checked-in
    // compile_commands.json from a machine that is not this one) is no better than none: it is
    // treated the same as an empty one, and inference takes over. A database that is only partly
    // stale keeps its remaining units below, once loaded, rather than being discarded here.
    const auto has_existing_unit = [](const spec::Set& set) {
        return std::ranges::any_of(set.units, [](const spec::TranslationUnit& unit) { return platform::fs::is_regular_file(spec::absolute_source(unit)); });
    };
    if (!loaded || loaded->database.sets.empty() || std::ranges::all_of(loaded->database.sets, [](const spec::Set& set) { return set.units.empty(); })
        || std::ranges::none_of(loaded->database.sets, has_existing_unit)) {
        InferOptions infer;
        if (options.trusted && options.runner) {
            if (!options.compilerOverride.empty() && options.compilerOverride != "kit") {
                if (auto facts = prober(options.compilerOverride, std::vector<std::string> {}); facts && usable_for_semantics(*facts)) {
                    infer.facts = std::move(*facts);
                } else {
                    model.issues.push_back(ModelIssue { "toolchain-not-found", std::format("the configured compiler {} cannot provide module semantics", options.compilerOverride) });
                }
            }
            const bool kitRequested { options.compilerOverride == "kit" };
            if (!infer.facts && options.discoverCompilers && !kitRequested) {
                const auto candidates = toolchain::discover_compilers(options.runner);
                // On Windows the machine's own Visual Studio comes first when it has the std module (design 9.3, D27).
                if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) {
                    for (const auto& candidate : candidates) {
                        if (candidate.origin != "visual-studio" || candidate.family != spec::Family::msvc) continue;
                        auto facts = prober(candidate.driver, std::vector<std::string> {});
                        if (facts && usable_for_semantics(*facts)) {
                            infer.facts = std::move(*facts);
                            break;
                        }
                        if (auto notice = facts ? visual_studio_notice(*facts) : std::nullopt) model.notices.push_back(std::move(*notice));
                    }
                }
                for (const auto& candidate : candidates) {
                    if (infer.facts) break;
                    if (candidate.family != spec::Family::gcc && candidate.family != spec::Family::clang) continue;
                    if (auto facts = prober(candidate.driver, std::vector<std::string> {}); facts && usable_for_semantics(*facts)) {
                        infer.facts = std::move(*facts);
                    }
                }
            }
        }
        infer.languageStandard = inferred_language_standard(infer.facts);
        loaded = infer_database(model.root, infer, scanner);
        if (model.source != SourceKind::inferred && detection.kind != SourceKind::inferred) {
            model.issues.push_back(ModelIssue { "model-fallback", std::format("{} data was not available; sources are scanned instead", to_string(detection.kind)) });
        }
        model.source = SourceKind::inferred;
        model.watch.insert(model.watch.end(), { "**/*.cppm", "**/*.ccm", "**/*.cxxm", "**/*.ixx", "**/*.mpp", "**/*.cpp", "**/*.cc", "**/*.cxx" });
    }

    model.database = std::move(loaded->database);
    model.facts = std::move(loaded->facts);
    // A database a producer wrote is level 2 at least (enrichment saw to that); the S1 library structures
    // its arguments into options for level 3, which mcpp leaves to it (mcpp-community/mcpp#636). Before
    // the renaming below, while each unit's source is still spelled as its arguments spell it.
    const bool producedDatabase { model.source == SourceKind::build_database
                                  || model.database.generator.value_or(spec::Generator {}).name == "mcpp" };
    if (producedDatabase) spec::complete_options(model.database);
    // One name for each file. A producer may reach a file through a symbolic
    // link the workspace does not (mcpp resolves /var to /private/var on macOS),
    // and everything after this compares files by name: open documents,
    // exclusions, the module index, and clangd matching an unsaved buffer to
    // the module source it builds.
    for (auto& set : model.database.sets) {
        for (auto& unit : set.units) unit.source = platform::fs::canonical_path(spec::absolute_source(unit));
    }
    // Stale database detection (real-project plan RP2.4): a unit whose file does not exist any more is used
    // for nothing (the caller cannot open, scan or build it), so it is dropped rather than left to
    // fail every later step in a different way each time; the status says once that the database was
    // stale rather than the model silently losing units. A fully stale database was already turned
    // into inference above; this is the partial case.
    if (model.source != SourceKind::inferred) {
        std::size_t staleUnits { 0 };
        for (auto& set : model.database.sets) {
            const auto stale = std::ranges::remove_if(set.units, [&](const spec::TranslationUnit& unit) { return !platform::fs::is_regular_file(unit.source); });
            staleUnits += static_cast<std::size_t>(std::ranges::distance(stale));
            set.units.erase(stale.begin(), stale.end());
        }
        if (staleUnits > 0) {
            model.notices.push_back(ModelIssue { "stale-database",
                std::format("{} {} database {} named {} that no longer exist; the rest of the model is used",
                            staleUnits, to_string(model.source), staleUnits == 1 ? "entry" : "entries", staleUnits == 1 ? "a file" : "files") });
        }
    }
    // Generated-source recovery (real-project plan RP2.3): a module a unit imports but nothing in the database
    // provides may still be a real file on disk, at one of the two places a build leaves what it
    // generated -- the project's own build directory, or mcpp's build-database cache, which survives
    // a `target/` the project later deleted (the incident this exists for). Tried before a stand-in
    // is ever considered, so an importer gets the real declarations when they can still be found; a
    // module nothing usable provides even after this stays for the engine plan's own stand-in.
    if (model.source != SourceKind::inferred) {
        std::set<std::string, std::less<>> provided;
        for (const auto& set : model.database.sets) {
            for (const auto& unit : set.units) {
                for (const auto& [name, bmi] : unit.providedModules) provided.insert(name);
            }
        }
        std::set<std::string, std::less<>> missing;
        for (const auto& set : model.database.sets) {
            for (const auto& unit : set.units) {
                for (const auto& name : unit.requiredModules) {
                    if (name != "std" && name != "std.compat" && !provided.contains(name)) missing.insert(name);
                }
            }
        }
        if (!missing.empty()) {
            const GeneratedSourceOptions recoveryOptions {
                model.root, options.homeDirectory.empty() ? platform::dirs::home_directory() : options.homeDirectory, scanner
            };
            for (const auto& name : missing) {
                auto recovered = find_generated_source(name, recoveryOptions);
                if (!recovered) continue;
                // The arguments of an existing unit in the set that needed this module: the same
                // include paths and standard version it was built with, since nothing else says what
                // this generated unit's own arguments were (robustness design C2's own fallback, for
                // sources rather than for open documents).
                for (auto& set : model.database.sets) {
                    const auto importer = std::ranges::find_if(set.units, [&](const spec::TranslationUnit& unit) {
                        return std::ranges::find(unit.requiredModules, name) != unit.requiredModules.end();
                    });
                    if (importer == set.units.end()) continue;
                    // `.arguments` and `.options` (when the importer has structured ones) are kept
                    // as they are: the same compiler, include paths and standard the importer was
                    // built with, which is the best guess for a file mcppls never saw a command for
                    // (the same reasoning normalize/plan.cpp's own openSources fallback uses).
                    spec::TranslationUnit recoveredUnit { *importer };
                    recoveredUnit.source = platform::fs::canonical_path(*recovered);
                    recoveredUnit.object.clear();
                    recoveredUnit.isPrivate = false;
                    // What it imports, from the file itself: a generated module may import others (`std`).
                    recoveredUnit.requiredModules = scanner ? required_names(scanner(recoveredUnit.source)) : std::vector<std::string> {};
                    recoveredUnit.providedModules = { { name, std::string {} } };
                    recoveredUnit.role = spec::Role::module_interface;
                    set.units.push_back(std::move(recoveredUnit));
                    model.notices.push_back(ModelIssue { "generated-module-recovered",
                        std::format("module {} is not in the build description; its generated source was found at {} and used instead of a stand-in",
                                    name, *recovered) });
                    break;
                }
            }
        }
    }
    model.level = producedDatabase ? std::max(spec::conformance_level(model.database), 1) : 2;
    // The tier says how the model was obtained, not what kind of project it is: an mcpp project
    // described through mcpp's compile_commands.json (an mcpp too old to emit a build database) is
    // the design's L3 -- a database plus scanning -- not L1.
    model.tier = model.source == SourceKind::mcpp && !producedDatabase ? tier_of(SourceKind::compile_commands) : tier_of(model.source);
    set_profile(model, options.kit);
    if (options.probeCache != nullptr) options.probeCache->save();
    return model;
}

} // namespace mcppls::project
