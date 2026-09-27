module mcppls.project.cmake;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.base.log;
import mcppls.platform.fs;
import mcppls.platform.env;
import mcppls.platform.process;
import mcppls.platform.toolrun;
import mcppls.spec.database;
import mcppls.spec.discovery;
import mcppls.toolchain.probe;
import mcppls.project.infer;
import mcppls.project.provider;

namespace mcppls::project {

namespace fs = platform::fs;

namespace {

base::Result<InferredDatabase> from_commands(std::string_view path, std::string_view root, const ProviderContext& context) {
    return database_from_compile_commands_file(path, root, "cmake", context.scanner, context.prober);
}

// CMake's CMAKE_EXPERIMENTAL_EXPORT_BUILD_DATABASE gate needs a UUID that
// Kitware rotates with the feature (Source/cmExperimental.cxx); a wrong one
// fails the generate step outright ("CMake Generate step failed"), so the
// private configure only passes the switch for a (major, minor) measured
// here. Measured against the official linux-x86_64 binaries with Clang 22.1.8
// and Ninja: configuring, then building the "build_database.json" target, of
// a two-set modules project (a library with one exported partition and an
// executable that imports it) and, separately, of an ordinary non-modules
// project:
//   - 3.31.0-3.31.12 and 4.0.0-4.0.7 (UUID 4bd552e2-b7fb-429a-ab23-c83ef53f3f13)
//     and 4.1.0-4.3.5 (UUID 73194a1d-c0b5-41b9-9190-a4512925e192) configure,
//     and even fully build, without error, but their Ninja generator never
//     wires the per-target databases into the merge step: build_database.json
//     always comes out `{"sets": []}`, for either project, even after a full
//     build (checked end to end on 3.31.6 and 4.3.5; the UUID-only boundaries
//     were read from Source/cmExperimental.cxx at every tag from each line's
//     .0 release to its latest patch). Not worth the extra configure flags or
//     build step.
//   - 4.4.0-4.4.3 (UUID 70ef007e-b743-492d-9407-e35eeac03a40, the value this
//     task specified for 4.4.2) merge correctly for both projects (checked
//     end to end on 4.4.0 and 4.4.2); patch releases only backport bug fixes,
//     so the same UUID and behaviour are assumed for the rest of the 4.4 line.
// A (major, minor) this table has no row for falls back to the compile
// database, exactly as before this table existed.
struct GateEntry {
    int major;
    int minor;
    std::string_view uuid;
};
constexpr std::array<GateEntry, 1> BUILD_DATABASE_GATES { {
    { 4, 4, "70ef007e-b743-492d-9407-e35eeac03a40" },
} };

// "cmake version 4.4.2\n\nCMake suite ..." -> {4, 4}. Anything that does not
// start with the expected prefix, or whose first two components are not
// plain integers, cannot be matched against the table above.
std::optional<std::pair<int, int>> cmake_major_minor(std::string_view versionOutput) {
    constexpr std::string_view PREFIX { "cmake version " };
    const auto lines = base::split_lines(versionOutput);
    if (lines.empty()) return std::nullopt;
    const std::string_view first { base::trim(lines.front()) };
    if (!first.starts_with(PREFIX)) return std::nullopt;
    const auto parts = base::split(first.substr(PREFIX.size()), '.');
    if (parts.size() < 2) return std::nullopt;
    int major { 0 };
    int minor { 0 };
    const auto [majorEnd, majorError] = std::from_chars(parts[0].data(), parts[0].data() + parts[0].size(), major);
    const auto [minorEnd, minorError] = std::from_chars(parts[1].data(), parts[1].data() + parts[1].size(), minor);
    if (majorError != std::errc {} || majorEnd != parts[0].data() + parts[0].size()) return std::nullopt;
    if (minorError != std::errc {} || minorEnd != parts[1].data() + parts[1].size()) return std::nullopt;
    return std::pair { major, minor };
}

// A database with at least one real translation unit: an empty or all-empty
// `sets` means the merge did not work (see BUILD_DATABASE_GATES above), the
// same test load_project uses to decide a source produced nothing usable.
bool has_translation_units(const spec::Database& database) {
    return !database.sets.empty() && !std::ranges::all_of(database.sets, [](const spec::Set& set) { return set.units.empty(); });
}

// The conventional build directory names and layouts CMake tutorials and IDEs use, in no
// particular preference beyond "found first, sorted"; a configure preset's own `binaryDir`
// (resolve_cmake_preset) is tried before these.
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

// `${sourceDir}`, `${sourceParentDir}`, `${presetName}`, `${sourceDirName}` and `$env{NAME}`
// (CMakePresets.json's own macro grammar, the subset the plan calls for -- `${generator}` and the
// rest are not read by anything here). An unknown `${...}` macro is left as written rather than
// dropped, so a preset this cannot fully expand still shows what it meant to say.
std::string expand_preset_macros(std::string_view text, std::string_view root, std::string_view presetName) {
    std::string out;
    for (std::size_t i { 0 }; i < text.size();) {
        if (text.substr(i).starts_with("${")) {
            const std::size_t close { text.find('}', i) };
            if (close != std::string_view::npos) {
                const std::string_view name { text.substr(i + 2, close - i - 2) };
                if (name == "sourceDir") out += root;
                else if (name == "sourceParentDir") out += base::parent_path(root);
                else if (name == "sourceDirName") out += base::file_name(root);
                else if (name == "presetName") out += presetName;
                else out += text.substr(i, close - i + 1);
                i = close + 1;
                continue;
            }
        }
        if (text.substr(i).starts_with("$env{")) {
            const std::size_t close { text.find('}', i) };
            if (close != std::string_view::npos) {
                const std::string_view name { text.substr(i + 5, close - i - 5) };
                out += platform::env::get(name).value_or(std::string {});
                i = close + 1;
                continue;
            }
        }
        out += text[i++];
    }
    return out;
}

struct RawPreset {
    std::string name;
    bool hidden { false };
    std::vector<std::string> inherits;
    std::optional<std::string> binaryDir;
    std::optional<std::string> generator;
    std::optional<std::string> toolchainFile;
    std::vector<std::pair<std::string, std::string>> cacheVariables;
};

std::optional<RawPreset> parse_one_preset(const nlohmann::json& entry) {
    if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string()) return std::nullopt;
    RawPreset preset;
    preset.name = entry["name"].get<std::string>();
    preset.hidden = entry.value("hidden", false);
    if (const auto it = entry.find("inherits"); it != entry.end()) {
        if (it->is_string()) preset.inherits.push_back(it->get<std::string>());
        else if (it->is_array()) {
            for (const auto& parent : *it) {
                if (parent.is_string()) preset.inherits.push_back(parent.get<std::string>());
            }
        }
    }
    if (const auto it = entry.find("binaryDir"); it != entry.end() && it->is_string()) preset.binaryDir = it->get<std::string>();
    if (const auto it = entry.find("generator"); it != entry.end() && it->is_string()) preset.generator = it->get<std::string>();
    if (const auto it = entry.find("toolchainFile"); it != entry.end() && it->is_string()) preset.toolchainFile = it->get<std::string>();
    if (const auto it = entry.find("cacheVariables"); it != entry.end() && it->is_object()) {
        for (const auto& item : it->items()) {
            const std::string& key { item.key() };
            const nlohmann::json& value { item.value() };
            if (value.is_string()) {
                preset.cacheVariables.emplace_back(key, value.get<std::string>());
            } else if (value.is_boolean()) {
                preset.cacheVariables.emplace_back(key, value.get<bool>() ? "ON" : "OFF");
            } else if (value.is_object()) {
                // {"type": "...", "value": "..."} -- the object form CMakePresets.json also allows.
                if (const auto v = value.find("value"); v != value.end()) {
                    if (v->is_string()) preset.cacheVariables.emplace_back(key, v->get<std::string>());
                    else if (v->is_boolean()) preset.cacheVariables.emplace_back(key, v->get<bool>() ? "ON" : "OFF");
                }
            }
        }
    }
    return preset;
}

// Reads one presets file's `configurePresets` into `byName`, in file order; a name already present
// (from CMakePresets.json, when this is the user's file) is replaced outright, not merged --
// "user presets win" means the user's own spelling of that preset is the whole of it.
void load_presets_file(std::string_view path, std::vector<std::string>& order, std::map<std::string, RawPreset, std::less<>>& byName) {
    const auto text = fs::read_file(path);
    if (!text) return;
    const nlohmann::json document = nlohmann::json::parse(*text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) return;
    const auto configurePresets = document.find("configurePresets");
    if (configurePresets == document.end() || !configurePresets->is_array()) return;
    for (const auto& entry : *configurePresets) {
        auto preset = parse_one_preset(entry);
        if (!preset) continue;
        if (!byName.contains(preset->name)) order.push_back(preset->name);
        byName[preset->name] = std::move(*preset);
    }
}

// `inherits` chains, later parents overriding earlier ones and the preset's own fields overriding
// anything inherited (CMake's own precedence); `seen` guards a cycle or a name nothing defines.
RawPreset resolve_chain(const std::string& name, const std::map<std::string, RawPreset, std::less<>>& byName,
                        std::set<std::string, std::less<>>& seen) {
    const auto it = byName.find(name);
    if (it == byName.end() || seen.size() > 32 || seen.contains(name)) return RawPreset { .name = name };
    seen.insert(name);
    RawPreset merged;
    for (const auto& parentName : it->second.inherits) {
        const RawPreset parent { resolve_chain(parentName, byName, seen) };
        if (parent.binaryDir) merged.binaryDir = parent.binaryDir;
        if (parent.generator) merged.generator = parent.generator;
        if (parent.toolchainFile) merged.toolchainFile = parent.toolchainFile;
        for (const auto& [key, value] : parent.cacheVariables) {
            const auto found = std::ranges::find_if(merged.cacheVariables, [&](const auto& kv) { return kv.first == key; });
            if (found != merged.cacheVariables.end()) found->second = value; else merged.cacheVariables.emplace_back(key, value);
        }
    }
    if (it->second.binaryDir) merged.binaryDir = it->second.binaryDir;
    if (it->second.generator) merged.generator = it->second.generator;
    if (it->second.toolchainFile) merged.toolchainFile = it->second.toolchainFile;
    for (const auto& [key, value] : it->second.cacheVariables) {
        const auto found = std::ranges::find_if(merged.cacheVariables, [&](const auto& kv) { return kv.first == key; });
        if (found != merged.cacheVariables.end()) found->second = value; else merged.cacheVariables.emplace_back(key, value);
    }
    merged.name = name;
    merged.hidden = it->second.hidden;
    return merged;
}

} // namespace

