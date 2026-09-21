module mcppls.project.cmake;

import std;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.base.log;
import mcppls.platform.fs;
import mcppls.platform.process;
import mcppls.platform.toolrun;
import mcppls.spec.database;
import mcppls.toolchain.probe;
import mcppls.project.detect;
import mcppls.project.compdb;
import mcppls.project.scan;
import mcppls.project.infer;
import mcppls.project.provider;

namespace mcppls::project {

namespace {

base::Result<InferredDatabase> from_commands(std::string_view path, const Detection& detection, const ProviderContext& context) {
    auto commands = read_compile_commands(path);
    if (!commands) return std::unexpected { commands.error() };
    auto database = database_from_commands(*commands, base::file_name(detection.root), context.scanner, context.prober);
    database.database.generator = spec::Generator { "cmake", "compile_commands.json" };
    return database;
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

} // namespace

std::optional<std::string> build_database_gate_uuid(std::string_view versionOutput) {
    const auto version = cmake_major_minor(versionOutput);
    if (!version) return std::nullopt;
    for (const auto& gate : BUILD_DATABASE_GATES) {
        if (gate.major == version->first && gate.minor == version->second) return std::string { gate.uuid };
    }
    return std::nullopt;
}

std::vector<std::string> cmake_configure_arguments(std::string_view root, std::string_view buildDirectory, bool useNinja,
                                                    const std::optional<std::string>& gateUuid) {
    std::vector<std::string> arguments { "-S", std::string { root }, "-B", std::string { buildDirectory }, "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON" };
    if (useNinja && gateUuid) {
        arguments.push_back(std::format("-DCMAKE_EXPERIMENTAL_EXPORT_BUILD_DATABASE={}", *gateUuid));
        arguments.emplace_back("-DCMAKE_EXPORT_BUILD_DATABASE=ON");
    }
    if (useNinja) {
        arguments.emplace_back("-G");
        arguments.emplace_back("Ninja");
    }
    return arguments;
}

base::Result<InferredDatabase> load_cmake(const Detection& detection, std::string_view privateBuildDirectory, const ProviderContext& context) {
    if (!detection.buildDatabase.empty()) {
        auto database = spec::load_database(detection.buildDatabase);
        if (database) return enrich_database(std::move(*database), context.scanner, context.prober);
        base::log::warning("ignoring {}: {}", detection.buildDatabase, database.error().message);
    }
    if (!detection.compileCommands.empty()) return from_commands(detection.compileCommands, detection, context);

    if (!context.trusted) return base::fail("untrusted-workspace", "the workspace is not trusted, so CMake was not configured");
    if (!context.runBuildTool) return base::fail("cmake-no-database", "mcppls.buildTool is off, so CMake was not configured");
    const auto cmake = find_tool("cmake", std::vector<std::string> {});
    if (!cmake) return base::fail("cmake-not-found", "CMakeLists.txt has no build directory and cmake is not on PATH");
    context.producerUsed = *cmake;
    const std::string buildDirectory { privateBuildDirectory };
    (void)platform::fs::create_directories(buildDirectory);
    const bool useNinja { static_cast<bool>(find_tool("ninja", std::vector<std::string> {})) };

    // A quick, separately-timed query (mirrors mcpp's own `--protocol-version`
    // probe in project/mcpp.cpp): the configure below can afford its full
    // budget even when this one is slow or unparsable.
    const auto versionAnswer = platform::toolrun::run({
        .program = *cmake,
        .arguments = { "--version" },
        .workDirectory = detection.root,
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

    // The build-database export only merges correctly on the CMake releases
    // BUILD_DATABASE_GATES lists, and was only measured with Ninja, the
    // generator this configure already prefers for module scanning; everywhere
    // else it looks exactly like it did before this table existed.
    const bool wantBuildDatabase { useNinja && gateUuid.has_value() };

    // CMake has no offline mode of its own. A private build directory being configured for the
    // first time may fetch what the project declares (FetchContent, ExternalProject, file(DOWNLOAD)),
    // and that is the one deliberate exception to "an implicit run does not reach the network"
    // (design 4.4, decision 7): without it a project built on FetchContent has no build description
    // at all. Every later configure adds FETCHCONTENT_UPDATES_DISCONNECTED, so nothing is refreshed.
    const bool firstConfigure { !platform::fs::is_regular_file(base::join_path(buildDirectory, "CMakeCache.txt")) };
    if (firstConfigure) base::log::info("configuring {} for the first time; it may download what the project declares", buildDirectory);
    auto result = platform::toolrun::run({
        .program = *cmake,
        .arguments = cmake_configure_arguments(detection.root, buildDirectory, useNinja, gateUuid),
        .workDirectory = detection.root,
        .purpose = firstConfigure ? "configure-first" : "configure",
        .root = context.rootKey,
        .network = firstConfigure ? platform::toolrun::Network::allowed : platform::toolrun::Network::offline,
        .bounds = platform::RunBounds { .hard = context.configureTimeout },
        .soft = context.producerSoft,
        .environmentWait = context.environmentWait,
        .onSoftDeadline = context.onSlow,
    });
    if (!result) return std::unexpected { result.error() };
    if (result->timedOut || result->exitCode != 0) {
        return base::fail("cmake-configure-failed", std::format("cmake configure failed ({}): {}", result->exitCode,
                                                                base::trim(result->error.empty() ? result->output : result->error)));
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
                return enrich_database(std::move(*loaded), context.scanner, context.prober);
            }
            base::log::warning("cmake's build_database.json came out empty; using its compile database instead");
        } else if (built) {
            base::log::warning("cmake --build --target build_database.json failed ({}): {}", built->exitCode,
                                base::trim(built->error.empty() ? built->output : built->error));
        }
    }

    const std::string commands { base::join_path(buildDirectory, "compile_commands.json") };
    if (!platform::fs::is_regular_file(commands)) return base::fail("cmake-no-database", "cmake did not write compile_commands.json");
    return from_commands(commands, detection, context);
}

} // namespace mcppls::project
