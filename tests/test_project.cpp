// Project detection, inference from sources and compile databases, and model loading.
import std;
import mcppls.testing;
import nlohmann.json;
import mcppls.os;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.fs;
import mcppls.platform.dirs;
import mcppls.spec.database;
import mcppls.spec.kit;
import mcppls.spec.metadata;
import mcppls.toolchain.probe;
import mcppls.project.boundary;
import mcppls.project.detect;
import mcppls.project.provider;
import mcppls.project.infer;
import mcppls.project.mcpp;
import mcppls.project.model;
import mcppls.project.compdb;
import mcppls.project.cmake;
import mcppls.project.xmake;
import mcppls.project.meson;
import mcppls.project.providers;
import mcppls.project.modelcache;
import mcppls.project.generated;
import mcppls.project.scan;

namespace fs = mcppls::platform::fs;
namespace b = mcppls::base;
namespace p = mcppls::project;
namespace s = mcppls::spec;

namespace {

std::string make_root(std::string_view name) {
    const std::string root { b::join_path(mcppls::platform::dirs::temp_directory(),
        std::format("mcppls-test-project-{}-{}", name, std::chrono::steady_clock::now().time_since_epoch().count())) };
    (void)fs::create_directories(root);
    return root;
}

void write(const std::string& root, std::string_view relative, std::string_view content) {
    const std::string path { b::join_path(root, relative) };
    (void)fs::create_directories(b::parent_path(path));
    (void)fs::write_file(path, content);
}

void write_fixture(const std::string& root) {
    write(root, "src/main.cpp", "import std;\nimport hello.greet;\n\nint main() {\n    std::println(\"{}\", hello::greet(\"mcpp\"));\n}\n");
    write(root, "src/greet/greet.cppm", "export module hello.greet;\nexport import :detail;\nimport std;\n");
    write(root, "src/greet/detail.cppm", "export module hello.greet:detail;\nimport std;\n");
    write(root, "target/obj/generated.cppm", "export module should.be.skipped;\n");
    write(root, ".hidden/x.cpp", "import hidden;\n");
}

} // namespace