std::optional<std::string> build_database_gate_uuid(std::string_view versionOutput) {
    const auto version = cmake_major_minor(versionOutput);
    if (!version) return std::nullopt;
    for (const auto& gate : BUILD_DATABASE_GATES) {
        if (gate.major == version->first && gate.minor == version->second) return std::string { gate.uuid };
    }
    return std::nullopt;
}

std::optional<ResolvedCMakePreset> resolve_cmake_preset(std::string_view root) {
    std::vector<std::string> order;
    std::map<std::string, RawPreset, std::less<>> byName;
    load_presets_file(base::join_path(root, "CMakePresets.json"), order, byName);
    load_presets_file(base::join_path(root, "CMakeUserPresets.json"), order, byName);
    for (const auto& name : order) {
        std::set<std::string, std::less<>> seen;
        const RawPreset resolved { resolve_chain(name, byName, seen) };
        if (resolved.hidden) continue;
        ResolvedCMakePreset out;
        out.name = name;
        if (resolved.binaryDir) out.binaryDir = base::normalize_path(expand_preset_macros(*resolved.binaryDir, root, name));
        if (resolved.generator) out.generator = *resolved.generator;
        if (resolved.toolchainFile) out.toolchainFile = expand_preset_macros(*resolved.toolchainFile, root, name);
        for (const auto& [key, value] : resolved.cacheVariables) out.cacheVariables.emplace_back(key, expand_preset_macros(value, root, name));
        return out;
    }
    return std::nullopt;
}

