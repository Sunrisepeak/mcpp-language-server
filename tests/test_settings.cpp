// The configuration registry (0.0.6 plan §9 T1): its own invariants, precedence and origins, every
// accepted JSON shape, an unknown value falling back to the default, an alias, and the three places
// a row's docs are supposed to agree with it -- `docs/30-settings.md`, its zh-CN mirror, and
// `editors/vscode/package.json`.
import std;
import mcppls.testing;
import nlohmann.json;
import mcpplibs.cmdline;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.fs;
import mcppls.config.settings;

namespace fs = mcppls::platform::fs;
namespace base = mcppls::base;
namespace cmdline = mcpplibs::cmdline;
namespace settings = mcppls::config::settings;
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

// A tiny `App` with exactly the global options the shipped registry's rows declare (the same loop
// `mcppls.cli.commands` runs), so a test can turn a plain argv into the `ParsedArgs`
// `Settings::apply_command_line` takes, without going through a whole process.
cmdline::ParsedArgs parse_argv(std::vector<std::string> arguments) {
    cmdline::App app { "mcppls" };
    for (const auto& row : settings::registry()) {
        if (row.surface != settings::Surface::server || row.commandLine.empty()) continue;
        const std::string flag { row.commandLine.substr(2) };
        (void)app.option(flag)
            .global(true)
            .takes_value(row.kind != settings::Kind::boolean)
            .multiple(row.kind == settings::Kind::list && row.commandLineRepeatable);
    }
    arguments.insert(arguments.begin(), "mcppls");
    auto parsed = app.parse_from(arguments);
    return parsed ? std::move(*parsed) : cmdline::ParsedArgs {};
}

// The text strictly between two marker lines, or empty when either is missing -- what a doc file
// embeds between `<!-- settings:begin -->` and `<!-- settings:end -->`.
std::string between_markers(std::string_view text, std::string_view begin, std::string_view end) {
    const auto beginAt = text.find(begin);
    const auto endAt = text.find(end);
    if (beginAt == std::string_view::npos || endAt == std::string_view::npos || endAt < beginAt) return {};
    const auto contentStart = beginAt + begin.size() + 1;   // + the newline right after the marker
    if (contentStart >= endAt) return {};
    return std::string { text.substr(contentStart, endAt - contentStart - 1) };   // - the newline right before it
}

} // namespace

