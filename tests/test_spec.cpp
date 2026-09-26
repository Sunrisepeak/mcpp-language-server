// S1 database, P3286 metadata, S4 kits and S2 discovery.
import std;
import mcppls.testing;
import nlohmann.json;
import mcppls.base.path;
import mcppls.platform.fs;
import mcppls.platform.dirs;
import mcppls.spec.database;
import mcppls.spec.metadata;
import mcppls.spec.kit;
import mcppls.spec.discovery;
import mcppls.platform.env;
import mcppls.platform.stdio;


namespace fs = mcppls::platform::fs;
namespace base = mcppls::base;
using Json = nlohmann::json;

namespace {

std::string repository_root() {
    std::string directory { fs::current_directory() };
    while (true) {
        if (fs::is_regular_file(base::join_path(directory, "docs/specs/README.md"))) return directory;
        const std::string parent { base::parent_path(directory) };
        if (parent == directory) return fs::current_directory();
        directory = parent;
    }
}

std::string scratch(std::string_view name) {
    const std::string directory { base::join_path(mcppls::platform::dirs::temp_directory(),
        std::format("mcppls-test-spec-{}-{}", name, std::chrono::steady_clock::now().time_since_epoch().count())) };
    (void)fs::create_directories(directory);
    return directory;
}

// This program started again as an S2 producer, so discovery runs through a real child on every system.
int producer_main(std::string_view mode, std::string_view databasePath) {
    namespace stdio = mcppls::platform::stdio;
    std::string request;
    while (true) {
        auto chunk = stdio::read_input();
        if (!chunk || chunk->empty()) break;
        request += *chunk;
    }
    const Json parsed = Json::parse(request, nullptr, false);
    if (parsed.is_discarded() || !parsed.contains("workspace") || !parsed.contains("profile-version")) {
        (void)stdio::write_output(R"({"kind":"error","message":"the request is not an S2 request"})" "\n");
        return 1;
    }
    if (mode == "stream") {
        // Standard error is free text a consumer does not interpret, even when it looks like a message.
        (void)stdio::write_error(R"({"kind":"error","message":"not on standard output"})" "\n");
        (void)stdio::write_output(R"({"kind":"progress","message":"configuring"})" "\n");
        (void)stdio::write_output(R"({"kind":"vendor-note","detail":1})" "\n");
        (void)stdio::write_output(Json { { "kind", "finished" }, { "database", std::string { databasePath } }, { "watch", Json::array({ "mcpp.toml" }) },
                                         { "future-field", true } }.dump() + "\n");
        return 0;
    }
    if (mode == "truncated") {
        (void)stdio::write_output(R"({"kind":"progress","message":"configuring"})" "\n");
        return 0;
    }
    if (mode == "failing") {
        (void)stdio::write_output(R"({"kind":"error","message":"no toolchain","code":"toolchain"})" "\n");
        return 3;
    }
    if (mode == "sleeping") std::this_thread::sleep_for(std::chrono::seconds { 30 });
    return 0;
}

std::string self_path() {
    const auto arguments = mcppls::platform::env::arguments();
    std::string path { arguments.empty() ? std::string {} : arguments.front() };
    if (!base::is_absolute_path(path)) path = base::join_path(fs::current_directory(), path);
    return base::normalize_path(path);
}

} // namespace