std::vector<std::string> cmake_configure_arguments(std::string_view root, std::string_view buildDirectory, bool useNinja,
                                                    const std::optional<std::string>& gateUuid, bool offline,
                                                    const ResolvedCMakePreset* preset) {
    std::vector<std::string> arguments { "-S", std::string { root }, "-B", std::string { buildDirectory }, "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON" };
    // B-3(a), 2026-09-27 plan D1: offline first. BD7's "the first configure may download" is
    // withdrawn -- this is passed unconditionally on the first configure too, and never when the
    // user asked mcppls to go online (design 4.4).
    if (offline) arguments.emplace_back("-DFETCHCONTENT_FULLY_DISCONNECTED=ON");
    const bool presetPicksGenerator { preset != nullptr && !preset->generator.empty() };
    if (preset != nullptr) {
        // B-3(b): a private configure that ignored the user's own preset could end up describing a
        // different compiler than the one they actually build with (plan §3.3); `architecture` and
        // `toolset` are IDE-generator concepts (Visual Studio's `-A`/`-T`) this private configure,
        // which always names a generator explicitly, has no use for.
        if (!preset->toolchainFile.empty()) arguments.push_back(std::format("-DCMAKE_TOOLCHAIN_FILE={}", preset->toolchainFile));
        for (const auto& [name, value] : preset->cacheVariables) arguments.push_back(std::format("-D{}={}", name, value));
    }
    // The build-database export only merges correctly on the CMake releases BUILD_DATABASE_GATES
    // lists, and was only measured with Ninja; a preset that names a different generator is
    // followed instead (plan §3.3), and the gate is skipped rather than guessed at.
    if (useNinja && gateUuid && (!presetPicksGenerator || preset->generator == "Ninja")) {
        arguments.push_back(std::format("-DCMAKE_EXPERIMENTAL_EXPORT_BUILD_DATABASE={}", *gateUuid));
        arguments.emplace_back("-DCMAKE_EXPORT_BUILD_DATABASE=ON");
    }
    if (presetPicksGenerator) {
        arguments.emplace_back("-G");
        arguments.push_back(preset->generator);
    } else if (useNinja) {
        arguments.emplace_back("-G");
        arguments.emplace_back("Ninja");
    }
    return arguments;
}