int main() {
    using namespace mcppls::testing;

    "every row's key is unique, its aliases name no other row, and its default is inside its own vocabulary"_test = [] {
        std::set<std::string_view, std::less<>> keys;
        std::set<std::string_view, std::less<>> commandLines;
        for (const auto& row : settings::registry()) {
            expect(keys.insert(row.key).second) << row.key;
            for (const auto& alias : row.aliases) expect(keys.insert(alias).second) << alias;
            // An environment row's default is "not set" -- empty, whatever its `Kind` -- since
            // `Settings`'s constructor reads it from the process environment rather than validating
            // it against the row (nothing here runs it, so nothing it says can silently take effect).
            if (row.kind == settings::Kind::enumeration && row.surface != settings::Surface::environment) {
                expect(std::ranges::find(row.values, row.defaultValue) != row.values.end()) << row.key;
            }
            if (row.kind == settings::Kind::list && !row.values.empty()) {
                for (auto piece : base::split(row.defaultValue, ',')) {
                    if (!piece.empty()) expect(std::ranges::find(row.values, piece) != row.values.end()) << row.key;
                }
            }
            if (row.kind == settings::Kind::boolean) expect(row.defaultValue == "true" || row.defaultValue == "false") << row.key;
            // Every server row's own command-line spelling belongs to it alone.
            if (row.surface == settings::Surface::server && !row.commandLine.empty()) {
                expect(commandLines.insert(row.commandLine).second) << row.commandLine;
            }
            // A `client` or `environment` row never has one: neither is read by this command line.
            if (row.surface != settings::Surface::server) expect(row.commandLine.empty()) << row.key;
        }
    };

    "the command line wins over initializationOptions, which wins over the default"_test = [] {
        settings::Settings values;
        expect(values.origin("buildTool") == settings::Origin::defaulted);
        expect(values.string_value("buildTool") == "offline");

        values.apply_command_line(parse_argv({ "--build-tool", "online" }));
        expect(values.origin("buildTool") == settings::Origin::commandLine);
        expect(values.string_value("buildTool") == "online");

        // initializationOptions tries to set both buildTool (command-line already did) and
        // toolEnvironment (nobody has yet): only the second actually moves.
        values.apply_initialization_options(Json { { "buildTool", "off" }, { "toolEnvironment", "editor" } });
        expect(values.string_value("buildTool") == "online") << "the command line is immune to initializationOptions";
        expect(values.origin("buildTool") == settings::Origin::commandLine);
        expect(values.string_value("toolEnvironment") == "editor");
        expect(values.origin("toolEnvironment") == settings::Origin::client);

        // A later didChangeConfiguration updates the client-origin value, but not the command line's.
        auto result = values.apply_configuration_change(
            Json { { "settings", Json { { "mcppls", Json { { "buildTool", "off" }, { "toolEnvironment", "auto" } } } } } });
        expect(values.string_value("buildTool") == "online") << "still immune";
        expect(values.string_value("toolEnvironment") == "auto");
        expect(values.origin("toolEnvironment") == settings::Origin::clientUpdated);
        expect(std::ranges::find(result.changedKeys, std::string { "toolEnvironment" }) != result.changedKeys.end());
        expect(std::ranges::find(result.changedKeys, std::string { "buildTool" }) == result.changedKeys.end());
        // toolEnvironment is a `restart`-applies row; a `reload` one is what belongs in reloadKeys.
        expect(std::ranges::find(result.restartKeys, std::string { "toolEnvironment" }) != result.restartKeys.end());
    };

    "initializationOptions accepts a nested object, a dotted key, and either wrapped in mcppls or not"_test = [] {
        for (const Json& shape : {
                 Json { { "semanticTokens", Json { { "modules", false } } } },
                 Json { { "semanticTokens.modules", false } },
                 Json { { "mcppls", Json { { "semanticTokens", Json { { "modules", false } } } } } },
                 Json { { "mcppls", Json { { "semanticTokens.modules", false } } } },
             }) {
            settings::Settings values;
            values.apply_initialization_options(shape);
            expect(values.bool_value("semanticTokens.modules") == false) << shape.dump();
            expect(values.origin("semanticTokens.modules") == settings::Origin::client) << shape.dump();
        }
        // The whole `initialize` params works too: only its `initializationOptions` key is read.
        settings::Settings values;
        values.apply_initialization_options(Json { { "initializationOptions", Json { { "engine", "none" } } }, { "capabilities", Json::object() } });
        expect(values.string_value("engine") == "none");
    };

    "an unknown enumeration value falls back to the default and is recorded as a problem, never silently"_test = [] {
        settings::Settings values;
        values.apply_initialization_options(Json { { "buildTool", "carrier-pigeon" } });
        expect(values.string_value("buildTool") == "offline") << "an unrecognized value must not silently turn the network on";
        expect(values.origin("buildTool") == settings::Origin::defaulted);
        expect(!values.problems().empty());
        expect(values.problems().front().key == "buildTool");
    };

    "an alias is honoured wherever the current name is"_test = [] {
        static const std::vector<settings::Setting> rows { settings::Setting {
            .key = "testOnly.currentName",
            .kind = settings::Kind::boolean,
            .defaultValue = "false",
            .surface = settings::Surface::server,
            .applies = settings::Applies::immediately,
            .category = "build",
            .since = "0.0.6",
            .summary = "test-only row: proves an alias still works after a rename.",
            .summaryZh = "仅供测试：证明改名后旧名依然生效。",
            .aliases = { "testOnly.oldName" },
        } };
        settings::Settings values { rows };
        values.apply_initialization_options(Json { { "testOnly", Json { { "oldName", true } } } });
        expect(values.bool_value("testOnly.currentName") == true);
        expect(values.origin("testOnly.currentName") == settings::Origin::client);

        settings::Settings viaConfigurationChange { rows };
        auto result = viaConfigurationChange.apply_configuration_change(
            Json { { "settings", Json { { "mcppls", Json { { "testOnly.oldName", true } } } } } });
        expect(viaConfigurationChange.bool_value("testOnly.currentName") == true);
        expect(std::ranges::find(result.changedKeys, std::string { "testOnly.currentName" }) != result.changedKeys.end());
    };

    "docs/30-settings.md and its zh-CN mirror embed the renderer's own output, byte for byte"_test = [] {
        const std::string root { repository_root() };
        for (const auto& [path, lang] : { std::pair { std::string { "docs/30-settings.md" }, std::string_view { "en" } },
                                          std::pair { std::string { "docs/zh-CN/30-settings.md" }, std::string_view { "zh-CN" } } }) {
            const auto text = fs::read_file(base::join_path(root, path));
            expect(fatal(text.has_value())) << path;
            // A Windows checkout may turn the file's line endings into CRLF; the renderer writes LF.
            std::string lf { *text };
            std::erase(lf, '\r');
            const std::string embedded { between_markers(lf, "<!-- settings:begin -->", "<!-- settings:end -->") };
            const std::string rendered { settings::to_markdown(settings::registry(), lang) };
            expect(embedded == rendered) << path;
        }
    };

    "every mcppls.* property of editors/vscode/package.json is a registry row, in step with it"_test = [] {
        const std::string root { repository_root() };
        const auto text = fs::read_file(base::join_path(root, "editors/vscode/package.json"));
        expect(fatal(text.has_value()));
        const Json manifest = Json::parse(*text);
        const Json& properties = manifest.at("contributes").at("configuration").at("properties");

        std::set<std::string, std::less<>> seen;
        // Not `for (const auto& [name, schema] : properties.items())`: a const basic_json's
        // `items()` has no non-const iterator for structured bindings to destructure (src/ai/model/schema.cpp).
        for (const auto& entry : properties.items()) {
            const std::string& name { entry.key() };
            const Json& schema { entry.value() };
            if (!name.starts_with("mcppls.")) continue;
            const std::string key { name.substr(7) };
            seen.insert(key);
            const settings::Setting* row { settings::find(settings::registry(), key) };
            expect(fatal(row != nullptr)) << name << " is in package.json but not the registry";
            expect(row->surface == settings::Surface::server || row->surface == settings::Surface::client) << name;
            expect(row->clientConfigurable) << name << " is in package.json but the registry does not expect it there";

            if (row->kind == settings::Kind::boolean) {
                expect(schema.value("type", std::string {}) == "boolean") << name;
                expect(schema.at("default").get<bool>() == (row->defaultValue == "true")) << name;
            } else if (row->kind == settings::Kind::list) {
                expect(schema.value("type", std::string {}) == "array") << name;
                std::vector<std::string> defaultMembers;
                for (const auto& item : schema.at("default")) defaultMembers.push_back(item.get<std::string>());
                std::vector<std::string> rowMembers;
                for (auto piece : base::split(row->defaultValue, ',')) rowMembers.emplace_back(piece);
                expect(defaultMembers == rowMembers) << name;
                if (schema.contains("items") && schema.at("items").contains("enum")) {
                    expect(schema.at("items").at("enum").get<std::vector<std::string>>() == row->values) << name;
                }
            } else {
                expect(schema.value("type", std::string {}) == "string") << name;
                expect(schema.at("default").get<std::string>() == row->defaultValue) << name;
                if (schema.contains("enum")) expect(schema.at("enum").get<std::vector<std::string>>() == row->values) << name;
            }
        }

        for (const auto& row : settings::registry()) {
            if (!row.clientConfigurable) continue;
            expect(seen.contains(row.key)) << row.key << " should be in package.json (clientConfigurable) but is not";
        }
    };

    return report();
}
