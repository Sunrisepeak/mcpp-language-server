// The last model a producer gave, kept whole so the next session can use it before the producer
// has said anything (design 4.1).
//
// Two rules give this file its shape. One cache per source --- model.mcpp.json, model.cmake.json,
// model.inferred.json --- so a session that fell back to scanned sources cannot overwrite what the
// build tool said. And a fingerprint of everything the producer read, so a cache can say whether it
// is still current: the build description files with their size and modification time, the producer
// itself, and the few environment variables that decide which producer is found.
export module mcppls.project.modelcache;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.project.detect;
import mcppls.project.model;
import mcppls.spec.database;

export namespace mcppls::project {

// One input of the fingerprint as it was (0.0.13 W5, W6): a fingerprint says only that something changed; these say what,
// and whether only its modification time did (a checkout, a branch switch, a file restored), which leaves the model as it
// was. `contentHash` is kept for files of at most CONTENT_HASH_LIMIT bytes, the build description files.
struct InputStamp {
    std::string path;
    bool present { false };
    std::uint64_t size { 0 };
    std::int64_t modified { 0 };
    std::string contentHash;
    bool operator==(const InputStamp&) const = default;
};
inline constexpr std::uint64_t CONTENT_HASH_LIMIT { 1024 * 1024 };

struct CachedModel {
    ProjectModel model;
    std::string fingerprint;         // of the inputs, when it was saved
    std::vector<InputStamp> inputs;  // the same inputs, one by one (empty in a cache an older server wrote)
    std::string producer;            // the program that produced it, when there was one
    std::string producerVersion;
    std::int64_t savedAtMs { 0 };    // wall clock, for "how old is this"
    std::int64_t producerMs { 0 };   // how long the producer took to describe it (G-4: its next deadline); 0 unknown
};

// "model.mcpp.json", "model.cmake.json", ... One file per source: a worse source never overwrites
// a better one's cache (design P5).
std::string cache_file_name(SourceKind source);

nlohmann::json model_to_json(const ProjectModel& model);
// Which sets and units of `database` carry options the S1 library derived (spec::complete_options) rather
// than options the producer stated: the plan treats the two differently, so a model is described with it.
nlohmann::json derived_options_to_json(const spec::Database& database);
base::Result<ProjectModel> model_from_json(const nlohmann::json& value);

// What the producer read, as one string. `watch` is what the producer said it depends on; the
// manifest is added because a project always depends on it even before a producer has spoken.
std::string inputs_fingerprint(std::string_view root, std::span<const std::string> watch, std::string_view manifest,
                               std::string_view producer, std::string_view producerVersion);

// The inputs `inputs_fingerprint` covers, one stamp each, the producer's own file first.
std::vector<InputStamp> input_stamps(std::string_view root, std::span<const std::string> watch, std::string_view manifest,
                                     std::string_view producer);

// Whether a cached model still describes the project, input by input: an input whose size and modification time moved
// but whose content is the one the cache saw is unchanged (`touched`), and `changed` says what did change, first.
struct InputsVerdict {
    bool unchanged { false };
    std::size_t touched { 0 };
    std::string changed;
};
InputsVerdict compare_inputs(std::span<const InputStamp> cached, std::span<const InputStamp> current);

base::Result<void> save_model(std::string_view directory, const CachedModel& cached);
// nullopt when there is no such cache, or it cannot be read: a cache that cannot be read is no cache.
std::optional<CachedModel> load_model(std::string_view directory, SourceKind source);

} // namespace mcppls::project