std::vector<std::string> fetchcontent_missing_dependencies(std::string_view output) {
    static constexpr std::string_view MARKER { "requires the source directory for dependency " };
    std::vector<std::string> names;
    if (!output.contains("FETCHCONTENT_FULLY_DISCONNECTED")) return names;
    // Measured against a real cmake 4.4.2 (this task, an offline FetchContent_MakeAvailable with no
    // source populated): the message CMake actually prints wraps this exact phrase across a line
    // break and reindents it ("requires the source\n  directory for dependency fmt to already be
    // populated"), so a plain contiguous search for MARKER never matches the real message -- only a
    // hand-typed one-line test string. Every run of whitespace collapses to one space first.
    std::string collapsed;
    collapsed.reserve(output.size());
    bool lastWasSpace { false };
    for (const char c : output) {
        const bool isSpace { c == ' ' || c == '\t' || c == '\n' || c == '\r' };
        if (isSpace) {
            if (!lastWasSpace) collapsed += ' ';
        } else {
            collapsed += c;
        }
        lastWasSpace = isSpace;
    }
    std::size_t pos { 0 };
    while (true) {
        const std::size_t found { collapsed.find(MARKER, pos) };
        if (found == std::string::npos) break;
        const std::string_view rest { std::string_view { collapsed }.substr(found + MARKER.size()) };
        const std::size_t end { rest.find(' ') };
        if (std::string name { rest.substr(0, end) }; !name.empty() && std::ranges::find(names, name) == names.end()) {
            names.push_back(std::move(name));
        }
        pos = found + MARKER.size();
    }
    return names;
}