int main(int argc, char* argv[]) {
    using namespace mcppls::testing;
    if (argc >= 3 && std::string_view { argv[1] } == "--producer") return producer_main(argv[2], argc >= 4 ? argv[3] : "");
    const std::string root { repository_root() };

    "the level 3 GCC example loads"_test = [&] {
        auto database = mcppls::spec::load_database(base::join_path(root, "docs/specs/examples/s1-level3-gcc.json"));
        expect(fatal(database.has_value())) << (database ? "" : database.error().message);
        expect(database->profileVersion == "0.2.0");
        expect(mcppls::spec::conformance_level(*database) == 3_i);
        expect(fatal(database->sets.size() == 1u));
        const auto& set = database->sets.front();
        expect(set.units.size() == 3u);
        expect(set.units[1].role == std::optional<mcppls::spec::Role> { mcppls::spec::Role::module_interface });
        expect(set.units[2].isPrivate);
        const auto* toolchain = mcppls::spec::find_toolchain(*database, set.toolchain);
        expect(fatal(toolchain != nullptr));
        expect(toolchain->family == mcppls::spec::Family::gcc);
        expect(toolchain->stdlib.has_value() && toolchain->stdlib->name == "libstdc++");
        expect(mcppls::spec::absolute_source(set.units[0]) == "/home/u/hello/src/greet/detail.cppm") << mcppls::spec::absolute_source(set.units[0]);
    };

    "a database round-trips through JSON"_test = [&] {
        auto first = mcppls::spec::load_database(base::join_path(root, "docs/specs/examples/s1-level2-clang-two-sets.json"));
        expect(fatal(first.has_value()));
        const Json written = Json::parse(mcppls::spec::to_json(*first).dump());
        auto second = mcppls::spec::from_json(written);
        expect(fatal(second.has_value()));
        expect(mcppls::spec::to_json(*second) == mcppls::spec::to_json(*first));
        expect(mcppls::spec::conformance_level(*second) == 2_i);
    };

    "module names resolve in the specified order"_test = [&] {
        auto database = mcppls::spec::load_database(base::join_path(root, "docs/specs/examples/s1-level2-clang-two-sets.json"));
        expect(fatal(database.has_value()));
        const auto shapes = mcppls::spec::find_set(*database, "shapes@Debug");
        const auto app = mcppls::spec::find_set(*database, "app@Debug");
        expect(fatal(shapes.has_value() && app.has_value()));
        std::vector<std::string> manifestsRead;
        const mcppls::spec::MetadataReader reader = [&](std::string_view path) {
            manifestsRead.emplace_back(path);
            return std::vector<mcppls::spec::ModuleEntry> { { "std", "/opt/llvm/share/libc++/v1/std.cppm", true, {}, {} },
                                                    { "std.compat", "/opt/llvm/share/libc++/v1/std.compat.cppm", true, {}, {} } };
        };
        const auto inSet = mcppls::spec::resolve_module(*database, *shapes, "demo.shapes:circle", reader);
        expect(inSet.from == mcppls::spec::ResolvedFrom::set);
        expect(inSet.providers.size() == 1u && inSet.providers[0].source == "/work/demo/shapes/circle.cppm");
        const auto visible = mcppls::spec::resolve_module(*database, *app, "demo.shapes", reader);
        expect(visible.from == mcppls::spec::ResolvedFrom::visible_set);
        expect(visible.providers.size() == 1u && visible.providers[0].set == "shapes@Debug");
        expect(manifestsRead.empty());
        const auto stdlib = mcppls::spec::resolve_module(*database, *app, "std", reader);
        expect(stdlib.from == mcppls::spec::ResolvedFrom::stdlib);
        expect(stdlib.providers.size() == 1u);
        const auto missing = mcppls::spec::resolve_module(*database, *app, "nowhere", reader);
        expect(missing.from == mcppls::spec::ResolvedFrom::unresolved && missing.providers.empty());
    };

    "private units are not visible and duplicates are ambiguous"_test = [] {
        auto database = mcppls::spec::from_json(Json::parse(R"({
          "version": 1, "revision": 0,
          "sets": [
            { "name": "a", "visible-sets": ["b", "c"], "translation-units": [
              { "source": "main.cpp", "work-directory": "/p", "arguments": ["cc"], "requires": ["m", "hidden"] } ] },
            { "name": "b", "translation-units": [
              { "source": "b/m.cppm", "work-directory": "/p", "arguments": ["cc"], "provides": { "m": "" } },
              { "source": "b/h.cppm", "work-directory": "/p", "arguments": ["cc"], "provides": { "hidden": "" }, "private": true } ] },
            { "name": "c", "translation-units": [
              { "source": "c/m.cppm", "work-directory": "/p", "arguments": ["cc"], "provides": { "m": "" } } ] }
          ] })"));
        expect(fatal(database.has_value()));
        expect(mcppls::spec::conformance_level(*database) == 1_i);
        const mcppls::spec::MetadataReader none = [](std::string_view) { return std::vector<mcppls::spec::ModuleEntry> {}; };
        const auto m = mcppls::spec::resolve_module(*database, 0, "m", none);
        expect(m.from == mcppls::spec::ResolvedFrom::visible_set);
        expect(m.ambiguous());
        const auto hidden = mcppls::spec::resolve_module(*database, 0, "hidden", none);
        expect(hidden.from == mcppls::spec::ResolvedFrom::unresolved);
        // Visibility is what a set lists, never derived through the sets it lists.
        auto chain = mcppls::spec::from_json(Json::parse(R"({
          "version": 1, "revision": 0,
          "sets": [
            { "name": "a", "visible-sets": ["b"], "translation-units": [
              { "source": "a.cpp", "work-directory": "/p", "arguments": ["cc"], "requires": ["far"] } ] },
            { "name": "b", "visible-sets": ["c"], "translation-units": [
              { "source": "b.cppm", "work-directory": "/p", "arguments": ["cc"], "provides": { "near": "" } } ] },
            { "name": "c", "translation-units": [
              { "source": "c.cppm", "work-directory": "/p", "arguments": ["cc"], "provides": { "far": "" } } ] }
          ], "future-top-level": { "ignored": true } })"));
        expect(fatal(chain.has_value())) << "unknown fields are ignored";
        expect(mcppls::spec::resolve_module(*chain, 0, "far", none).from == mcppls::spec::ResolvedFrom::unresolved)
            << "a set does not see what its visible sets see";
        const auto commands = mcppls::spec::to_compile_commands(*database);
        expect(commands.size() == 4u);
        expect(commands[0]["directory"] == "/p");
        expect(mcppls::spec::to_compile_commands(*database, "b").size() == 2u) << "the caller chooses the set";
    };

    "invalid databases are errors"_test = [] {
        expect(!mcppls::spec::from_json(Json::parse(R"({"sets": []})")).has_value());
        expect(!mcppls::spec::from_json(Json::parse(R"({"version": 1, "sets": [ { "translation-units": [] } ]})")).has_value());
        expect(!mcppls::spec::from_json(Json::parse(R"({"version": 1, "sets": [ { "name": "s", "translation-units": [ { "source": "x" } ] } ]})")).has_value());
        expect(!mcppls::spec::from_json(Json::parse("[]")).has_value());
    };

    "module metadata paths resolve against the manifest"_test = [] {
        const std::string directory { scratch("metadata") };
        const std::string lib { base::join_path(directory, "lib/x86_64-unknown-linux-gnu") };
        expect(fs::create_directories(lib).has_value());
        const std::string manifest { base::join_path(lib, "libc++.modules.json") };
        expect(fs::write_file(manifest, R"({"version":1,"revision":1,"modules":[
            {"logical-name":"std","source-path":"../../share/libc++/v1/std.cppm","is-std-library":true,
             "local-arguments":{"system-include-directories":["../../share/libc++/v1"]}},
            {"logical-name":"std.compat","source-path":"../../share/libc++/v1/std.compat.cppm","is-std-library":true,
             "local-arguments":{"definitions":[{"name":"X","value":"1"}]}}]})").has_value());
        auto entries = mcppls::spec::read_module_metadata(manifest);
        expect(fatal(entries.has_value()));
        expect(fatal(entries->size() == 2u));
        expect((*entries)[0].source == base::join_path(directory, "share/libc++/v1/std.cppm")) << (*entries)[0].source;
        expect((*entries)[0].systemIncludeDirectories.size() == 1u);
        expect((*entries)[1].definitions.size() == 1u && (*entries)[1].definitions[0].value == std::optional<std::string> { "1" });
        const std::string gcc { base::join_path(directory, "gcc.json") };
        expect(fs::write_file(gcc, R"({"version":1,"revision":1,"modules":[{"logical-name":"std","source-path":"/usr/include/c++/16/bits/std.cc","is-std-library":true}]})").has_value());
        auto gccEntries = mcppls::spec::read_module_metadata(gcc);
        expect(fatal(gccEntries.has_value() && gccEntries->size() == 1u));
        expect((*gccEntries)[0].source == "/usr/include/c++/16/bits/std.cc");
        expect(!mcppls::spec::read_module_metadata(base::join_path(directory, "absent.json")).has_value());
        // The MSVC STL's own shape, as windows-2022's 14.44.35207 ships it (usable plan E1).
        const std::string msvc { base::join_path(directory, "msvc/modules/modules.json") };
        expect(fs::create_directories(base::parent_path(msvc)).has_value());
        expect(fs::write_file(msvc, R"({"version": 1, "revision": 0, "library": "microsoft/STL", "module-sources": ["std.ixx", "std.compat.ixx"]})").has_value());
        auto msvcEntries = mcppls::spec::read_module_metadata(msvc);
        expect(fatal(msvcEntries.has_value() && msvcEntries->size() == 2u));
        expect((*msvcEntries)[0].logicalName == "std" && (*msvcEntries)[1].logicalName == "std.compat") << (*msvcEntries)[1].logicalName;
        expect((*msvcEntries)[0].source == base::join_path(directory, "msvc/modules/std.ixx") && (*msvcEntries)[0].isStdLibrary);
        fs::remove_all(directory);
    };

    "kit examples load and bad kits are refused"_test = [&] {
        for (std::string_view name : { "s4-kit-linux-x64.json", "s4-kit-win32-x64.json", "s4-kit-darwin-arm64.json" }) {
            auto text = fs::read_file(base::join_path(root, base::join_path("docs/specs/examples", name)));
            expect(fatal(text.has_value()));
            auto kit = mcppls::spec::parse_kit(Json::parse(*text), "/kits/k");
            expect(kit.has_value()) << name << (kit ? "" : kit.error().message);
            if (kit) {
                expect(!kit->systemIncludeDirectories.empty());
                expect(kit->moduleMetadata.starts_with("/kits/k/"));
                expect(mcppls::spec::requires_macos_sdk(*kit) == (name == "s4-kit-darwin-arm64.json"));
            }
        }
        auto valid = Json::parse(R"({"kit-version":1,"name":"k","target":"t",
            "stdlib":{"name":"libc++","version":"23.1.0","module-metadata":"m.json"},
            "system-include-directories":["include"],"sysroot":null,"licenses":[]})");
        expect(mcppls::spec::parse_kit(valid, "/k").has_value());
        auto escaping = valid;
        escaping["system-include-directories"] = Json::array({ "../outside" });
        expect(!mcppls::spec::parse_kit(escaping, "/k").has_value());
        auto absolute = valid;
        absolute["stdlib"]["module-metadata"] = "C:/m.json";
        expect(!mcppls::spec::parse_kit(absolute, "/k").has_value());
        auto unknownRequirement = valid;
        unknownRequirement["requires"] = Json::array({ Json { { "kind", "quantum-sdk" } } });
        expect(!mcppls::spec::parse_kit(unknownRequirement, "/k").has_value());
        auto future = valid;
        future["kit-version"] = 2;
        expect(!mcppls::spec::parse_kit(future, "/k").has_value());
        auto extended = valid;
        extended["vendor-field"] = Json { { "x", 1 } };
        extended["stdlib"]["vendor"] = "y";
        expect(mcppls::spec::parse_kit(extended, "/k").has_value()) << "fields a consumer does not know are ignored";
    };

    "discovery output is interpreted"_test = [&] {
        auto text = fs::read_file(base::join_path(root, "docs/specs/examples/s2-messages.jsonl"));
        expect(fatal(text.has_value()));
        auto result = mcppls::spec::parse_discovery_output(*text);
        expect(fatal(result.has_value()));
        expect(result->database == "/home/u/hello/target/build_database.json");
        expect(result->watch.size() == 4u);
        expect(result->progress.size() == 2u);
        expect(!mcppls::spec::parse_discovery_output(R"({"kind":"progress","message":"x"})").has_value());
        auto failed = mcppls::spec::parse_discovery_output(R"({"kind":"error","message":"no toolchain"})");
        expect(!failed.has_value() && failed.error().message == "no toolchain");
        expect(!mcppls::spec::parse_discovery_output("not json").has_value());
        const Json request = mcppls::spec::make_discovery_request({ "/w", { "/w/a.cppm" }, "debug", false });
        expect(request["profile-version"] == "0.2.0");
        expect(request["files"].size() == 1u);
        // S2 3.2: the consumer says whether this run may reach the network. The default is no.
        expect(request["network"] == false);
        expect(mcppls::spec::make_discovery_request({ "/w", {}, {}, true })["network"] == true);
        for (const auto& field : request.items()) {
            const std::string name { field.key() };
            expect(name == "workspace" || name == "files" || name == "configuration" || name == "network" || name == "profile-version")
                << "the request carries nothing else: " << name;
        }
    };

    "a stream ends with a terminal message, and nothing else counts"_test = [] {
        namespace spec = mcppls::spec;
        auto unknown = spec::parse_discovery_output("{\"kind\":\"progress\",\"message\":\"a\"}\n{\"kind\":\"vendor\"}\n"
                                                    "{\"kind\":\"finished\",\"database\":\"/w/db.json\",\"watch\":[],\"extra\":1}\n");
        expect(fatal(unknown.has_value()));
        expect(unknown->progress.size() == 1u) << "a message of unknown kind is progress without a message";
        expect(!spec::parse_discovery_output("{\"kind\":\"progress\",\"message\":\"a\"}\n{\"kind\":\"vendor\"}\n").has_value())
            << "an unknown kind as the last line leaves the stream without a terminal message";
        expect(!spec::parse_discovery_output("{\"kind\":\"progress\",\"message\":\"a\"}\n{not json}\n").has_value());
        expect(!spec::parse_discovery_output(R"({"kind":"finished","database":"target/db.json","watch":[]})").has_value()) << "a relative database";
        auto afterTerminal = spec::parse_discovery_output("{\"kind\":\"finished\",\"database\":\"/w/db.json\",\"watch\":[\"/w/a\"]}\ntrailing\n");
        expect(afterTerminal.has_value() && afterTerminal->watch.size() == 1u) << "reading stops at the terminal message";
    };

    "a discovery command runs as a child, within a bound"_test = [&] {
        namespace spec = mcppls::spec;
        const std::string self { self_path() };
        const std::string database { base::join_path(root, "docs/specs/examples/s1-level3-gcc.json") };
        const spec::DiscoveryRequest request { root, {}, {} };
        const std::vector<std::string> stream { self, "--producer", "stream", database };
        auto answered = spec::run_discovery(stream, request, root, spec::RunContext { .hard = std::chrono::seconds { 60 } });
        expect(fatal(answered.has_value())) << (answered ? "" : answered.error().message);
        expect(answered->database == database && answered->watch == std::vector<std::string> { "mcpp.toml" });
        const std::vector<std::string> truncated { self, "--producer", "truncated" };
        expect(!spec::run_discovery(truncated, request, root, spec::RunContext { .hard = std::chrono::seconds { 60 } }).has_value());
        const std::vector<std::string> failing { self, "--producer", "failing" };
        auto failed = spec::run_discovery(failing, request, root, spec::RunContext { .hard = std::chrono::seconds { 60 } });
        expect(!failed.has_value() && failed.error().message == "no toolchain");
        const std::vector<std::string> sleeping { self, "--producer", "sleeping" };
        const auto started = std::chrono::steady_clock::now();
        auto expired = spec::run_discovery(sleeping, request, root, spec::RunContext { .hard = std::chrono::milliseconds { 500 } });
        expect(!expired.has_value() && expired.error().code == "discovery-timeout");
        expect(std::chrono::steady_clock::now() - started < std::chrono::seconds { 20 }) << "the command is terminated when the bound expires";
    };

    "a single-document envelope is read, and only a database answers"_test = [&] {
        namespace spec = mcppls::spec;
        const auto text = fs::read_file(base::join_path(root, "docs/specs/examples/s2-envelope.json"));
        expect(fatal(text.has_value()));
        auto document = spec::parse_database_envelope(*text);
        expect(fatal(document.has_value())) << (document ? "" : document.error().message);
        expect(document->database.is_object() && !document->watch.empty() && !document->effects.empty());
        Json envelope = Json::parse(*text);
        envelope["vendor-extension"] = Json { { "x", 1 } };
        expect(spec::parse_database_envelope(envelope.dump()).has_value()) << "unknown fields are ignored";
        Json wrongKind = Json::parse(*text);
        wrongKind["kind"] = "mcpp.build";
        expect(!spec::parse_database_envelope(wrongKind.dump()).has_value());
        Json futureKind = Json::parse(*text);
        futureKind["kindVersion"] = 2;
        expect(!spec::parse_database_envelope(futureKind.dump()).has_value());
        Json failure = Json::parse(*text);
        failure.erase("data");
        failure["diagnostics"] = Json::array({ Json { { "code", "E_TOOLCHAIN" }, { "severity", "error" }, { "message", "no compiler" } } });
        auto failed = spec::parse_database_envelope(failure.dump());
        expect(!failed.has_value() && failed.error().message.find("no compiler") != std::string::npos) << "a command without data has failed; its diagnostics say why";
        // S2 0.3.0 (S2-3.4-12, S2-3.4-13): data with error diagnostics describes all but what they name, by `path`.
        Json partial = Json::parse(*text);
        partial["diagnostics"] = Json::array({ Json { { "code", "MCPP_MEMBER_FAILED" }, { "severity", "error" }, { "message", "lupdate failed" },
                                                      { "path", "tools/updater/mcpp.toml" } } });
        auto described = spec::parse_database_envelope(partial.dump());
        expect(fatal(described.has_value())) << "the rest of the document is used";
        expect(described->database.is_object() && described->diagnostics.size() == 1u);
        expect(described->diagnostics[0].path == "tools/updater/mcpp.toml" && described->diagnostics[0].severity == "error");

        auto protocol = spec::parse_producer_protocol(R"({"schemaVersion":1,"kind":"mcpp.protocol","kinds":{"mcpp.build-database":1},
            "commands":{"emit build-database":{"effects":["read-project","network"]}},"future":{}})");
        expect(fatal(protocol.has_value()));
        expect(protocol->kinds.contains("mcpp.build-database"));
        expect(protocol->commandEffects.at("emit build-database") == std::vector<std::string> { "read-project", "network" });
        expect(!spec::parse_producer_protocol("[]").has_value());
        expect(spec::effects_acceptable(protocol->commandEffects.at("emit build-database")));
        const std::vector<std::string> writes { "read-project", "write-project" };
        expect(!spec::effects_acceptable(writes)) << "a command that writes into the project is not run";
    };

    return report();
}
