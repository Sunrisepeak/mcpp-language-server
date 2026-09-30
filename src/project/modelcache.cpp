module mcppls.project.modelcache;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.sha256;
import mcppls.platform.fs;
import mcppls.spec.database;
import mcppls.toolchain.probe;

namespace mcppls::project {

namespace {

using Json = nlohmann::json;

Json issues_to_json(const std::vector<ModelIssue>& issues) {
    Json array = Json::array();
    for (const auto& issue : issues) array.push_back(Json { { "code", issue.code }, { "message", issue.message } });
    return array;
}

std::vector<ModelIssue> issues_from_json(const Json& value) {
    std::vector<ModelIssue> issues;
    if (!value.is_array()) return issues;
    for (const auto& entry : value) {
        if (!entry.is_object()) continue;
        issues.push_back(ModelIssue { entry.value("code", std::string {}), entry.value("message", std::string {}) });
    }
    return issues;
}

std::optional<SourceKind> source_from_name(std::string_view name) {
    for (const SourceKind kind : { SourceKind::build_database, SourceKind::mcpp, SourceKind::cmake, SourceKind::xmake,
                                   SourceKind::meson, SourceKind::compile_commands, SourceKind::inferred }) {
        if (to_string(kind) == name) return kind;
    }
    return std::nullopt;
}

// What the producer read is what the fingerprint is made of. The environment deliberately is NOT
// part of it, though it decides which producer is found: the tool environment resolves in the
// background (design 4.3) and the cache is read before that finishes, so digesting it would make
// every first comparison differ from every later one and no cache would ever look current. What a
// changed PATH actually means --- another mcpp --- is caught by the producer's own path and file
// stamp below, and by the background run that confirms the cache either way.

void derived_from_json(const Json& value, spec::Database& database) {
    if (!value.is_object()) return;
    if (const auto sets = value.find("sets"); sets != value.end() && sets->is_array()) {
        for (const auto& index : *sets) {
            if (index.is_number_unsigned() && index.get<std::size_t>() < database.sets.size()) database.sets[index.get<std::size_t>()].optionsDerived = true;
        }
    }
    if (const auto units = value.find("units"); units != value.end() && units->is_array()) {
        for (const auto& pair : *units) {
            if (!pair.is_array() || pair.size() != 2 || !pair[0].is_number_unsigned() || !pair[1].is_number_unsigned()) continue;
            const auto s = pair[0].get<std::size_t>();
            const auto u = pair[1].get<std::size_t>();
            if (s < database.sets.size() && u < database.sets[s].units.size()) database.sets[s].units[u].optionsDerived = true;
        }
    }
}

// The envelope's version. 3 (0.0.7) added `optionsDerived`; a version-2 cache is ignored rather than read
// with every derived option taken for a stated one.
constexpr int CACHE_VERSION { 3 };

} // namespace

// Which options the S1 library derived from the arguments (spec::complete_options) rather than the
// producer stated (issue #30). The S1 document itself has no place for this, and must not: it says what a
// build is, not how this server completed it. Without it the options read back look stated, the plan
// builds the engine's arguments from them instead of the producer's (reordered, -O and -g dropped), and
// every BMI clangd keyed on the cold start's commands is rebuilt on the warm one.
Json derived_options_to_json(const spec::Database& database) {
    Json sets = Json::array();
    Json units = Json::array();
    for (std::size_t s { 0 }; s < database.sets.size(); ++s) {
        if (database.sets[s].optionsDerived) sets.push_back(s);
        for (std::size_t u { 0 }; u < database.sets[s].units.size(); ++u) {
            if (database.sets[s].units[u].optionsDerived) units.push_back(Json::array({ s, u }));
        }
    }
    return Json { { "sets", std::move(sets) }, { "units", std::move(units) } };
}

std::string cache_file_name(SourceKind source) {
    return std::format("model.{}.json", to_string(source));
}

Json model_to_json(const ProjectModel& model) {
    Json facts = Json::object();
    for (const auto& [driver, driverFacts] : model.facts) facts[driver] = toolchain::facts_to_json(driverFacts);
    return Json {
        { "root", model.root },
        { "source", std::string { to_string(model.source) } },
        { "detected", std::string { to_string(model.detected) } },
        { "level", model.level },
        { "tier", model.tier },
        { "usesKit", model.usesKit },
        { "watch", model.watch },
        { "issues", issues_to_json(model.issues) },
        { "notices", issues_to_json(model.notices) },
        { "profile", Json { { "kind", model.profile.kind }, { "compiler", model.profile.compiler },
                            { "stdlib", model.profile.stdlib }, { "target", model.profile.target } } },
        { "facts", std::move(facts) },
        { "database", Json::parse(spec::to_json(model.database).dump()) },
        { "optionsDerived", derived_options_to_json(model.database) },
    };
}

base::Result<ProjectModel> model_from_json(const Json& value) {
    if (!value.is_object()) return base::fail("model-cache", "the cached model is not an object");
    const auto database = value.find("database");
    if (database == value.end()) return base::fail("model-cache", "the cached model has no database");
    auto loaded = spec::from_json(*database);
    if (!loaded) return std::unexpected { loaded.error() };

    ProjectModel model;
    model.root = value.value("root", std::string {});
    model.source = source_from_name(value.value("source", std::string {})).value_or(SourceKind::inferred);
    model.detected = source_from_name(value.value("detected", std::string {})).value_or(model.source);
    model.level = value.value("level", 2);
    // Cached with the model: it depends on how the model was obtained, which `source` alone does not say.
    // A cache written before the field existed falls back to what its source promises.
    model.tier = value.value("tier", tier_of(model.source));
    model.usesKit = value.value("usesKit", false);
    model.database = std::move(*loaded);
    derived_from_json(value.value("optionsDerived", Json::object()), model.database);
    if (const auto watch = value.find("watch"); watch != value.end() && watch->is_array()) {
        for (const auto& entry : *watch) {
            if (entry.is_string()) model.watch.push_back(entry.get<std::string>());
        }
    }
    model.issues = issues_from_json(value.value("issues", Json::array()));
    model.notices = issues_from_json(value.value("notices", Json::array()));
    if (const auto profile = value.find("profile"); profile != value.end() && profile->is_object()) {
        model.profile.kind = profile->value("kind", std::string {});
        model.profile.compiler = profile->value("compiler", std::string {});
        model.profile.stdlib = profile->value("stdlib", std::string {});
        model.profile.target = profile->value("target", std::string {});
    }
    if (const auto facts = value.find("facts"); facts != value.end() && facts->is_object()) {
        for (const auto& entry : facts->items()) {
            if (auto parsed = toolchain::facts_from_json(entry.value())) model.facts.emplace(entry.key(), std::move(*parsed));
        }
    }
    return model;
}

std::string inputs_fingerprint(std::string_view root, std::span<const std::string> watch, std::string_view manifest,
                               std::string_view producer, std::string_view producerVersion) {
    std::vector<std::string> paths;
    if (!manifest.empty()) paths.emplace_back(manifest);
    for (const auto& entry : watch) {
        // A watch entry is a path or a glob relative to the root. A glob has no stamp of its own;
        // it goes into the fingerprint as text, which at least notices the producer changing it.
        paths.push_back(base::is_absolute_path(entry) ? entry : base::join_path(root, entry));
    }
    std::ranges::sort(paths);
    paths.erase(std::ranges::unique(paths).begin(), paths.end());

    std::string text;
    text += std::format("producer\t{}\t{}\n", producer, producerVersion);
    if (!producer.empty()) {
        if (auto stamp = platform::fs::stamp(producer)) {
            text += std::format("producer-file\t{}\t{}\n", stamp->size, stamp->modified);
        }
    }
    for (const auto& path : paths) {
        if (auto stamp = platform::fs::stamp(path)) {
            text += std::format("{}\t{}\t{}\n", path, stamp->size, stamp->modified);
        } else {
            text += std::format("{}\tabsent\n", path);
        }
    }
    return base::sha256_hex(text);
}

base::Result<void> save_model(std::string_view directory, const CachedModel& cached) {
    Json envelope {
        { "version", CACHE_VERSION },
        { "fingerprint", cached.fingerprint },
        { "producer", cached.producer },
        { "producerVersion", cached.producerVersion },
        { "savedAtMs", cached.savedAtMs },
        { "producerMs", cached.producerMs },
        { "model", model_to_json(cached.model) },
    };
    const std::string path { base::join_path(directory, cache_file_name(cached.model.source)) };
    return platform::fs::write_file_atomic(path, envelope.dump());
}

std::optional<CachedModel> load_model(std::string_view directory, SourceKind source) {
    const std::string path { base::join_path(directory, cache_file_name(source)) };
    auto text = platform::fs::read_file(path);
    if (!text) return std::nullopt;
    const Json envelope = Json::parse(*text, nullptr, false);
    if (envelope.is_discarded() || !envelope.is_object() || envelope.value("version", 0) != CACHE_VERSION) return std::nullopt;
    const auto model = envelope.find("model");
    if (model == envelope.end()) return std::nullopt;
    auto parsed = model_from_json(*model);
    if (!parsed) {
        base::log::info("the cached {} model cannot be read ({}); it is ignored", to_string(source), parsed.error().message);
        return std::nullopt;
    }
    if (parsed->source != source) return std::nullopt;
    CachedModel cached;
    cached.model = std::move(*parsed);
    cached.fingerprint = envelope.value("fingerprint", std::string {});
    cached.producer = envelope.value("producer", std::string {});
    cached.producerVersion = envelope.value("producerVersion", std::string {});
    cached.savedAtMs = envelope.value("savedAtMs", std::int64_t { 0 });
    cached.producerMs = envelope.value("producerMs", std::int64_t { 0 });
    return cached;
}

} // namespace mcppls::project