std::optional<Claim> CmakeProvider::detect(std::string_view root) const {
    const std::string manifest { base::join_path(std::string { root }, "CMakeLists.txt") };
    if (!fs::is_regular_file(manifest)) return std::nullopt;
    Claim claim;
    claim.provider = "cmake";
    claim.confidence = 100;
    claim.root = std::string { root };
    claim.manifest = manifest;
    if (const auto preset = resolve_cmake_preset(root); preset && !preset->binaryDir.empty()) {
        claim.markers.push_back(std::format("configure preset {}", preset->name));
        const std::string database { base::join_path(preset->binaryDir, "build_database.json") };
        const std::string commands { base::join_path(preset->binaryDir, "compile_commands.json") };
        if (fs::is_regular_file(database)) {
            claim.buildDirectory = preset->binaryDir;
            claim.buildDatabase = database;
        } else if (fs::is_regular_file(commands)) {
            claim.buildDirectory = preset->binaryDir;
            claim.compileCommands = commands;
        }
    }
    if (claim.buildDatabase.empty() && claim.compileCommands.empty()) {
        for (const auto& directory : cmake_build_directories(root)) {
            const std::string database { base::join_path(directory, "build_database.json") };
            const std::string commands { base::join_path(directory, "compile_commands.json") };
            if (fs::is_regular_file(database) && claim.buildDatabase.empty()) {
                claim.buildDirectory = directory;
                claim.buildDatabase = database;
            }
            if (fs::is_regular_file(commands) && claim.compileCommands.empty()) {
                if (claim.buildDirectory.empty()) claim.buildDirectory = directory;
                claim.compileCommands = commands;
            }
        }
    }
    if (claim.buildDirectory.empty()) {
        if (const auto directories = cmake_build_directories(root); !directories.empty()) claim.buildDirectory = directories.front();
    }
    return claim;
}

std::optional<Answer> CmakeProvider::existing(const Claim& claim, const ProviderContext& context) const {
    if (!claim.buildDatabase.empty()) {
        auto database = spec::load_database(claim.buildDatabase);
        if (database) return Answer { .outcome = Outcome::ok, .database = enrich_database(std::move(*database), context.scanner, context.prober) };
        base::log::warning("ignoring {}: {}", claim.buildDatabase, database.error().message);
    }
    if (!claim.compileCommands.empty()) return from_result(from_commands(claim.compileCommands, claim.root, context));
    return std::nullopt;
}

