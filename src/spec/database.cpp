module mcppls.spec.database;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.platform.fs;
import mcppls.spec.metadata;

namespace mcppls::spec {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

namespace {

constexpr std::array<std::pair<Family, std::string_view>, 5> FAMILY_NAMES { {
    { Family::gcc, "gcc" }, { Family::clang, "clang" }, { Family::msvc, "msvc" },
    { Family::clang_cl, "clang-cl" }, { Family::other, "other" },
} };

constexpr std::array<std::pair<Role, std::string_view>, 7> ROLE_NAMES { {
    { Role::module_interface, "module-interface" },
    { Role::module_partition_interface, "module-partition-interface" },
    { Role::module_partition_implementation, "module-partition-implementation" },
    { Role::module_implementation, "module-implementation" },
    { Role::non_module, "non-module" },
    { Role::unknown, "unknown" },
    { Role::header_unit, "header-unit" },
} };

std::string string_or(const Json& object, std::string_view key, std::string fallback = {}) {
    const auto it = object.find(key);
    return (it != object.end() && it->is_string()) ? it->get<std::string>() : std::move(fallback);
}

std::vector<std::string> strings(const Json& object, std::string_view key) {
    std::vector<std::string> result;
    const auto it = object.find(key);
    if (it == object.end() || !it->is_array()) return result;
    for (const auto& item : *it) {
        if (item.is_string()) result.push_back(item.get<std::string>());
    }
    return result;
}

std::string resolve_in(std::string_view directory, std::string path) {
    if (path.empty() || directory.empty() || base::is_absolute_path(path)) return path;
    return base::join_path(directory, path);
}

SemanticOptions read_options(const Json& value) {
    SemanticOptions options;
    if (!value.is_object()) return options;
    if (auto it = value.find("language-standard"); it != value.end() && it->is_string()) options.languageStandard = it->get<std::string>();
    if (auto it = value.find("language-extensions"); it != value.end() && it->is_string()) options.languageExtensions = it->get<std::string>();
    if (auto it = value.find("macros"); it != value.end() && it->is_array()) {
        for (const auto& macro : *it) {
            if (!macro.is_object()) continue;
            if (auto define = macro.find("define"); define != macro.end() && define->is_string()) {
                Macro item { define->get<std::string>(), std::nullopt, false };
                if (auto macroValue = macro.find("value"); macroValue != macro.end() && macroValue->is_string()) item.value = macroValue->get<std::string>();
                options.macros.push_back(std::move(item));
            } else if (auto undefine = macro.find("undefine"); undefine != macro.end() && undefine->is_string()) {
                options.macros.push_back(Macro { undefine->get<std::string>(), std::nullopt, true });
            }
        }
    }
    if (auto it = value.find("include-directories"); it != value.end() && it->is_object()) {
        options.includeDirectories.user = strings(*it, "user");
        options.includeDirectories.quote = strings(*it, "quote");
        options.includeDirectories.system = strings(*it, "system");
        options.includeDirectories.after = strings(*it, "after");
    }
    options.forcedIncludes = strings(value, "forced-includes");
    if (auto it = value.find("exceptions"); it != value.end() && it->is_boolean()) options.exceptions = it->get<bool>();
    if (auto it = value.find("rtti"); it != value.end() && it->is_boolean()) options.rtti = it->get<bool>();
    if (auto it = value.find("raw-semantic-arguments"); it != value.end() && it->is_object()) {
        for (const auto& item : it->items()) {
            std::vector<std::string> list;
            if (item.value().is_array()) {
                for (const auto& argument : item.value()) {
                    if (argument.is_string()) list.push_back(argument.get<std::string>());
                }
            }
            options.rawSemanticArguments.emplace_back(item.key(), std::move(list));
        }
    }
    return options;
}

OrderedJson write_options(const SemanticOptions& options) {
    OrderedJson value = OrderedJson::object();
    if (options.languageStandard) value["language-standard"] = *options.languageStandard;
    if (options.languageExtensions) value["language-extensions"] = *options.languageExtensions;
    OrderedJson macros = OrderedJson::array();
    for (const auto& macro : options.macros) {
        if (macro.undefine) {
            macros.push_back(OrderedJson { { "undefine", macro.name } });
        } else {
            OrderedJson item = OrderedJson::object();
            item["define"] = macro.name;
            item["value"] = macro.value ? OrderedJson(*macro.value) : OrderedJson(nullptr);
            macros.push_back(std::move(item));
        }
    }
    value["macros"] = std::move(macros);
    value["include-directories"] = OrderedJson {
        { "user", options.includeDirectories.user }, { "quote", options.includeDirectories.quote },
        { "system", options.includeDirectories.system }, { "after", options.includeDirectories.after },
    };
    value["forced-includes"] = options.forcedIncludes;
    if (options.exceptions) value["exceptions"] = *options.exceptions;
    if (options.rtti) value["rtti"] = *options.rtti;
    if (!options.rawSemanticArguments.empty()) {
        OrderedJson raw = OrderedJson::object();
        for (const auto& [family, arguments] : options.rawSemanticArguments) raw[family] = arguments;
        value["raw-semantic-arguments"] = std::move(raw);
    }
    return value;
}

base::Result<Toolchain> read_toolchain(const Json& value, std::string_view id, std::string_view databaseDirectory) {
    if (!value.is_object()) return base::fail("database-invalid", std::format("toolchain {} is not an object", id));
    Toolchain toolchain;
    const auto family = parse_family(string_or(value, "family"));
    if (!family) return base::fail("database-invalid", std::format("toolchain {} has no valid family", id));
    toolchain.family = *family;
    toolchain.version = string_or(value, "version");
    toolchain.buildId = string_or(value, "build-id");
    toolchain.driver = resolve_in(databaseDirectory, string_or(value, "driver"));
    toolchain.target = string_or(value, "target");
    toolchain.sysroot = resolve_in(databaseDirectory, string_or(value, "sysroot"));
    toolchain.configFiles = strings(value, "config-files");
    if (auto stdlib = value.find("stdlib"); stdlib != value.end() && stdlib->is_object()) {
        toolchain.stdlib = Stdlib { string_or(*stdlib, "name"), string_or(*stdlib, "version"),
                                    resolve_in(databaseDirectory, string_or(*stdlib, "module-metadata")) };
    }
    return toolchain;
}

bool non_empty_options(const std::optional<SemanticOptions>& options) { return options.has_value(); }

} // namespace

std::string_view to_string(Family family) {
    for (const auto& [value, name] : FAMILY_NAMES) {
        if (value == family) return name;
    }
    return "other";
}

std::optional<Family> parse_family(std::string_view text) {
    for (const auto& [value, name] : FAMILY_NAMES) {
        if (name == text) return value;
    }
    return std::nullopt;
}

std::string_view to_string(Role role) {
    for (const auto& [value, name] : ROLE_NAMES) {
        if (value == role) return name;
    }
    return "unknown";
}

std::optional<Role> parse_role(std::string_view text) {
    for (const auto& [value, name] : ROLE_NAMES) {
        if (name == text) return value;
    }
    return std::nullopt;
}

bool is_importable(Role role) {
    return role == Role::module_interface || role == Role::module_partition_interface
        || role == Role::module_partition_implementation;
}

std::string_view to_string(ResolvedFrom from) {
    switch (from) {
    case ResolvedFrom::set: return "set";
    case ResolvedFrom::visible_set: return "visible-set";
    case ResolvedFrom::module_metadata: return "module-metadata";
    case ResolvedFrom::stdlib: return "stdlib";
    case ResolvedFrom::unresolved: return "unresolved";
    }
    return "unresolved";
}

base::Result<Database> from_json(const Json& document, std::string_view databaseDirectory) {
    if (!document.is_object()) return base::fail("database-invalid", "a build database is a JSON object");
    Database database;
    if (auto version = document.find("version"); version != document.end() && version->is_number_integer()) {
        database.version = version->get<int>();
    } else {
        return base::fail("database-invalid", "missing integer \"version\"");
    }
    if (auto revision = document.find("revision"); revision != document.end() && revision->is_number_integer()) {
        database.revision = revision->get<int>();
    }
    if (auto ide = document.find("ide"); ide != document.end() && ide->is_object()) {
        database.hasIde = true;
        database.profileVersion = string_or(*ide, "profile-version");
        if (auto generator = ide->find("generator"); generator != ide->end() && generator->is_object()) {
            database.generator = Generator { string_or(*generator, "name"), string_or(*generator, "version") };
        }
        if (auto toolchains = ide->find("toolchains"); toolchains != ide->end() && toolchains->is_object()) {
            for (const auto& item : toolchains->items()) {
                auto toolchain = read_toolchain(item.value(), item.key(), databaseDirectory);
                if (!toolchain) return std::unexpected { toolchain.error() };
                database.toolchains.emplace_back(item.key(), std::move(*toolchain));
            }
        }
    } else {
        database.profileVersion.clear();
    }

    const auto sets = document.find("sets");
    if (sets == document.end() || !sets->is_array()) return base::fail("database-invalid", "missing \"sets\" array");
    for (const auto& setValue : *sets) {
        if (!setValue.is_object()) return base::fail("database-invalid", "a set is not an object");
        Set set;
        set.name = string_or(setValue, "name");
        if (set.name.empty()) return base::fail("database-invalid", "a set has no name");
        set.familyName = string_or(setValue, "family-name");
        set.visibleSets = strings(setValue, "visible-sets");
        set.baselineArguments = strings(setValue, "baseline-arguments");
        if (auto ide = setValue.find("ide"); ide != setValue.end() && ide->is_object()) {
            set.hasIde = true;
            set.toolchain = string_or(*ide, "toolchain");
            set.configuration = string_or(*ide, "configuration");
            set.kind = string_or(*ide, "kind");
            if (auto options = ide->find("options"); options != ide->end() && options->is_object()) set.options = read_options(*options);
            for (auto path : strings(*ide, "module-metadata")) set.moduleMetadata.push_back(resolve_in(databaseDirectory, std::move(path)));
        }
        const auto units = setValue.find("translation-units");
        if (units != setValue.end() && units->is_array()) {
            for (const auto& unitValue : *units) {
                if (!unitValue.is_object()) return base::fail("database-invalid", std::format("set {} has a unit that is not an object", set.name));
                TranslationUnit unit;
                unit.source = string_or(unitValue, "source");
                unit.workDirectory = string_or(unitValue, "work-directory");
                unit.arguments = strings(unitValue, "arguments");
                if (unit.source.empty() || unit.workDirectory.empty() || unit.arguments.empty()) {
                    return base::fail("database-invalid", std::format("set {} has a unit without source, work-directory or arguments", set.name));
                }
                unit.localArguments = strings(unitValue, "local-arguments");
                unit.object = string_or(unitValue, "object");
                unit.isPrivate = unitValue.value("private", false);
                if (auto provides = unitValue.find("provides"); provides != unitValue.end() && provides->is_object()) {
                    for (const auto& item : provides->items()) {
                        unit.providedModules.emplace_back(item.key(), item.value().is_string() ? item.value().get<std::string>() : std::string {});
                    }
                }
                unit.requiredModules = strings(unitValue, "requires");
                if (auto ide = unitValue.find("ide"); ide != unitValue.end() && ide->is_object()) {
                    if (auto role = ide->find("role"); role != ide->end() && role->is_string()) {
                        unit.role = parse_role(role->get<std::string>()).value_or(Role::unknown);
                    }
                    if (auto options = ide->find("options"); options != ide->end() && options->is_object()) unit.options = read_options(*options);
                }
                set.units.push_back(std::move(unit));
            }
        }
        database.sets.push_back(std::move(set));
    }
    return database;
}

OrderedJson to_json(const Database& database) {
    OrderedJson document = OrderedJson::object();
    document["version"] = database.version;
    document["revision"] = database.revision;
    if (database.hasIde) {
        OrderedJson ide = OrderedJson::object();
        ide["profile-version"] = database.profileVersion;
        if (database.generator) ide["generator"] = OrderedJson { { "name", database.generator->name }, { "version", database.generator->version } };
        OrderedJson toolchains = OrderedJson::object();
        for (const auto& [id, toolchain] : database.toolchains) {
            OrderedJson value = OrderedJson::object();
            value["family"] = to_string(toolchain.family);
            value["version"] = toolchain.version;
            if (!toolchain.buildId.empty()) value["build-id"] = toolchain.buildId;
            value["driver"] = toolchain.driver;
            value["target"] = toolchain.target;
            if (!toolchain.sysroot.empty()) value["sysroot"] = toolchain.sysroot;
            if (toolchain.stdlib) {
                OrderedJson stdlib = OrderedJson::object();
                stdlib["name"] = toolchain.stdlib->name;
                if (!toolchain.stdlib->version.empty()) stdlib["version"] = toolchain.stdlib->version;
                if (!toolchain.stdlib->moduleMetadata.empty()) stdlib["module-metadata"] = toolchain.stdlib->moduleMetadata;
                value["stdlib"] = std::move(stdlib);
            }
            value["config-files"] = toolchain.configFiles;
            toolchains[id] = std::move(value);
        }
        ide["toolchains"] = std::move(toolchains);
        document["ide"] = std::move(ide);
    }
    OrderedJson sets = OrderedJson::array();
    for (const auto& set : database.sets) {
        OrderedJson setValue = OrderedJson::object();
        setValue["name"] = set.name;
        setValue["family-name"] = set.familyName;
        setValue["visible-sets"] = set.visibleSets;
        setValue["baseline-arguments"] = set.baselineArguments;
        if (set.hasIde) {
            OrderedJson ide = OrderedJson::object();
            ide["toolchain"] = set.toolchain;
            if (!set.configuration.empty()) ide["configuration"] = set.configuration;
            if (!set.kind.empty()) ide["kind"] = set.kind;
            if (set.options) ide["options"] = write_options(*set.options);
            if (!set.moduleMetadata.empty()) ide["module-metadata"] = set.moduleMetadata;
            setValue["ide"] = std::move(ide);
        }
        OrderedJson units = OrderedJson::array();
        for (const auto& unit : set.units) {
            OrderedJson unitValue = OrderedJson::object();
            unitValue["source"] = unit.source;
            unitValue["work-directory"] = unit.workDirectory;
            unitValue["arguments"] = unit.arguments;
            unitValue["local-arguments"] = unit.localArguments;
            unitValue["object"] = unit.object;
            unitValue["private"] = unit.isPrivate;
            OrderedJson provides = OrderedJson::object();
            for (const auto& [name, bmi] : unit.providedModules) provides[name] = bmi;
            unitValue["provides"] = std::move(provides);
            unitValue["requires"] = unit.requiredModules;
            if (unit.role || unit.options) {
                OrderedJson ide = OrderedJson::object();
                if (unit.role) ide["role"] = to_string(*unit.role);
                if (unit.options) ide["options"] = write_options(*unit.options);
                unitValue["ide"] = std::move(ide);
            }
            units.push_back(std::move(unitValue));
        }
        setValue["translation-units"] = std::move(units);
        sets.push_back(std::move(setValue));
    }
    document["sets"] = std::move(sets);
    return document;
}

base::Result<Database> load_database(std::string_view path) {
    auto text = platform::fs::read_file(path);
    if (!text) return std::unexpected { text.error() };
    Json document = Json::parse(*text, nullptr, false);
    if (document.is_discarded()) return base::fail("database-invalid", std::format("{} is not valid JSON", path));
    return from_json(document, base::parent_path(path));
}

int conformance_level(const Database& database) {
    // Level 1 cannot be refuted from the document alone beyond its shape.
    int level { 1 };
    bool level2 { database.hasIde && !database.profileVersion.empty() && !database.toolchains.empty() };
    bool level3 { true };
    for (const auto& set : database.sets) {
        if (!set.hasIde || set.toolchain.empty() || find_toolchain(database, set.toolchain) == nullptr) level2 = false;
        if (!non_empty_options(set.options)) level3 = false;
        for (const auto& unit : set.units) {
            if (!unit.role) level2 = false;
            if (!unit.localArguments.empty() && !unit.options) level3 = false;
        }
    }
    if (level2) level = 2;
    if (level2 && level3) level = 3;
    return level;
}

const Toolchain* find_toolchain(const Database& database, std::string_view id) {
    for (const auto& [key, toolchain] : database.toolchains) {
        if (key == id) return &toolchain;
    }
    return nullptr;
}

std::optional<std::size_t> find_set(const Database& database, std::string_view name) {
    for (std::size_t i { 0 }; i < database.sets.size(); ++i) {
        if (database.sets[i].name == name) return i;
    }
    return std::nullopt;
}

std::string absolute_path_in_unit(const TranslationUnit& unit, std::string_view path) {
    return base::join_path(unit.workDirectory, path);
}

std::string absolute_source(const TranslationUnit& unit) { return absolute_path_in_unit(unit, unit.source); }

OrderedJson to_compile_commands(const Database& database, std::string_view setName) {
    OrderedJson entries = OrderedJson::array();
    std::set<std::string> seen;
    for (const auto& set : database.sets) {
        if (!setName.empty() && set.name != setName) continue;
        for (const auto& unit : set.units) {
            const std::string source { absolute_source(unit) };
            if (!seen.insert(source).second) continue;
            OrderedJson entry = OrderedJson::object();
            entry["directory"] = unit.workDirectory;
            entry["file"] = unit.source;
            entry["arguments"] = unit.arguments;
            if (!unit.object.empty()) entry["output"] = unit.object;
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

Resolution resolve_module(const Database& database, std::size_t setIndex, std::string_view moduleName, const MetadataReader& reader) {
    Resolution resolution;
    if (setIndex >= database.sets.size()) return resolution;
    const Set& set { database.sets[setIndex] };

    auto collect_units = [&](const Set& from, bool publicOnly) {
        std::vector<Provider> found;
        for (const auto& unit : from.units) {
            if (publicOnly && unit.isPrivate) continue;
            for (const auto& [name, bmi] : unit.providedModules) {
                if (name == moduleName) found.push_back(Provider { absolute_source(unit), from.name, unit.role, {} });
            }
        }
        return found;
    };

    resolution.providers = collect_units(set, false);
    if (!resolution.providers.empty()) {
        resolution.from = ResolvedFrom::set;
        return resolution;
    }
    for (const auto& visibleName : set.visibleSets) {
        if (auto index = find_set(database, visibleName)) {
            auto found = collect_units(database.sets[*index], true);
            resolution.providers.insert(resolution.providers.end(), found.begin(), found.end());
        }
    }
    if (!resolution.providers.empty()) {
        resolution.from = ResolvedFrom::visible_set;
        return resolution;
    }
    auto collect_metadata = [&](std::string_view manifest) {
        for (const auto& entry : reader(manifest)) {
            if (entry.logicalName == moduleName) {
                resolution.providers.push_back(Provider { entry.source, {}, Role::module_interface, entry.systemIncludeDirectories });
            }
        }
    };
    for (const auto& manifest : set.moduleMetadata) collect_metadata(manifest);
    if (!resolution.providers.empty()) {
        resolution.from = ResolvedFrom::module_metadata;
        return resolution;
    }
    if (const Toolchain* toolchain = find_toolchain(database, set.toolchain); toolchain != nullptr && toolchain->stdlib) {
        collect_metadata(toolchain->stdlib->moduleMetadata);
        if (!resolution.providers.empty()) {
            resolution.from = ResolvedFrom::stdlib;
            return resolution;
        }
    }
    resolution.from = ResolvedFrom::unresolved;
    return resolution;
}

MetadataReader caching_metadata_reader() {
    auto cache = std::make_shared<std::map<std::string, std::vector<ModuleEntry>, std::less<>>>();
    auto mutex = std::make_shared<std::mutex>();
    return [cache, mutex](std::string_view path) -> std::vector<ModuleEntry> {
        std::lock_guard lock { *mutex };
        if (auto it = cache->find(path); it != cache->end()) return it->second;
        auto entries = read_module_metadata(path);
        auto& stored = (*cache)[std::string { path }];
        if (entries) stored = std::move(*entries);
        return stored;
    };
}

} // namespace mcppls::spec
