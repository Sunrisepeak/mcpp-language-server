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

export namespace mcppls::project {

struct CachedModel {
    ProjectModel model;
    std::string fingerprint;         // of the inputs, when it was saved
    std::string producer;            // the program that produced it, when there was one
    std::string producerVersion;
    std::int64_t savedAtMs { 0 };    // wall clock, for "how old is this"
};

// "model.mcpp.json", "model.cmake.json", ... One file per source: a worse source never overwrites
// a better one's cache (design P5).
std::string cache_file_name(SourceKind source);

nlohmann::json model_to_json(const ProjectModel& model);
base::Result<ProjectModel> model_from_json(const nlohmann::json& value);

// What the producer read, as one string. `watch` is what the producer said it depends on; the
// manifest is added because a project always depends on it even before a producer has spoken.
std::string inputs_fingerprint(std::string_view root, std::span<const std::string> watch, std::string_view manifest,
                               std::string_view producer, std::string_view producerVersion);

base::Result<void> save_model(std::string_view directory, const CachedModel& cached);
// nullopt when there is no such cache, or it cannot be read: a cache that cannot be read is no cache.
std::optional<CachedModel> load_model(std::string_view directory, SourceKind source);

} // namespace mcppls::project