int main() {
    using namespace mcppls::testing;

    "detection order"_test = [] {
        const auto providers = p::registered_providers();

        const std::string mcpp { make_root("mcpp") };
        write(mcpp, "mcpp.toml", "[package]\nname = \"hello\"\n");
        write(mcpp, "CMakeLists.txt", "project(x)\n");
        expect(p::detect_project(mcpp, {}, providers).kind == p::SourceKind::mcpp);

        const std::string cmake { make_root("cmake") };
        write(cmake, "CMakeLists.txt", "project(x)\n");
        write(cmake, "build/CMakeCache.txt", "");
        write(cmake, "build/compile_commands.json", "[]");
        write(cmake, "xmake.lua", "target(\"x\")\n");   // B-1 task 1: CMake still wins over xmake
        const auto cmakeDetection = p::detect_project(cmake, {}, providers);
        expect(cmakeDetection.kind == p::SourceKind::cmake);
        expect(cmakeDetection.buildDirectory == b::join_path(cmake, "build"));
        expect(cmakeDetection.compileCommands == b::join_path(cmake, "build/compile_commands.json"));

        const std::string xmake { make_root("xmake") };
        write(xmake, "xmake.lua", "target(\"x\")\n");
        write(xmake, "meson.build", "project('x')\n");   // xmake still wins over meson
        expect(p::detect_project(xmake, {}, providers).kind == p::SourceKind::xmake);

        const std::string meson { make_root("meson") };
        write(meson, "meson.build", "project('x')\n");
        write(meson, "compile_commands.json", "[]");   // meson still wins over a bare compile_commands.json
        expect(p::detect_project(meson, {}, providers).kind == p::SourceKind::meson);

        const std::string commands { make_root("compdb") };
        write(commands, "compile_commands.json", "[]");
        expect(p::detect_project(commands, {}, providers).kind == p::SourceKind::compile_commands);

        const std::string loose { make_root("loose") };
        write(loose, "a.cppm", "export module a;\n");
        expect(p::detect_project(loose, {}, providers).kind == p::SourceKind::inferred);
        expect(p::detect_project(loose, "db.json", providers).kind == p::SourceKind::build_database);

        // B-7: buildDiscovery = false skips every provider, configuredDatabase aside.
        expect(p::detect_project(mcpp, {}, providers, false).kind == p::SourceKind::inferred)
            << "buildDiscovery off: nothing is detected even though mcpp.toml is right there";
        expect(p::detect_project(loose, "db.json", providers, false).kind == p::SourceKind::build_database)
            << "an explicit database is still used";

        // B-1 task 1 / B-7: a provider left out of the list behaves as if its files were not there.
        std::vector<p::BuildSystemProvider*> withoutMcpp;
        for (auto* provider : providers) {
            if (provider->id() != "mcpp") withoutMcpp.push_back(provider);
        }
        expect(p::detect_project(mcpp, {}, withoutMcpp).kind == p::SourceKind::cmake)
            << "mcpp filtered out: CMakeLists.txt is next";

        for (const auto& root : { mcpp, cmake, xmake, meson, commands, loose }) fs::remove_all(root);
    };

    "tier_of and to_string cover xmake and meson"_test = [] {
        expect(p::to_string(p::SourceKind::xmake) == std::string_view { "xmake" });
        expect(p::to_string(p::SourceKind::meson) == std::string_view { "meson" });
        // design §2.1: tier 3, same as a bare compile_commands.json -- exact arguments, module roles
        // recovered by scanning rather than said by the build system itself.
        expect(p::tier_of(p::SourceKind::xmake) == 3_i);
        expect(p::tier_of(p::SourceKind::meson) == 3_i);
        expect(p::tier_of(p::SourceKind::xmake) == p::tier_of(p::SourceKind::compile_commands));
    };

    "inference from sources"_test = [] {
        const std::string root { make_root("infer") };
        write_fixture(root);
        const auto inferred = p::infer_database(root, p::InferOptions {}, p::file_scanner());
        expect(fatal(inferred.database.sets.size() == 1u));
        const auto& set = inferred.database.sets.front();
        expect(set.units.size() == 3u) << "target/ and dot directories are skipped";
        std::map<std::string, const s::TranslationUnit*> byName;
        for (const auto& unit : set.units) byName[std::string { b::file_name(unit.source) }] = &unit;
        expect(fatal(byName.contains("greet.cppm") && byName.contains("detail.cppm") && byName.contains("main.cpp")));
        expect(byName["greet.cppm"]->role == std::optional<s::Role> { s::Role::module_interface });
        expect(byName["detail.cppm"]->providedModules.front().first == "hello.greet:detail");
        expect(byName["greet.cppm"]->requiredModules == std::vector<std::string> { "hello.greet:detail", "std" });
        expect(byName["main.cpp"]->role == std::optional<s::Role> { s::Role::non_module });
        expect(byName["main.cpp"]->arguments.back() == byName["main.cpp"]->source);
        expect(set.toolchain.empty());
        const auto resolution = s::resolve_module(inferred.database, 0, "hello.greet", [](std::string_view) { return std::vector<s::ModuleEntry> {}; });
        expect(resolution.from == s::ResolvedFrom::set);
        fs::remove_all(root);
    };

    "compile commands become sets per toolchain"_test = [] {
        const std::string root { make_root("sets") };
        write_fixture(root);
        std::vector<p::CompileCommand> commands;
        for (std::string_view file : { "src/main.cpp", "src/greet/greet.cppm", "src/greet/detail.cppm" }) {
            const std::string path { b::join_path(root, file) };
            commands.push_back(p::CompileCommand { root, path, "", { "/opt/gcc/bin/g++", "-std=c++23", "-c", path } });
        }
        commands.push_back(p::CompileCommand { root, b::join_path(root, "src/c.c"), "", { "/opt/gcc/bin/gcc", "-c", "src/c.c" } });
        const p::Prober prober = [](std::string_view driver, std::span<const std::string>) -> std::optional<mcppls::toolchain::ToolchainFacts> {
            mcppls::toolchain::ToolchainFacts facts;
            facts.toolchain.family = s::Family::gcc;
            facts.toolchain.version = "16.1.0";
            facts.toolchain.driver = std::string { driver };
            facts.toolchain.target = "x86_64-linux-gnu";
            return facts;
        };
        const auto result = p::database_from_commands(commands, "hello", p::file_scanner(), prober);
        expect(fatal(result.database.sets.size() == 1u));
        expect(result.database.sets.front().units.size() == 3u) << "C sources are not part of the C++ model";
        expect(result.database.sets.front().toolchain == "gcc-16.1.0-x86_64-linux-gnu");
        expect(result.facts.size() == 1u);
        expect(s::conformance_level(result.database) == 2_i);
        fs::remove_all(root);
    };

    "an untrusted workspace still gets a model"_test = [] {
        const std::string root { make_root("untrusted") };
        write_fixture(root);
        write(root, "mcpp.toml", "[package]\nname = \"hello\"\nversion = \"0.1.0\"\n");
        s::Kit kit;
        kit.name = "k";
        kit.target = "x86_64-unknown-linux-gnu";
        kit.stdlibName = "libc++";
        kit.stdlibVersion = "23.1.0";
        p::LoadOptions options;
        options.trusted = false;
        options.kit = &kit;
        options.cacheDirectory = b::join_path(root, ".cache-dir");
        const auto model = p::load_project(root, options);
        expect(model.source == p::SourceKind::inferred);
        expect(model.detected == p::SourceKind::mcpp) << "the project is still an mcpp project whose data was not available";
        expect(model.usesKit);
        expect(model.profile.kind == "semantic-kit" && model.profile.stdlib == "libc++ 23.1.0");
        expect(std::ranges::any_of(model.issues, [](const p::ModelIssue& issue) { return issue.code == "untrusted-workspace"; }))
            << "load_mcpp is never even called for an untrusted workspace now (real-project plan RP3.1): nothing a trusted "
               "session or another build once left on disk should be read just because it is sitting there";
        expect(!model.watch.empty());
        expect(model.database.sets.front().units.size() == 3u);
        fs::remove_all(root);
    };

    "B-7: mcppls.buildDiscovery = false detects, reads and runs nothing implicitly"_test = [] {
        const std::string root { make_root("build-discovery-off") };
        write_fixture(root);
        write(root, "mcpp.toml", "[package]\nname = \"hello\"\nversion = \"0.1.0\"\n");
        p::LoadOptions options;
        options.trusted = true;
        options.buildDiscovery = false;
        options.cacheDirectory = b::join_path(root, ".cache-dir");
        const auto model = p::load_project(root, options);
        expect(model.detected == p::SourceKind::inferred) << "buildDiscovery off: not even detection runs";
        expect(model.source == p::SourceKind::inferred);
        expect(std::ranges::any_of(model.notices, [](const p::ModelIssue& notice) { return notice.code == "build-discovery-off"; }))
            << "a notice, not an issue: nothing is reduced by an explicit setting";
        expect(std::ranges::none_of(model.issues, [](const p::ModelIssue& issue) { return issue.code == "build-discovery-off"; }));
        fs::remove_all(root);
    };

    "a producer's build database is completed, not replaced"_test = [] {
        const std::string root { make_root("database") };
        write_fixture(root);
        const std::string main { b::join_path(root, "src/main.cpp") };
        const std::string greet { b::join_path(root, "src/greet/greet.cppm") };
        const std::string detail { b::join_path(root, "src/greet/detail.cppm") };
        nlohmann::json document = nlohmann::json::parse(std::format(R"({{
          "version": 1, "revision": 0,
          "sets": [ {{ "name": "hello", "translation-units": [
            {{ "source": "{}", "work-directory": "{}", "arguments": ["clang++", "-c", "{}"] }},
            {{ "source": "{}", "work-directory": "{}", "arguments": ["clang++", "-c", "{}"] }},
            {{ "source": "{}", "work-directory": "{}", "arguments": ["clang++", "-c", "{}"], "ide": {{ "role": "module-partition-interface" }},
               "provides": {{ "hello.greet:detail": "" }}, "requires": ["std"] }}
          ] }} ] }})", main, root, main, greet, root, greet, detail, root, detail));
        write(root, "build/db.json", document.dump());
        // trusted: reading the one file `mcppls.database` names is not a build-tool run, but design
        // item 5 (an untrusted workspace is L4 unconditionally) does not carve out an exception for
        // it either -- an untrusted workspace's own settings could otherwise point mcppls at a file
        // of its choosing. What this test is about is database completion, not trust.
        p::LoadOptions options;
        options.trusted = true;
        const auto model = p::load_project(root, p::LoadOptions { options.trusted, {}, "build/db.json" });
        expect(model.source == p::SourceKind::build_database && model.detected == p::SourceKind::build_database);
        expect(fatal(model.database.sets.size() == 1u && model.database.sets[0].units.size() == 3u));
        const auto& units = model.database.sets[0].units;
        expect(units[1].role == std::optional<s::Role> { s::Role::module_interface }) << "scanned";
        expect(units[1].providedModules.size() == 1u && units[1].providedModules[0].first == "hello.greet");
        expect(units[2].requiredModules == std::vector<std::string> { "std" }) << "kept as the producer wrote it";
        fs::remove_all(root);
    };

    "CMake build-database gate UUIDs"_test = [] {
        // Measured (W4): 4.4.0-4.4.3 merge build_database.json correctly and
        // share this UUID; every other line CMake currently ships (3.31.x,
        // 4.0.x-4.3.x) configures fine but never merges anything into it, so
        // it is deliberately not in the table (see project/cmake.cpp).
        expect(p::build_database_gate_uuid("cmake version 4.4.2\n\nCMake suite maintained and supported by Kitware (kitware.com/cmake).\n")
               == std::optional<std::string> { "70ef007e-b743-492d-9407-e35eeac03a40" });
        expect(p::build_database_gate_uuid("cmake version 4.4.0\n") == std::optional<std::string> { "70ef007e-b743-492d-9407-e35eeac03a40" })
            << "the whole 4.4 line, not only 4.4.2";
        expect(!p::build_database_gate_uuid("cmake version 3.31.6\n\nCMake suite maintained and supported by Kitware (kitware.com/cmake).\n"))
            << "configures without error, but never merges (W4 measurement)";
        expect(!p::build_database_gate_uuid("cmake version 4.3.5\n")) << "same measurement, the other UUID group";
        expect(!p::build_database_gate_uuid("")) << "unparsable: empty output";
        expect(!p::build_database_gate_uuid("zsh: command not found: cmake\n")) << "unparsable: no \"cmake version\" prefix";
        expect(!p::build_database_gate_uuid("cmake version four point four\n")) << "unparsable: non-numeric components";
    };

    "the private configure's arguments carry the gate only when it is known"_test = [] {
        const auto withGate = p::cmake_configure_arguments("/proj", "/cache/cmake", true, std::optional<std::string> { "the-uuid" }, false);
        expect(std::ranges::find(withGate, std::string { "-DCMAKE_EXPERIMENTAL_EXPORT_BUILD_DATABASE=the-uuid" }) != withGate.end());
        expect(std::ranges::find(withGate, std::string { "-DCMAKE_EXPORT_BUILD_DATABASE=ON" }) != withGate.end());
        expect(std::ranges::find(withGate, std::string { "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON" }) != withGate.end()) << "always written";
        expect(fatal(withGate.size() >= 2u));
        expect(withGate[withGate.size() - 2] == "-G" && withGate.back() == "Ninja");

        const auto unknownVersion = p::cmake_configure_arguments("/proj", "/cache/cmake", true, std::nullopt, false);
        expect(std::ranges::none_of(unknownVersion, [](const std::string& arg) { return arg.contains("BUILD_DATABASE"); }))
            << "an unconfirmed CMake version must not see the switch";
        expect(std::ranges::find(unknownVersion, std::string { "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON" }) != unknownVersion.end())
            << "today's behaviour is otherwise unchanged";

        const auto noNinja = p::cmake_configure_arguments("/proj", "/cache/cmake", false, std::optional<std::string> { "the-uuid" }, false);
        expect(std::ranges::none_of(noNinja, [](const std::string& arg) { return arg.contains("BUILD_DATABASE") || arg == "Ninja"; }))
            << "the gate was only measured with Ninja";
    };

    "B-3(a): offline passes FETCHCONTENT_FULLY_DISCONNECTED, online never does"_test = [] {
        const auto offline = p::cmake_configure_arguments("/proj", "/cache/cmake", true, std::nullopt, true);
        expect(std::ranges::find(offline, std::string { "-DFETCHCONTENT_FULLY_DISCONNECTED=ON" }) != offline.end())
            << "D1: offline first, even on what would have been the first configure (BD7 withdrawn)";

        const auto online = p::cmake_configure_arguments("/proj", "/cache/cmake", true, std::nullopt, false);
        expect(std::ranges::none_of(online, [](const std::string& arg) { return arg.contains("FETCHCONTENT_FULLY_DISCONNECTED"); }))
            << "never passed when the user asked mcppls to go online";
    };

    "B-3(a): an offline FetchContent failure names the dependency"_test = [] {
        // Measured against a real cmake 4.4.2 (this task): `FetchContent_Declare(fmt URL
        // https://example.invalid/...)` then a target that links `fmt::fmt`, configured with
        // `-DFETCHCONTENT_FULLY_DISCONNECTED=ON` into an empty build directory. The message CMake
        // actually writes wraps the exact marker phrase across a line break and reindents it --
        // a plain contiguous search over the raw text would never match this, only a hand-typed
        // one-line string, which is why the parser collapses whitespace first.
        constexpr std::string_view MESSAGE {
            "CMake Warning at /opt/cmake/share/cmake-4.4/Modules/FetchContent.cmake:2126 (message):\n"
            "  FETCHCONTENT_FULLY_DISCONNECTED is set to true, which requires the source\n"
            "  directory for dependency fmt to already be populated.  This generally means\n"
            "  it must not be set to true the first time CMake is run in a build\n"
            "  directory.  The following source directory should already be populated, but\n"
            "  it doesn't exist:\n"
            "\n"
            "    /build/_deps/fmt-src\n"
            "\n"
            "  Policy CMP0170 controls enforcement of this requirement.\n"
            "Call Stack (most recent call first):\n"
            "  CMakeLists.txt:5 (FetchContent_MakeAvailable)\n"
            "\n"
            "\n"
            "-- Configuring done (4.6s)\n"
            "CMake Error at CMakeLists.txt:7 (target_link_libraries):\n"
            "  Target \"x\" links to:\n"
            "\n"
            "    fmt::fmt\n"
            "\n"
            "  but the target was not found.\n"
        };
        const auto missing = p::fetchcontent_missing_dependencies(MESSAGE);
        expect(missing == std::vector<std::string> { "fmt" });
        expect(p::fetchcontent_missing_dependencies("CMake Error: something else entirely").empty())
            << "a different configure failure names nothing";
        expect(p::fetchcontent_missing_dependencies(
                   "FETCHCONTENT_FULLY_DISCONNECTED is set to true, which requires the source directory for dependency fmt to already be "
                   "populated\nFETCHCONTENT_FULLY_DISCONNECTED is set to true, which requires the source directory for dependency eigen to "
                   "already be populated\n")
               == (std::vector<std::string> { "fmt", "eigen" }))
            << "every dependency named, in order, several fetched at once";
    };

    "B-3(b): CMakePresets.json, inherits and macro expansion"_test = [] {
        const std::string root { make_root("cmake-presets") };
        write(root, "CMakeLists.txt", "project(x)\n");
        write(root, "CMakePresets.json", R"json({
          "version": 3,
          "configurePresets": [
            { "name": "base", "hidden": true, "generator": "Ninja",
              "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug", "SHARED": "ON" } },
            { "name": "linux", "inherits": "base", "binaryDir": "${sourceDir}/out/${presetName}",
              "cacheVariables": { "CMAKE_BUILD_TYPE": "RelWithDebInfo" },
              "toolchainFile": "${sourceParentDir}/${sourceDirName}-toolchain.cmake" }
          ]
        })json");
        const auto preset = p::resolve_cmake_preset(root);
        expect(fatal(preset.has_value()));
        expect(preset->name == "linux");
        expect(preset->binaryDir == b::join_path(root, "out/linux")) << "${sourceDir} and ${presetName} expanded";
        expect(preset->generator == "Ninja") << "inherited from the hidden base preset";
        expect(preset->toolchainFile == std::format("{}-toolchain.cmake", root)) << "${sourceParentDir} and ${sourceDirName} expanded";
        expect(fatal(preset->cacheVariables.size() == 2u));
        const auto buildType = std::ranges::find_if(preset->cacheVariables, [](const auto& kv) { return kv.first == "CMAKE_BUILD_TYPE"; });
        expect(fatal(buildType != preset->cacheVariables.end()));
        expect(buildType->second == "RelWithDebInfo") << "the preset's own value overrides what it inherited";
        const auto shared = std::ranges::find_if(preset->cacheVariables, [](const auto& kv) { return kv.first == "SHARED"; });
        expect(fatal(shared != preset->cacheVariables.end())) << "inherited variables are kept, not only overridden ones";
        expect(shared->second == "ON");
        fs::remove_all(root);
    };

    "B-3(b): a hidden-only file has no chosen preset, and a user preset wins by name"_test = [] {
        const std::string root { make_root("cmake-presets-hidden") };
        write(root, "CMakeLists.txt", "project(x)\n");
        write(root, "CMakePresets.json", R"json({"version": 3, "configurePresets": [
            { "name": "base", "hidden": true },
            { "name": "dev", "binaryDir": "${sourceDir}/build-dev" }
        ]})json");
        expect(p::resolve_cmake_preset(root)->name == "dev");

        write(root, "CMakeUserPresets.json", R"json({"version": 3, "configurePresets": [
            { "name": "dev", "binaryDir": "${sourceDir}/build-dev-mine" }
        ]})json");
        const auto userWins = p::resolve_cmake_preset(root);
        expect(fatal(userWins.has_value()));
        expect(userWins->binaryDir == b::join_path(root, "build-dev-mine")) << "user presets win (same name: fully replaced)";
        fs::remove_all(root);
    };

    "B-5: xmake's environment, arguments and classification"_test = [] {
        const auto environment = p::xmake_environment("/cache/xmake/config");
        expect(std::ranges::find(environment, std::pair<std::string, std::string> { "XMAKE_CONFIGDIR", "/cache/xmake/config" }) != environment.end());
        expect(std::ranges::find(environment, std::pair<std::string, std::string> { "XMAKE_THEME", "plain" }) != environment.end());

        const auto offline = p::xmake_configure_arguments("/cache/xmake/build", true);
        expect(offline == (std::vector<std::string> { "f", "-c", "--confirm=no", "--policies=package.fetch_only,network.mode:private",
                                                       "--builddir=/cache/xmake/build" }));
        const auto online = p::xmake_configure_arguments("/cache/xmake/build", false);
        expect(online == (std::vector<std::string> { "f", "-c", "-y", "--builddir=/cache/xmake/build" }));

        expect(p::xmake_compile_commands_arguments("/cache/xmake/out")
               == (std::vector<std::string> { "project", "-k", "compile_commands", "/cache/xmake/out" }));

        expect(p::xmake_missing_packages("checking for platform ... linux\nThe packages(xxhash, fmt) not found, please install them first!\n")
               == (std::vector<std::string> { "xxhash", "fmt" }));
        expect(p::xmake_missing_packages(
                   "please specify weather to install them (y/n): \nnote: install or modify (m) these packages (pass -y to skip confirm)\n"
                   "in xmake.lua, packages.lua ->\n  -> zlib 1.3.1 [+debug]: xmake.lua:3\n  -> fmt 11.0.2: xmake.lua:4\n")
               == (std::vector<std::string> { "zlib", "fmt" }));
        expect(p::xmake_missing_packages("checking for platform ... linux\nbuild ok!\n").empty()) << "a successful run names nothing";

        // xmake keeps the offline policy in its own configuration: an online run must configure again.
        const std::pair<std::uint64_t, std::int64_t> manifest { 64, 1000 };
        expect(p::xmake_configuration_key(true, manifest) != p::xmake_configuration_key(false, manifest));
        expect(p::xmake_configuration_key(true, manifest) != p::xmake_configuration_key(true, std::pair<std::uint64_t, std::int64_t> { 64, 2000 }));
        expect(p::xmake_configuration_key(true, manifest) == p::xmake_configuration_key(true, manifest));
    };

    "X-2: an xmake.conf is parsed as text, top-level scalars only"_test = [] {
        // Recorded 2026-10-01 from `xmake f -m debug --fancy=y --tag=zz` (xmake v3.1.1) in a project that declares two options.
        const std::string conf {
            "{\n    __toolchains_linux_x86_64 = {\n        \"envs\",\n        \"gcc\"\n    },\n    arch = \"x86_64\",\n    builddir = \"build\",\n"
            "    ccache = true,\n    fancy = true,\n    host = \"linux\",\n    kind = \"static\",\n    mode = \"debug\",\n    ndk_stdcxx = true,\n"
            "    network = \"public\",\n    pkg_searchdirs = \"/tmp\",\n    plat = \"linux\",\n    proxy_pac = \"pac.lua\",\n    tag = \"zz\",\n"
            "    theme = \"default\"\n}" };
        const auto options = p::parse_xmake_conf(conf);
        const auto get = [&](std::string_view name) -> const p::XmakeOption* {
            const auto it = std::ranges::find_if(options, [&](const p::XmakeOption& option) { return option.name == name; });
            return it == options.end() ? nullptr : &*it;
        };
        expect(fatal(get("mode") != nullptr));
        expect(get("mode")->value == "debug" && !get("mode")->flag);
        expect(fatal(get("fancy") != nullptr));
        expect(get("fancy")->value == "true" && get("fancy")->flag);
        expect(get("ccache")->flag);
        expect(get("envs") == nullptr) << "a nested table's items are not keys";
        expect(get("__toolchains_linux_x86_64") == nullptr) << "a table is not a scalar";

        // Strings with escapes and quotes, false, numbers and expressions the parser does not take, a comment-free Lua it never runs.
        const auto odd = p::parse_xmake_conf("{ a = 'it\\'s', b = false, n = 3, e = 1 + 2, t = { x = \"no\" }, z = \"last\" }");
        expect(odd == (std::vector<p::XmakeOption> { { "a", "it's", false }, { "b", "false", true }, { "z", "last", false } }));
        expect(p::parse_xmake_conf("").empty());
        expect(p::parse_xmake_conf("not a table").empty());
        expect(p::parse_xmake_conf("{ mode = \"debug\"").size() == 1) << "a file cut short keeps what was complete";
    };

    "X-2: the private configuration follows the user's choices, without internal keys"_test = [] {
        const std::vector<p::XmakeOption> user {
            { "arch", "x86_64", false }, { "builddir", "build", false }, { "ccache", "true", true }, { "fancy", "true", true },
            { "host", "linux", false }, { "kind", "static", false }, { "mode", "debug", false }, { "ndk_stdcxx", "true", true },
            { "network", "public", false }, { "pkg_searchdirs", "/tmp", false }, { "plat", "linux", false }, { "policies", "x", false },
            { "proxy_pac", "pac.lua", false }, { "__private", "1", false }, { "tag", "zz", false }, { "theme", "default", false },
            { "toolchain", "gcc", false }, { "sdk", "", false }, { "clean", "true", true } };
        const auto arguments = p::xmake_configure_arguments("/c/build", true, user);
        expect(arguments == (std::vector<std::string> { "f", "-c", "-p", "linux", "-a", "x86_64", "-m", "debug", "--fancy=y", "--kind=static",
                                                         "--tag=zz", "--toolchain=gcc", "--confirm=no",
                                                         "--policies=package.fetch_only,network.mode:private", "--builddir=/c/build" }))
            << b::join(arguments, " ");
        expect(p::xmake_configure_arguments("/c/build", false, user).back() == "--builddir=/c/build");
        const auto online = p::xmake_configure_arguments("/c/build", false, user);
        expect(std::ranges::find(online, "-y") != online.end());

        // The retry after "Invalid option": the standard ones only, and the notice names what was left out.
        const auto reduced = p::xmake_configure_arguments("/c/build", true, user, true);
        expect(reduced == (std::vector<std::string> { "f", "-c", "-p", "linux", "-a", "x86_64", "-m", "debug", "--kind=static", "--toolchain=gcc",
                                                       "--confirm=no", "--policies=package.fetch_only,network.mode:private", "--builddir=/c/build" }))
            << b::join(reduced, " ");
        expect(p::xmake_left_out(user) == (std::vector<std::string> { "fancy", "tag" }));
        for (const char* name : { "plat", "arch", "mode", "toolchain", "sdk", "runtimes", "kind" }) expect(p::xmake_option_is_standard(name)) << name;
        for (const char* name : { "builddir", "host", "theme", "network", "proxy_pac", "ccache", "policies", "pkg_searchdirs", "ndk_stdcxx", "__toolchains_linux_x86_64" }) {
            expect(p::xmake_option_is_internal(name)) << name;
        }
        expect(!p::xmake_option_is_internal("fancy"));

        // No .xmake/: the arguments are today's.
        expect(p::xmake_configure_arguments("/c/build", false) == (std::vector<std::string> { "f", "-c", "-y", "--builddir=/c/build" }));

        // What xmake v3.1.1 printed for `xmake f -c --nosuch=1` (exit code 255).
        expect(p::xmake_unknown_option("[38;2;0;255;0m        --tag=TAG   The tag option\nerror: Invalid option: --nosuch=1\n"));
        expect(p::xmake_unknown_option("error: unknown option: --x"));
        expect(!p::xmake_unknown_option("The packages(fmt) not found"));
    };

    "X-2: the newest .xmake/<plat>/<arch>/xmake.conf is the user's configuration, read only"_test = [] {
        const std::string root { make_root("xmake-conf") };
        expect(!p::read_xmake_user_config(root).has_value()) << "no .xmake/: today's behaviour";
        write(root, ".xmake/linux/x86_64/xmake.conf", "{ mode = \"release\", plat = \"linux\" }");
        write(root, ".xmake/linux/x86_64/cache/other", "x");
        const auto first = p::read_xmake_user_config(root);
        expect(fatal(first.has_value()));
        expect(first->path == b::join_path(root, ".xmake/linux/x86_64/xmake.conf"));
        expect(first->options.size() == 2);
        // A later `xmake f` for another architecture is the last one.
        std::this_thread::sleep_for(std::chrono::milliseconds { 30 });
        write(root, ".xmake/linux/arm64/xmake.conf", "{ mode = \"debug\", plat = \"linux\", arch = \"arm64\" }");
        const auto second = p::read_xmake_user_config(root);
        expect(fatal(second.has_value()));
        expect(second->path == b::join_path(root, ".xmake/linux/arm64/xmake.conf"));
        expect(second->options.size() == 3);
        fs::remove_all(root);
    };

    "X-2: the configuration key includes the user's xmake.conf, X-1: staleness of the project's file"_test = [] {
        const std::pair<std::uint64_t, std::int64_t> manifest { 64, 1000 };
        const std::pair<std::uint64_t, std::int64_t> conf { 300, 5000 };
        expect(p::xmake_configuration_key(true, manifest, conf) != p::xmake_configuration_key(true, manifest));
        expect(p::xmake_configuration_key(true, manifest, conf) != p::xmake_configuration_key(true, manifest, std::pair<std::uint64_t, std::int64_t> { 300, 6000 }))
            << "a new `xmake f` configures again";
        expect(p::xmake_configuration_key(true, manifest, conf) != p::xmake_configuration_key(true, manifest, std::pair<std::uint64_t, std::int64_t> { 301, 5000 }));
        expect(p::xmake_configuration_key(true, manifest, conf) == p::xmake_configuration_key(true, manifest, conf));
        expect(p::xmake_configuration_key(true, manifest) == p::xmake_configuration_key(true, manifest, std::nullopt));

        const std::array<std::int64_t, 2> older { 100, 200 };
        const std::array<std::int64_t, 2> newer { 100, 500 };
        expect(!p::xmake_commands_out_of_date(300, older));
        expect(p::xmake_commands_out_of_date(300, newer)) << "any input newer than the file";
        expect(!p::xmake_commands_out_of_date(300, std::array<std::int64_t, 1> { 300 })) << "the same time is not older";
        expect(!p::xmake_commands_out_of_date(300, std::span<const std::int64_t> {}));
    };

    "X-1: xmake's own file is read only when xmake cannot run, and says when it is older than xmake.lua"_test = [] {
        const std::string root { make_root("xmake-fallback") };
        write(root, "src/main.cpp", "int main() { return 0; }\n");
        write(root, "compile_commands.json",
              nlohmann::json::array({ nlohmann::json { { "directory", root }, { "file", b::join_path(root, "src/main.cpp") },
                                                       { "arguments", nlohmann::json::array({ "g++", "-c", b::join_path(root, "src/main.cpp") }) } } })
                  .dump());
        std::this_thread::sleep_for(std::chrono::milliseconds { 30 });
        write(root, "xmake.lua", "target(\"x\")\n");   // written after the file: the file is older
        const p::XmakeProvider provider;
        const auto claim = provider.detect(root);
        expect(fatal(claim.has_value() && !claim->compileCommands.empty()));
        p::ProviderContext context;
        context.scanner = p::file_scanner();
        context.prober = [](std::string_view, std::span<const std::string>) -> std::optional<mcppls::toolchain::ToolchainFacts> { return std::nullopt; };

        // Untrusted, or the build tool off: the file is the only source, watched, with one notice.
        for (const bool trusted : { false, true }) {
            context.trusted = trusted;
            context.runBuildTool = false;
            const auto answer = provider.existing(*claim, context);
            expect(fatal(answer.has_value() && answer->database.has_value()));
            expect(std::ranges::count_if(answer->database->notices, [](const auto& notice) { return notice.first == "xmake-commands-stale"; }) == 1);
            expect(std::ranges::find(answer->database->watch, claim->compileCommands) != answer->database->watch.end()) << "watched in the fallback only";
        }
        const auto watched = provider.watch_inputs(*claim);
        expect(std::ranges::find(watched, claim->compileCommands) == watched.end());
        expect(std::ranges::find(watched, std::string { ".xmake/*/*/xmake.conf" }) != watched.end());

        // A file newer than xmake.lua has nothing to say.
        std::this_thread::sleep_for(std::chrono::milliseconds { 30 });
        write(root, "compile_commands.json", fs::read_file(claim->compileCommands).value());
        context.trusted = false;
        const auto fresh = provider.existing(*claim, context);
        expect(fatal(fresh.has_value() && fresh->database.has_value()));
        expect(fresh->database->notices.empty());
        fs::remove_all(root);
    };

    "B-5: xmake detect() and existing() read what is already there"_test = [] {
        const std::string root { make_root("xmake") };
        write(root, "xmake.lua", "target(\"hello\")\n    set_kind(\"binary\")\n    add_files(\"src/*.cpp\")\n");
        write(root, "src/main.cpp", "import std;\nint main() { return 0; }\n");
        const p::XmakeProvider provider;
        const auto claim = provider.detect(root);
        expect(fatal(claim.has_value()));
        expect(claim->manifest == b::join_path(root, "xmake.lua"));
        expect(claim->compileCommands.empty()) << "nothing generated yet";

        write(root, ".vscode/compile_commands.json",
              nlohmann::json::array({ nlohmann::json { { "directory", root }, { "file", b::join_path(root, "src/main.cpp") },
                                                       { "arguments", nlohmann::json::array({ "g++", "-std=c++23", "-c",
                                                                                              b::join_path(root, "src/main.cpp") }) } } })
                  .dump());
        const auto claimAfter = provider.detect(root);
        expect(fatal(claimAfter.has_value()));
        expect(claimAfter->compileCommands == b::join_path(root, ".vscode/compile_commands.json")) << "xmake's own VS Code plugin writes here";

        p::ProviderContext context;
        context.scanner = p::file_scanner();
        context.prober = [](std::string_view, std::span<const std::string>) -> std::optional<mcppls::toolchain::ToolchainFacts> { return std::nullopt; };
        const auto answer = provider.existing(*claimAfter, context);
        expect(fatal(answer.has_value()));
        expect(answer->outcome == p::Outcome::ok);
        expect(fatal(answer->database.has_value()));
        expect(answer->database->database.generator.value_or(s::Generator {}).name == "xmake");
        fs::remove_all(root);
    };

    "B-6: meson's setup arguments and classification"_test = [] {
        expect(p::meson_setup_arguments("/cache/meson", true, false) == (std::vector<std::string> { "setup", "--wrap-mode=nodownload", "/cache/meson" }));
        expect(p::meson_setup_arguments("/cache/meson", false, false) == (std::vector<std::string> { "setup", "/cache/meson" }));
        expect(p::meson_setup_arguments("/cache/meson", true, true)
               == (std::vector<std::string> { "setup", "--reconfigure", "--wrap-mode=nodownload", "/cache/meson" }))
            << "a second describe() into the same private directory reconfigures instead";

        expect(p::meson_missing_subprojects("Automatic wrap-based subproject downloading is disabled\nSubproject 'fmt' is buried here\n")
               == std::vector<std::string> { "fmt" });
        expect(p::meson_missing_subprojects("ninja: build stopped: subcommand failed.\n").empty()) << "a different failure names nothing";
    };

    "B-6: meson detect() finds an existing configured build directory"_test = [] {
        const std::string root { make_root("meson") };
        write(root, "meson.build", "project('hello', 'cpp')\n");
        write(root, "builddir/meson-private/coredata.dat", "");
        write(root, "builddir/compile_commands.json", "[]");
        const p::MesonProvider provider;
        const auto claim = provider.detect(root);
        expect(fatal(claim.has_value()));
        expect(claim->buildDirectory == b::join_path(root, "builddir"));
        expect(claim->compileCommands == b::join_path(root, "builddir/compile_commands.json"));
        fs::remove_all(root);
    };

    "a real CMake 4.4.2 build database becomes a level 2 model"_test = [] {
        // Recorded 2026-09-14 from `cmake -G Ninja -DCMAKE_CXX_COMPILER=clang++
        // -DCMAKE_EXPERIMENTAL_EXPORT_BUILD_DATABASE=70ef007e-b743-492d-9407-e35eeac03a40
        // -DCMAKE_EXPORT_BUILD_DATABASE=ON` then `cmake --build build --target
        // build_database.json` (CMake 4.4.2, Clang 22.1.8, Ninja) against the
        // same shapes/circle/app project as conformance/fixtures/cmake-clang,
        // trimmed of nothing but its absolute paths. CMake's own document has
        // no "ide" object anywhere (unlike the synthetic one above), so every
        // role below comes from scanning; "provides"/"requires" are kept
        // exactly as CMake wrote them, empty ones included.
        const std::string root { make_root("cmake-bdb") };
        write(root, "CMakeLists.txt", "project(shapes)\n");
        write(root, "build/CMakeCache.txt", "");
        write(root, "src/shapes.cppm", "export module shapes;\nexport import :circle;\n\nexport namespace shapes {\nint square(int side) { return side * side; }\n}\n");
        write(root, "src/circle.cppm", "export module shapes:circle;\n\nexport namespace shapes {\ndouble circle_area(double radius) { return 3.0 * radius * radius; }\n}\n");
        write(root, "src/main.cpp", "import shapes;\n\nint main() {\n    return shapes::square(2) + static_cast<int>(shapes::circle_area(1.0));\n}\n");
        const std::string build { b::join_path(root, "build") };
        constexpr std::string_view TEMPLATE { R"json({
          "version": 1, "revision": 0,
          "sets": [
            { "name": "shapes@", "family-name": "shapes", "visible-sets": [], "translation-units": [
                { "source": "@ROOT@/src/shapes.cppm", "work-directory": "@BUILD@",
                  "arguments": ["/opt/llvm/bin/clang++", "-std=c++20", "-c", "@ROOT@/src/shapes.cppm"],
                  "object": "CMakeFiles/shapes.dir/src/shapes.cppm.o", "private": false,
                  "provides": { "shapes": "@BUILD@/CMakeFiles/shapes.dir/shapes.pcm" }, "requires": ["shapes:circle"] },
                { "source": "@ROOT@/src/circle.cppm", "work-directory": "@BUILD@",
                  "arguments": ["/opt/llvm/bin/clang++", "-std=c++20", "-c", "@ROOT@/src/circle.cppm"],
                  "object": "CMakeFiles/shapes.dir/src/circle.cppm.o", "private": false,
                  "provides": { "shapes:circle": "@BUILD@/CMakeFiles/shapes.dir/shapes-circle.pcm" }, "requires": [] }
            ] },
            { "name": "app@", "family-name": "app", "visible-sets": ["shapes@"], "translation-units": [
                { "source": "@ROOT@/src/main.cpp", "work-directory": "@BUILD@",
                  "arguments": ["/opt/llvm/bin/clang++", "-std=c++20", "-c", "@ROOT@/src/main.cpp"],
                  "object": "CMakeFiles/app.dir/src/main.cpp.o", "private": true,
                  "provides": {}, "requires": ["shapes"] }
            ] }
          ]
        })json" };
        write(root, "build/build_database.json", b::replace_all(b::replace_all(TEMPLATE, "@ROOT@", root), "@BUILD@", build));

        // End to end: a CMakeLists.txt with a build/ that has both
        // CMakeCache.txt and build_database.json is a CMake project (not a
        // bare S1 override), and its level is the fixed 2 the design gives
        // every CMake source (project/model.cpp), not whatever
        // conformance_level would compute from the document alone.
        // trusted: this is about reading an already-generated build_database.json, not about trust
        // (real-project plan RP3.1 makes an untrusted workspace inferred regardless of what is on disk).
        p::LoadOptions options;
        options.trusted = true;
        options.cacheDirectory = b::join_path(root, ".cache-dir");
        const auto model = p::load_project(root, options);
        expect(model.source == p::SourceKind::cmake);
        expect(model.level == 2_i);
        expect(fatal(model.database.sets.size() == 2u));
        const auto& shapesSet = model.database.sets[0];
        const auto& appSet = model.database.sets[1];
        expect(shapesSet.name == "shapes@");
        expect(fatal(shapesSet.units.size() == 2u));
        expect(shapesSet.units[0].role == std::optional<s::Role> { s::Role::module_interface }) << "scanned: no \"ide\" object anywhere";
        expect(shapesSet.units[0].requiredModules == std::vector<std::string> { "shapes:circle" }) << "kept as CMake wrote it";
        expect(shapesSet.units[1].role == std::optional<s::Role> { s::Role::module_partition_interface }) << "scanned";
        expect(appSet.visibleSets == std::vector<std::string> { "shapes@" });
        expect(fatal(appSet.units.size() == 1u));
        expect(appSet.units[0].role == std::optional<s::Role> { s::Role::non_module }) << "scanned";
        expect(appSet.units[0].requiredModules == std::vector<std::string> { "shapes" }) << "kept as CMake wrote it";

        // The same document, read the way load_cmake reads it (spec::load_database
        // then enrich_database) with a fake compiler standing in for probing:
        // toolchain facts come from the first unit of each set, one probe per set.
        auto database = s::load_database(b::join_path(build, "build_database.json"));
        expect(fatal(database.has_value()));
        int probes { 0 };
        const p::Prober prober = [&](std::string_view driver, std::span<const std::string>) -> std::optional<mcppls::toolchain::ToolchainFacts> {
            ++probes;
            mcppls::toolchain::ToolchainFacts facts;
            facts.toolchain.family = s::Family::clang;
            facts.toolchain.version = "22.1.8";
            facts.toolchain.driver = std::string { driver };
            facts.toolchain.target = "x86_64-linux-gnu";
            return facts;
        };
        const auto enriched = p::enrich_database(std::move(*database), p::file_scanner(), prober);
        expect(probes == 2_i) << "one probe per set, shapes@ and app@";
        expect(fatal(enriched.facts.size() == 1u)) << "both sets share the same clang++ driver, so the same toolchain id";
        expect(enriched.database.sets[0].toolchain == "clang-22.1.8-x86_64-linux-gnu");
        expect(enriched.database.sets[1].toolchain == enriched.database.sets[0].toolchain);
        fs::remove_all(root);
    };

    "mcpp manifests"_test = [] {
        expect(p::mcpp_package_name("# c\n[package]\nname        = \"mcppls\"\nversion = \"0.1.0\"\n[dependencies]\nname = \"x\"\n") == "mcppls");
        expect(p::mcpp_package_name("[dependencies]\nname = \"x\"\n").empty());
    };

    "a package's std comes from mcpp's std build record"_test = [] {
        const std::string root { make_root("mcpp-std") };
        const std::string build { b::join_path(root, "target/x86_64-linux-gnu/3ea09e2fa2b8f653") };
        const std::string cache { b::join_path(root, "build-cache/v1/std/32dc1b80167fa105") };
        const std::string runtime { b::join_path(root, "xpkgs/openkal-llvm-runtime-0.9.6") };
        write(build, "build.ninja", std::format("cxxflags = -std=c++23\nbuild pcm.cache/std.pcm : stage_file {0}/pcm.cache/std.pcm\n"
                                                "build pcm.cache/std.compat.pcm : stage_file {0}/pcm.cache/std.compat.pcm | pcm.cache/std.pcm\n", cache));
        nlohmann::json record {
            { "schema", 1 },
            { "std_module_source", runtime + "/llvm-generated/std.cppm" },
            { "std_compat_source", runtime + "/llvm-generated/std.compat.cppm" },
            { "std_build_commands", nlohmann::json::array({
                std::format("cd '{0}' && env LD_LIBRARY_PATH='/llvm/lib' '/llvm/bin/clang++' -std=c++23 -Wno-reserved-module-identifier '-nostdinc++' "
                            "-isystem '{1}/llvm/libcxx/include' '-D_XOPEN_SOURCE=700' --precompile '{1}/llvm-generated/std.cppm' -o 'pcm.cache/std.pcm' 2>&1", cache, runtime),
                std::format("cd '{0}' && env LD_LIBRARY_PATH='/llvm/lib' '/llvm/bin/clang++' -std=c++23 'pcm.cache/std.pcm' -c -o std.o 2>&1", cache) }) },
            { "std_compat_build_commands", nlohmann::json::array({
                std::format("env LD_LIBRARY_PATH='/llvm/lib' '/llvm/bin/clang++' -std=c++23 -fmodule-file=std={0}/pcm.cache/std.pcm "
                            "--precompile '{1}/llvm-generated/std.compat.cppm' -o '{0}/pcm.cache/std.compat.pcm' 2>&1", cache, runtime) }) },
        };
        write(cache, "std-module.json", record.dump(2));
        p::CompileCommand user;
        user.directory = root;
        user.file = b::join_path(root, "src/main.cpp");
        user.arguments = { "/llvm/bin/clang++", "-std=c++23", std::format("-fmodule-file=std={}/pcm.cache/std.pcm", build), "-c", user.file };
        const std::vector<p::CompileCommand> commands { user };
        const auto units = p::mcpp_standard_units(commands);
        expect(fatal(units.size() == 2u));
        expect(units[0].file == runtime + "/llvm-generated/std.cppm");
        expect(units[0].directory == cache);
        expect(units[0].arguments == std::vector<std::string> { "/llvm/bin/clang++", "-std=c++23", "-Wno-reserved-module-identifier", "-nostdinc++", "-isystem",
                                                                runtime + "/llvm/libcxx/include", "-D_XOPEN_SOURCE=700", runtime + "/llvm-generated/std.cppm" })
            << b::join(units[0].arguments, " ");
        expect(units[1].file == runtime + "/llvm-generated/std.compat.cppm");
        expect(units[1].directory == cache) << "a command without cd runs where the record is";
        expect(std::ranges::none_of(units[1].arguments, [](const std::string& word) { return word.starts_with("-fmodule-file=") || word == "-o"; }));

        user.arguments = { "/llvm/bin/clang++", "-std=c++23", "-c", user.file };
        expect(p::mcpp_standard_units(std::vector<p::CompileCommand> { user }).empty()) << "no staged std BMI, nothing to find";
        fs::remove_all(root);
    };

    "a Visual Studio without the std module is named in a notice"_test = [] {
        const std::string root { make_root("visual-studio") };
        mcppls::toolchain::ToolchainFacts facts;
        facts.toolchain.family = s::Family::msvc;
        facts.toolchain.version = "19.29.30133";
        facts.msvc = mcppls::toolchain::MsvcEnvironment { b::join_path(root, "MSVC/14.29.30133"), "14.29.30133", "", "" };
        facts.toolchain.stdlib = s::Stdlib { "msvc-stl", "14.29.30133", "" };
        const auto notice = p::visual_studio_notice(facts);
        expect(fatal(notice.has_value()));
        expect(notice->code == "msvc-without-std-module" && notice->message.contains("14.29.30133")) << notice->message;

        write(root, "MSVC/14.44.35207/modules/modules.json", R"({"version": 1, "revision": 0, "library": "microsoft/STL", "module-sources": ["std.ixx"]})");
        facts.msvc->toolsVersion = "14.44.35207";
        facts.toolchain.stdlib = s::Stdlib { "msvc-stl", "14.44.35207", b::join_path(root, "MSVC/14.44.35207/modules/modules.json") };
        expect(!p::visual_studio_notice(facts).has_value()) << "a toolset with the std module is used, not explained";
        facts.msvc.reset();
        expect(!p::visual_studio_notice(facts).has_value()) << "no Visual Studio, nothing to say";
        fs::remove_all(root);
    };

    "workspace keys are stable and distinct"_test = [] {
        expect(p::workspace_key("/home/u/project") == p::workspace_key("/home/u/project/"));
        expect(p::workspace_key("/home/u/project") != p::workspace_key("/home/u/project2"));
        expect(p::workspace_key("/home/u/project").starts_with("project-"));
    };

    // Design 4.1. The cache is a model, not a warm index: what goes in comes out, so a session can
    // plan with it before the producer has said anything.
    "a cached model round-trips whole"_test = [] {
        p::ProjectModel model;
        model.root = "/w";
        model.source = p::SourceKind::mcpp;
        model.detected = p::SourceKind::mcpp;
        model.level = 3;
        model.usesKit = true;
        model.watch = { "mcpp.toml", "mcpp.lock" };
        model.issues = { p::ModelIssue { "some-code", "some message" } };
        model.notices = { p::ModelIssue { "a-notice", "worth knowing" } };
        model.profile = p::SemanticProfile { "build-toolchain", "gcc 16.1.0", "libstdc++ 16.1.0", "x86_64-linux-gnu" };
        model.database.sets.push_back(s::Set { .name = "app" });
        const auto restored = p::model_from_json(p::model_to_json(model));
        expect(fatal(restored.has_value())) << (restored ? "" : restored.error().message);
        expect(restored->source == p::SourceKind::mcpp && restored->detected == p::SourceKind::mcpp);
        expect(restored->level == 3 && restored->usesKit);
        expect(restored->watch == model.watch);
        expect(restored->issues.size() == 1u && restored->issues.front().code == "some-code");
        expect(restored->notices.size() == 1u && restored->notices.front().message == "worth knowing");
        expect(restored->profile.compiler == "gcc 16.1.0" && restored->profile.target == "x86_64-linux-gnu");
        expect(restored->database.sets.size() == 1u && restored->database.sets.front().name == "app");
    };

    // One file per source, so scanned sources can never replace what the build tool said (P5).
    "each source has its own cache file, and a worse one does not overwrite a better"_test = [] {
        const std::string directory { make_root("modelcache") };
        expect(p::cache_file_name(p::SourceKind::mcpp) == "model.mcpp.json");
        expect(p::cache_file_name(p::SourceKind::inferred) == "model.inferred.json");

        p::CachedModel fromMcpp;
        fromMcpp.model.source = p::SourceKind::mcpp;
        fromMcpp.model.level = 3;
        fromMcpp.fingerprint = "abc";
        fromMcpp.producer = "/usr/bin/mcpp";
        fromMcpp.producerVersion = "2026.9.16.1";
        expect(fatal(p::save_model(directory, fromMcpp).has_value()));

        p::CachedModel inferred;
        inferred.model.source = p::SourceKind::inferred;
        inferred.model.level = 2;
        inferred.fingerprint = "xyz";
        expect(fatal(p::save_model(directory, inferred).has_value()));

        auto readBack = p::load_model(directory, p::SourceKind::mcpp);
        expect(fatal(readBack.has_value()));
        expect(readBack->model.level == 3) << readBack->model.level;
        expect(readBack->fingerprint == "abc" && readBack->producerVersion == "2026.9.16.1");
        expect(p::load_model(directory, p::SourceKind::cmake) == std::nullopt);
        fs::remove_all(directory);
    };

    "the fingerprint follows the inputs, the producer and its version"_test = [] {
        const std::string root { make_root("fingerprint") };
        const std::string manifest { b::join_path(root, "mcpp.toml") };
        expect(fatal(fs::write_file(manifest, R"([package]
name = "x"
)").has_value()));
        const std::vector<std::string> watch { "mcpp.toml" };
        const std::string first { p::inputs_fingerprint(root, watch, manifest, "/usr/bin/mcpp", "2026.9.16.1") };
        expect(first == p::inputs_fingerprint(root, watch, manifest, "/usr/bin/mcpp", "2026.9.16.1")) << "the same inputs give the same answer";
        expect(first != p::inputs_fingerprint(root, watch, manifest, "/usr/bin/mcpp", "2026.9.15.1")) << "another producer version is another model";
        expect(first != p::inputs_fingerprint(root, watch, manifest, "/opt/mcpp", "2026.9.16.1")) << "another producer is another model";
        // A changed manifest is a changed fingerprint: size and modification time both move here.
        std::this_thread::sleep_for(std::chrono::milliseconds { 20 });
        expect(fatal(fs::write_file(manifest, R"([package]
name = "x"
version = "1"
)").has_value()));
        expect(first != p::inputs_fingerprint(root, watch, manifest, "/usr/bin/mcpp", "2026.9.16.1")) << "an edited manifest is a new fingerprint";
        fs::remove_all(root);
    };

    // The incident: mcppls's own project.level (S1's document-conformance number) and the README's
    // L1..L4 (which *kind* of source it is) share a range and are not the same question -- "mcpp
    // L3" and "inferred L2" both sounded like the good and bad case respectively, which is not what
    // either number says. `tier` is the second one; a level-1 mcpp database is still tier 1.
    "tier follows the model's source, independent of level"_test = [] {
        expect(p::tier_of(p::SourceKind::build_database) == 1_i);
        expect(p::tier_of(p::SourceKind::mcpp) == 1_i);
        expect(p::tier_of(p::SourceKind::cmake) == 2_i);
        expect(p::tier_of(p::SourceKind::compile_commands) == 3_i);
        expect(p::tier_of(p::SourceKind::inferred) == 4_i);

        const std::string root { make_root("tier") };
        write(root, "src/a.cppm", "export module a;\n");
        write(root, "compile_commands.json", nlohmann::json::array({ nlohmann::json {
            { "directory", root }, { "file", b::join_path(root, "src/a.cppm") },
            { "arguments", nlohmann::json::array({ "clang++", "-std=c++23", "-c", b::join_path(root, "src/a.cppm") }) },
        } }).dump());
        p::LoadOptions options;
        options.trusted = true;
        const auto model = p::load_project(root, options);
        expect(model.source == p::SourceKind::compile_commands);
        expect(model.tier == 3_i) << "tier 3 whatever `level` this bare document happens to compute to";
        fs::remove_all(root);
    };

    // real-project plan RP3.1: an untrusted workspace is L4 by definition. A `compile_commands.json` or
    // `target/` a *trusted* session (or a build run outside mcppls entirely) left on disk is still a
    // fact about the build, and reading it just because it is sitting there would make "untrusted"
    // mean something less than what the docs promise (docs/20-projects.md, design 2.1).
    "an untrusted workspace ignores a stale build description already on disk"_test = [] {
        const std::string root { make_root("untrusted-stale") };
        write_fixture(root);
        write(root, "mcpp.toml", "[package]\nname = \"hello\"\n");
        // A leftover compile_commands.json from a trusted run or another tool: it exists, is valid
        // JSON, and names a real file -- nothing about it looks "broken" except that this session
        // never trusted this workspace and so never should have read it.
        write(root, "compile_commands.json", nlohmann::json::array({ nlohmann::json {
            { "directory", root }, { "file", b::join_path(root, "src/main.cpp") },
            { "arguments", nlohmann::json::array({ "clang++", "-std=c++23", "-c", b::join_path(root, "src/main.cpp") }) },
        } }).dump());
        p::LoadOptions options;
        options.trusted = false;
        const auto model = p::load_project(root, options);
        expect(model.source == p::SourceKind::inferred);
        expect(model.tier == 4_i);
        expect(model.detected == p::SourceKind::mcpp);
        fs::remove_all(root);
    };

    // real-project plan RP2.4: a database entry naming a file that no longer exists (a deleted `target/`, a
    // `compile_commands.json` checked in from another machine) is used for what remains, not
    // discarded wholesale or left to fail some other way later.
    "a stale database entry is dropped and noted; the rest of the model is used"_test = [] {
        const std::string root { make_root("stale") };
        write(root, "src/a.cppm", "export module a;\n");
        write(root, "compile_commands.json", nlohmann::json::array({
            nlohmann::json { { "directory", root }, { "file", b::join_path(root, "src/a.cppm") },
                             { "arguments", nlohmann::json::array({ "clang++", "-std=c++23", "-c", b::join_path(root, "src/a.cppm") }) } },
            nlohmann::json { { "directory", root }, { "file", b::join_path(root, "src/deleted.cppm") },
                             { "arguments", nlohmann::json::array({ "clang++", "-std=c++23", "-c", b::join_path(root, "src/deleted.cppm") }) } },
        }).dump());
        p::LoadOptions options;
        options.trusted = true;
        const auto model = p::load_project(root, options);
        expect(model.source == p::SourceKind::compile_commands) << "some units still exist; not treated as fully stale";
        expect(fatal(model.database.sets.size() == 1u));
        expect(model.database.sets.front().units.size() == 1u) << "the entry naming a deleted file is dropped";
        expect(std::ranges::any_of(model.notices, [](const p::ModelIssue& n) { return n.code == "stale-database"; }));
        fs::remove_all(root);
    };

    "a database whose every entry is stale is treated as if it did not exist"_test = [] {
        const std::string root { make_root("fully-stale") };
        write(root, "compile_commands.json", nlohmann::json::array({
            nlohmann::json { { "directory", root }, { "file", b::join_path(root, "src/deleted.cppm") },
                             { "arguments", nlohmann::json::array({ "clang++", "-std=c++23", "-c", b::join_path(root, "src/deleted.cppm") }) } },
        }).dump());
        write(root, "src/live.cppm", "export module live;\n");
        p::LoadOptions options;
        options.trusted = true;
        const auto model = p::load_project(root, options);
        expect(model.source == p::SourceKind::inferred) << "inference finds src/live.cppm instead";
        fs::remove_all(root);
    };

    // real-project plan RP2.3: before a stand-in is created for a module nothing provides, mcppls looks for
    // where a build leaves what it generated. The incident this is for: an old mcpp's fallback
    // `compile_commands.json` outlives the `target/` a later `mcpp build` deletes and recreates, but
    // the generated file mcpp already built once is still in its own build-database cache.
    "find_generated_source verifies the module rather than taking the first file"_test = [] {
        const std::string root { make_root("recover-direct") };
        write(root, "target/.build-mcpp/deps/dep@1.0.0/out/other.cppm", "export module dep.other;\n");
        write(root, "target/.build-mcpp/deps/dep@1.0.0/out/thing.cppm", "export module dep.thing;\n");
        const p::GeneratedSourceOptions options { root, make_root("empty-home"), p::file_scanner() };
        const auto found = p::find_generated_source("dep.thing", options);
        expect(fatal(found.has_value()));
        expect(b::file_name(*found) == "thing.cppm");
        expect(!p::find_generated_source("dep.nothing", options).has_value()) << "nothing here declares it";
        fs::remove_all(root);
    };

    "find_generated_source takes the most recently built of several cached versions"_test = [] {
        const std::string root { make_root("recover-newest") };
        const std::string home { make_root("recover-newest-home") };
        const std::string cache { ".mcpp/cache/build-database" };
        write(home, cache + "/aaa/target/.build-mcpp/deps/dep@1.0.9/out/thing.cppm", "export module dep.thing; // older\n");
        write(home, cache + "/bbb/target/.build-mcpp/deps/dep@1.0.10/out/thing.cppm", "export module dep.thing; // newer\n");
        // The older build is listed first ("aaa" < "bbb"): it must not win by listing order.
        std::filesystem::last_write_time(b::join_path(home, cache + "/aaa/target/.build-mcpp/deps/dep@1.0.9/out/thing.cppm"),
                                         std::filesystem::file_time_type::clock::now() - std::chrono::hours { 1 });
        const auto found = p::find_generated_source("dep.thing", p::GeneratedSourceOptions { root, home, p::file_scanner() });
        expect(fatal(found.has_value()));
        expect(found->contains("dep@1.0.10")) << *found;
        fs::remove_all(root);
        fs::remove_all(home);
    };

    "a producer's planning directory is told from any other path"_test = [] {
        expect(p::private_target_relative("/h/.mcpp/cache/build-database/81d15f0e10d7a75b/target/.build-mcpp/out/qt") == std::optional<std::string> { ".build-mcpp/out/qt" });
        expect(p::private_target_relative("/h/.mcpp/cache/build-database/k/target/x86_64-linux-gnu/f/obj/a.o") == std::optional<std::string> { "x86_64-linux-gnu/f/obj/a.o" });
        expect(!p::private_target_relative("/p/qt-demo/target/.build-mcpp/out/qt").has_value()) << "the project's own build directory";
        expect(!p::private_target_relative("/h/.mcpp/cache/build-database/k/src/x.cpp").has_value()) << "not under the planning target";
    };

    "generated build output a producer only names is read from the project's own build (qt-demo)"_test = [] {
        const std::string root { make_root("generated-output") };
        const std::string home { make_root("generated-output-home") };
        // What mcpp's emit describes for a rules-qt project: an include directory in its planning directory
        // holding only empty placeholders, and a moc source that is one of them.
        const std::string planning { b::join_path(home, ".mcpp/cache/build-database/81d15f0e10d7a75b") };
        write(planning, "target/.build-mcpp/out/qt/moc_counter.cpp", "");
        write(planning, "target/.build-mcpp/out/qt/qrc_demo.cpp", "");
        const std::string privateOut { b::join_path(planning, "target/.build-mcpp/out/qt") };
        auto database_of = [&] {
            s::Database database;
            s::Set set;
            set.name = "qt-demo";
            set.baselineArguments = { "-I" + privateOut, "-std=c++23" };
            s::TranslationUnit main;
            main.source = b::join_path(root, "src/main.cpp");
            main.arguments = { "g++", "-I" + privateOut, "-std=c++23", "-c", main.source };
            s::TranslationUnit moc;
            moc.source = b::join_path(privateOut, "moc_counter.cpp");
            moc.arguments = { "g++", "-I", privateOut, "-c", moc.source };
            set.units = { main, moc };
            database.sets.push_back(set);
            return database;
        };

        // Never built: nothing to read instead; what is missing is reported, with where a build writes it.
        auto unbuilt = database_of();
        const auto before = p::use_project_build_output(unbuilt, root);
        expect(before.relocated.empty());
        expect(std::ranges::find(before.missing, privateOut) != before.missing.end());
        expect(std::ranges::find(before.watch, std::string { "target/.build-mcpp/out/qt/*" }) != before.watch.end());
        expect(unbuilt.sets[0].units[0].arguments[1] == "-I" + privateOut) << "left as it is";

        // Built once: the project's own output replaces the planning directory's, arguments and source alike.
        write(root, "target/.build-mcpp/out/qt/ui_mainwindow.h", "namespace Ui { class MainWindow {}; }\n");
        write(root, "target/.build-mcpp/out/qt/moc_counter.cpp", "// moc output\n");
        const std::string projectOut { b::join_path(root, "target/.build-mcpp/out/qt") };
        auto built = database_of();
        const auto after = p::use_project_build_output(built, root);
        expect(after.missing.empty());
        expect(built.sets[0].baselineArguments[0] == "-I" + projectOut);
        expect(built.sets[0].units[0].arguments[1] == "-I" + projectOut);
        expect(built.sets[0].units[1].arguments[2] == projectOut) << "the separate spelling too";
        expect(built.sets[0].units[1].source == b::join_path(projectOut, "moc_counter.cpp"));
        expect(built.sets[0].units[1].arguments.back() == b::join_path(projectOut, "moc_counter.cpp"));
        fs::remove_all(root);
        fs::remove_all(home);
    };

    "a rule's inputs are not translation units, whatever command a producer gives them"_test = [] {
        const std::vector<std::string> gcc { "g++", "-c" };
        expect(!p::compiled_by_c_family("/p/ui/mainwindow.ui", gcc));
        expect(!p::compiled_by_c_family("/p/res/demo.qrc", gcc));
        expect(!p::compiled_by_c_family("/p/i18n/qt_demo_zh_CN.ts", gcc));
        expect(p::compiled_by_c_family("/p/src/main.cpp", gcc));
        expect(p::compiled_by_c_family("/p/src/a.c", gcc));
        expect(p::compiled_by_c_family("/p/src/m.ixx", gcc));
        expect(p::compiled_by_c_family("/p/src/k.cu", gcc));
        expect(p::compiled_by_c_family("/p/src/generated.inl", std::vector<std::string> { "clang++", "-x", "c++", "-c" })) << "a language forced on it";
        expect(p::compiled_by_c_family("/p/src/generated.inl", std::vector<std::string> { "cl.exe", "/Tp/p/src/generated.inl" }));
    };

    "a generated module's real source is recovered before a stand-in is needed"_test = [] {
        const std::string root { make_root("generated") };
        const std::string home { make_root("generated-home") };
        write(root, "src/main.cpp", "import dep.thing;\n\nint main() { return 0; }\n");
        write(root, "compile_commands.json", nlohmann::json::array({ nlohmann::json {
            { "directory", root }, { "file", b::join_path(root, "src/main.cpp") },
            { "arguments", nlohmann::json::array({ "clang++", "-std=c++23", "-c", b::join_path(root, "src/main.cpp") }) },
        } }).dump());
        // Not where the database says it is (that `target/` is gone); where mcpp's own cache keeps
        // what it once built, keyed by a hash mcppls does not need to know or reproduce.
        write(home, ".mcpp/cache/build-database/deadbeef/target/.build-mcpp/deps/dep@1.0.0/out/thing.cppm", "export module dep.thing;\n");
        p::LoadOptions options;
        options.trusted = true;
        options.homeDirectory = home;
        const auto model = p::load_project(root, options);
        expect(fatal(model.database.sets.size() == 1u));
        const auto& units = model.database.sets.front().units;
        const auto recovered = std::ranges::find_if(units, [](const s::TranslationUnit& unit) {
            return !unit.providedModules.empty() && unit.providedModules.front().first == "dep.thing";
        });
        expect(fatal(recovered != units.end())) << "the recovered unit joined the model instead of a stand-in";
        expect(b::file_name(recovered->source) == "thing.cppm");
        expect(std::ranges::any_of(model.notices, [](const p::ModelIssue& n) { return n.code == "generated-module-recovered"; }));
        fs::remove_all(root);
        fs::remove_all(home);
    };

    // real-project plan RP2.1 (producer negotiation): comparing mcpp's date-based versions numerically, not
    // lexically -- "2026.9.9" sorts after "2026.9.10" as plain strings, which is backwards.
    "mcpp version comparison is numeric, not lexical"_test = [] {
        expect(p::mcpp_version_less("2026.8.8.4", "2026.9.21.3"));
        expect(!p::mcpp_version_less("2026.9.21.3", "2026.8.8.4"));
        expect(p::mcpp_version_less("2026.9.9", "2026.9.10")) << "a plain string compare gets this one backwards";
        expect(!p::mcpp_version_less("2026.9.10", "2026.9.9"));
        expect(!p::mcpp_version_less("2026.9.15.1", "2026.9.15.1")) << "equal versions: neither is less";
        expect(!p::mcpp_version_less("", "")) << "unparsed versions never look newer than each other";
    };

    "other mcpp executables are found newest first, and the resolved one is excluded"_test = [] {
        const std::string home { make_root("mcpp-store") };
        for (std::string_view version : { "2026.8.8.4", "2026.9.21.3", "2026.9.9.1" }) {
            write(home, std::format(".xlings/data/xpkgs/xim-x-mcpp/{}/bin/mcpp{}", version, mcppls::os::EXECUTABLE_SUFFIX), "#!/bin/sh\n");
        }
        const std::string resolved { b::join_path(home, std::format(".xlings/data/xpkgs/xim-x-mcpp/2026.8.8.4/bin/mcpp{}", mcppls::os::EXECUTABLE_SUFFIX)) };
        const auto others = p::other_mcpp_executables(resolved, home);
        expect(fatal(others.size() == 2u)) << others.size();
        expect(others.front().contains("2026.9.21.3")) << others.front();
        expect(others.back().contains("2026.9.9.1"));
        expect(std::ranges::none_of(others, [&](const std::string& path) { return b::same_path(path, resolved); }));
        fs::remove_all(home);
    };

    // real-project plan RP3.4: a directory below the root with its own build manifest is a different project
    // (a conformance fixture, a vendored copy, an example), not more of this one's sources -- a
    // general rule the incident against mcppls's own repository is one instance of, not the reason
    // for it.
    "inferred scanning does not cross into a nested project's own manifest"_test = [] {
        const std::string root { make_root("nested") };
        write(root, "src/a.cppm", "export module a;\n");
        write(root, "vendor/example/mcpp.toml", "[package]\nname = \"example\"\n");
        write(root, "vendor/example/src/b.cppm", "export module b;\n");
        const auto inferred = p::infer_database(root, p::InferOptions {}, p::file_scanner());
        expect(fatal(inferred.database.sets.size() == 1u));
        const auto& units = inferred.database.sets.front().units;
        expect(units.size() == 1u) << "only src/a.cppm; vendor/example is its own project";
        expect(b::file_name(units.front().source) == "a.cppm");
        fs::remove_all(root);
    };

    // Fix plan F5: issue #23's fallback scan took 166 of GalTranslPP's 349 units from vcpkg_installed/, and its
    // modules showed up as ambiguous. What package managers install is their packages' sources, not the project's.
    "inferred scanning leaves out what package managers install, and vcpkg packages below the root"_test = [] {
        const std::string root { make_root("packages") };
        write(root, "vcpkg.json", "{ \"name\": \"app\" }\n");   // the project's own manifest: its sources stay
        write(root, "src/a.cppm", "export module a;\n");
        for (const std::string_view installed : { "vcpkg_installed/x64-windows/include/fmt/fmt.cppm", "vcpkg/ports/fmt/fmt.cppm",
                                                  ".conan2/p/fmt/fmt.cppm", ".conan/data/fmt/fmt.cppm", ".xmake/packages/fmt/fmt.cppm",
                                                  ".cache/fmt/fmt.cppm", ".git/fmt.cppm" }) {
            write(root, installed, "export module fmt;\n");
        }
        write(root, "overlays/zlib/vcpkg.json", "{ \"name\": \"zlib\" }\n");
        write(root, "overlays/zlib/src/deep/z.cppm", "export module z;\n");
        const auto inferred = p::infer_database(root, p::InferOptions {}, p::file_scanner());
        expect(fatal(inferred.database.sets.size() == 1u));
        std::vector<std::string> names;
        for (const auto& unit : inferred.database.sets.front().units) names.emplace_back(b::file_name(unit.source));
        expect(names == std::vector<std::string> { "a.cppm" }) << std::format("{}", names);
        fs::remove_all(root);
    };

    // real-project plan RP3.4: a nested manifest is a separate project only when it is not part of
    // the outer project's own build -- a CMake subdirectory of a CMake project, or a member of an
    // mcpp workspace, is not.
    "a CMake project's subdirectories and an mcpp workspace's members are not separate projects"_test = [] {
        const std::string cmake { make_root("boundary-cmake") };
        write(cmake, "CMakeLists.txt", "add_subdirectory(libs/core)\n");
        write(cmake, "libs/core/CMakeLists.txt", "add_library(core)\n");
        write(cmake, "libs/core/core.cppm", "export module core;\n");
        write(cmake, "tools/gen/compile_commands.json", "[]\n");
        write(cmake, "tools/gen/gen.cpp", "int main() {}\n");
        const p::ProjectBoundaries inCMake { cmake };
        expect(!inCMake.separate(b::join_path(cmake, "libs/core"))) << "add_subdirectory territory";
        expect(inCMake.separate(b::join_path(cmake, "tools/gen"))) << "a compile database of its own is always another project";

        const std::string workspace { make_root("boundary-mcpp") };
        write(workspace, "mcpp.toml", "[workspace]\nmembers = [\n    \"modules/base\",\n    \"tools/*\",\n]\n\n[package]\nname = \"root\"\n");
        write(workspace, "modules/base/mcpp.toml", "[package]\nname = \"base\"\n");
        write(workspace, "tools/devtools/mcpp.toml", "[package]\nname = \"devtools\"\n");
        write(workspace, "conformance/fixtures/x/mcpp.toml", "[package]\nname = \"x\"\n");
        write(workspace, "examples/cmake/CMakeLists.txt", "project(example)\n");
        const p::ProjectBoundaries inMcpp { workspace };
        expect(!inMcpp.separate(b::join_path(workspace, "modules/base"))) << "a listed member";
        expect(!inMcpp.separate(b::join_path(workspace, "tools/devtools"))) << "a member through tools/*";
        expect(inMcpp.separate(b::join_path(workspace, "conformance/fixtures/x"))) << "not a member";
        expect(inMcpp.separate(b::join_path(workspace, "examples/cmake"))) << "a CMake project inside an mcpp one";
        expect(p::workspace_members("[workspace]\nmembers = [\"a\", \"b/*\"]\n") == std::vector<std::string> { "a", "b/*" });
        expect(p::workspace_members("[package]\nname = \"x\"\n").empty());
        fs::remove_all(cmake);
        fs::remove_all(workspace);
    };

    // Plan 2026-09-27: one list of project manifests for every place that looks for a root, and a subdirectory's own
    // CMakeLists.txt, xmake.lua or meson.build belongs to the project above it.
    "the project root of a file is the outermost of a nesting build system's manifests"_test = [] {
        const std::string meson { make_root("root-meson") };
        write(meson, "meson.build", "project('p', 'cpp')\nsubdir('src')\n");
        write(meson, "src/meson.build", "executable('app', 'main.cpp')\n");
        write(meson, "src/main.cpp", "int main() {}\n");
        expect(p::enclosing_project_root(b::join_path(meson, "src")) == meson) << "src/meson.build is part of the project above";
        const std::string xmake { make_root("root-xmake") };
        write(xmake, "xmake.lua", "includes(\"src\")\n");
        write(xmake, "src/xmake.lua", "target(\"app\")\n");
        expect(p::enclosing_project_root(b::join_path(xmake, "src")) == xmake);
        const std::string cmake { make_root("root-cmake") };
        write(cmake, "CMakeLists.txt", "add_subdirectory(libs/core)\n");
        write(cmake, "libs/core/CMakeLists.txt", "add_library(core)\n");
        expect(p::enclosing_project_root(b::join_path(cmake, "libs/core")) == b::join_path(cmake, "libs/core"))
            << "libs/ has no CMakeLists.txt of its own: the chain is broken there, as it is for CMake itself";
        write(cmake, "libs/CMakeLists.txt", "add_subdirectory(core)\n");
        expect(p::enclosing_project_root(b::join_path(cmake, "libs/core")) == cmake);
        const std::string loose { make_root("root-none") };
        write(loose, "a/b.cpp", "int x;\n");
        expect(!p::find_project_root(b::join_path(loose, "a")).has_value() || *p::find_project_root(b::join_path(loose, "a")) != b::join_path(loose, "a"));
        expect(std::ranges::find(p::project_manifests(), std::string_view { "xmake.lua" }) != p::project_manifests().end());
        expect(p::nesting_manifest("meson.build") && !p::nesting_manifest("mcpp.toml"));
        const p::ProjectBoundaries inMeson { meson };
        expect(!inMeson.separate(b::join_path(meson, "src"))) << "a meson subdir of a meson project";
        const p::ProjectBoundaries inCMake { cmake };
        write(cmake, "third_party/lib/meson.build", "project('lib', 'cpp')\n");
        expect(inCMake.separate(b::join_path(cmake, "third_party/lib"))) << "a meson project inside a CMake one";
        fs::remove_all(meson);
        fs::remove_all(xmake);
        fs::remove_all(cmake);
        fs::remove_all(loose);
    };

    return report();

    "sources nothing describes are read with the newest standard their compiler takes (C++26 alignment)"_test = [] {
        namespace project = mcppls::project;
        expect(project::inferred_language_standard(std::nullopt) == "c++26") << "the semantic kit: clang 23 and libc++ 23";
        const auto facts = [](mcppls::spec::Family family, std::string version) {
            mcppls::toolchain::ToolchainFacts facts;
            facts.toolchain.family = family;
            facts.toolchain.version = std::move(version);
            return std::optional<mcppls::toolchain::ToolchainFacts> { std::move(facts) };
        };
        expect(project::inferred_language_standard(facts(mcppls::spec::Family::gcc, "16.1.0")) == "c++26");
        expect(project::inferred_language_standard(facts(mcppls::spec::Family::gcc, "13.3.0")) == "c++23");
        expect(project::inferred_language_standard(facts(mcppls::spec::Family::clang, "22.1.8")) == "c++26");
        expect(project::inferred_language_standard(facts(mcppls::spec::Family::clang, "18.1.3")) == "c++2c");
        expect(project::inferred_language_standard(facts(mcppls::spec::Family::clang, "16.0.6")) == "c++23");
    };
}