Answer CmakeProvider::describe(const Claim& claim, const ProviderContext& context) const {
    if (!context.trusted) return Answer { .code = "untrusted-workspace", .reason = "the workspace is not trusted, so CMake was not configured" };
    if (!context.runBuildTool) return Answer { .code = "cmake-no-database", .reason = "mcppls.buildTool is off, so CMake was not configured" };
    const auto cmake = find_tool("cmake", std::vector<std::string> {});
    if (!cmake) return Answer { .code = "cmake-not-found", .reason = "CMakeLists.txt has no build directory and cmake is not on PATH" };
    context.producerUsed = *cmake;
    const std::string buildDirectory { context.privateDirectory };
    (void)fs::create_directories(buildDirectory);
    const bool useNinja { static_cast<bool>(find_tool("ninja", std::vector<std::string> {})) };

    // A quick, separately-timed query (mirrors mcpp's own `--protocol-version`
    // probe in project/mcpp.cpp): the configure below can afford its full
    // budget even when this one is slow or unparsable.
    const auto versionAnswer = platform::toolrun::run({
        .program = *cmake,
        .arguments = { "--version" },
        .workDirectory = claim.root,
        .purpose = "protocol",
        .root = context.rootKey,
        .bounds = platform::RunBounds { .hard = std::chrono::seconds { 20 } },
        .environmentWait = context.environmentWait,
    });
    std::optional<std::string> gateUuid;
    if (versionAnswer && !versionAnswer->timedOut && versionAnswer->exitCode == 0) {
        gateUuid = build_database_gate_uuid(versionAnswer->output);
        context.producerVersionUsed = std::string { base::trim(base::split_lines(versionAnswer->output).empty() ? std::string_view {}
                                                                                                                : base::split_lines(versionAnswer->output).front()) };
    }
    const bool wantBuildDatabase { useNinja && gateUuid.has_value() };

    const auto preset = resolve_cmake_preset(claim.root);
    const ResolvedCMakePreset* presetPtr { preset ? &*preset : nullptr };

    const bool firstConfigure { !fs::is_regular_file(base::join_path(buildDirectory, "CMakeCache.txt")) };
    if (firstConfigure) {
        base::log::info("configuring {} for the first time{}", buildDirectory,
                        context.offline ? " (offline: FETCHCONTENT_FULLY_DISCONNECTED)" : "; it may reach the network");
    }
    auto result = platform::toolrun::run({
        .program = *cmake,
        .arguments = cmake_configure_arguments(claim.root, buildDirectory, useNinja, gateUuid, context.offline, presetPtr),
        .workDirectory = claim.root,
        .purpose = firstConfigure ? "configure-first" : "configure",
        .root = context.rootKey,
        .network = context.offline ? platform::toolrun::Network::offline : platform::toolrun::Network::allowed,
        .bounds = platform::RunBounds { .hard = context.configureTimeout },
        .soft = context.producerSoft,
        .environmentWait = context.environmentWait,
        .onSoftDeadline = context.onSlow,
    });
    if (!result) return Answer { .code = result.error().code, .reason = result.error().message };
    if (result->timedOut) {
        return Answer { .outcome = Outcome::timed_out, .code = "cmake-configure-failed",
                        .reason = std::format("cmake configure did not finish in {} ms", context.configureTimeout.count()) };
    }
    if (result->exitCode != 0) {
        // B-3(a): the one shape an offline, FETCHCONTENT_FULLY_DISCONNECTED configure fails with
        // that is not a failure of the project's own CMakeLists.txt -- a dependency's source is not
        // already populated, and only a download (or an online configure) can populate it.
        if (context.offline) {
            if (auto missing = fetchcontent_missing_dependencies(result->output + "\n" + result->error); !missing.empty()) {
                return Answer { .outcome = Outcome::needs_download, .code = std::string { spec::NEEDS_DOWNLOAD },
                                .reason = std::format("cmake needs {} downloaded, and this run stayed offline (FETCHCONTENT_FULLY_DISCONNECTED)",
                                                      base::join(missing, ", ")),
                                .missing = missing };
            }
        }
        return Answer { .code = "cmake-configure-failed",
                        .reason = std::format("cmake configure failed ({}): {}", result->exitCode,
                                              base::trim(result->error.empty() ? result->output : result->error)) };
    }

    if (wantBuildDatabase) {
        // The default target does not produce the merged file (E16); Ninja
        // names the file itself as a buildable target (build.ninja: "build
        // build_database.json: CUSTOM_COMMAND ..."), and this alone is fast
        // because it only reruns the dependency scan, not a full compile.
        auto built = platform::toolrun::run({
            .program = *cmake,
            .arguments = { "--build", buildDirectory, "--target", "build_database.json" },
            .purpose = "build-database",
            .root = context.rootKey,
            .bounds = platform::RunBounds { .hard = context.configureTimeout },
            .soft = context.producerSoft,
            .environmentWait = context.environmentWait,
            .onSoftDeadline = context.onSlow,
        });
        if (built && !built->timedOut && built->exitCode == 0) {
            const std::string database { base::join_path(buildDirectory, "build_database.json") };
            if (auto loaded = spec::load_database(database); loaded && has_translation_units(*loaded)) {
                return Answer { .outcome = Outcome::ok, .database = enrich_database(std::move(*loaded), context.scanner, context.prober) };
            }
            base::log::warning("cmake's build_database.json came out empty; using its compile database instead");
        } else if (built) {
            base::log::warning("cmake --build --target build_database.json failed ({}): {}", built->exitCode,
                                base::trim(built->error.empty() ? built->output : built->error));
        }
    }

    const std::string commands { base::join_path(buildDirectory, "compile_commands.json") };
    if (!fs::is_regular_file(commands)) return Answer { .code = "cmake-no-database", .reason = "cmake did not write compile_commands.json" };
    return from_result(from_commands(commands, claim.root, context));
}

std::vector<std::string> CmakeProvider::watch_inputs(const Claim& claim) const {
    std::vector<std::string> watch { "**/CMakeLists.txt", "CMakePresets.json", "CMakeUserPresets.json", "**/*.cmake" };
    if (!claim.compileCommands.empty()) watch.push_back(claim.compileCommands);
    return watch;
}

} // namespace mcppls::project
