// The syntactic module index: module-level language features answered without
// the engine (design section 8.5) — module-name navigation, import completion,
// module hover, outline and workspace symbols, and module diagnostics.
export module mcppls.engine.native.index;

import std;
import nlohmann.json;
import mcppls.base.text;
import mcppls.spec.database;
import mcppls.spec.metadata;
import mcppls.project.scan;

export namespace mcppls::index {

struct ModuleUnit {
    std::string path;
    std::string name;          // "m" or "m:p"
    spec::Role role { spec::Role::unknown };
    base::Range declaration;   // the name in the module declaration
};

struct ExternalModule {
    std::string name;
    std::string path;          // source of the module unit, e.g. std.cppm
    std::string origin;        // stdlib | module-metadata
};

// The configurations that compile a unit of the model (mcpp-community/mcpp#797): the targets', the build programs'
// (`build.mcpp`, the libraries of `[build-dependencies]`, mcpp's engine interface), or both, for a package in both
// tables. 0 for a file no set describes.
inline constexpr unsigned TARGETS { 1 };
inline constexpr unsigned BUILD_PROGRAMS { 2 };

struct ModuleHit {
    std::string name;          // fully qualified, "m:p" for partitions
    base::Range range;
    bool isDeclaration { false };
};

class ModuleIndex {
private:
    std::map<std::string, std::pair<std::string, project::ScanResult>, std::less<>> files_;   // path key -> (path, scan)
    std::vector<ExternalModule> external_;
    std::string profileLabel_;
    // What import completion offers (fix plan 2026-09-26 F9, D4): every module name with what
    // provides it, built once and kept until a module is declared, dropped or changes role --
    // not on every edit, which leaves the declarations as they were.
    struct Candidate {
        std::string name;     // "m" or "m:p"
        std::string detail;   // the role of the unit that provides it, or where an external module comes from
        unsigned configurations { 0 };   // of its providers; 0 for an external module, which every configuration sees
    };
    std::map<std::string, unsigned, std::less<>> configurations_;   // path key -> TARGETS | BUILD_PROGRAMS
    std::set<std::string, std::less<>> engineInterface_;            // path keys of mcpp's engine interface
    mutable std::optional<std::vector<Candidate>> candidates_;
    std::uint64_t structure_ { 0 };
    const std::vector<Candidate>& candidates_now_() const;
    // The code and message of an import of `name` whose providers, `units`, are all of the configuration `importer`
    // is not compiled in; nullopt when the import is within a configuration or either is unknown.
    std::optional<std::pair<std::string_view, std::string>> across_configurations(std::string_view importer, std::string_view name,
                                                                                  std::span<const ModuleUnit> units) const;
    void structure_changed_();

public:
    void update(std::string_view path, std::string_view text);
    void remove(std::string_view path);
    void clear();
    void set_external(std::vector<ExternalModule> modules);
    void set_profile_label(std::string label);
    // Which configurations compile each unit, by path key, and which units are mcpp's engine interface. A unit sees
    // the modules its own configuration provides: an import of a module only the other configuration provides is
    // diagnosed, and import completion does not offer one.
    void set_configurations(std::map<std::string, unsigned, std::less<>> byPathKey, std::set<std::string, std::less<>> engineInterface);
    unsigned configuration_of(std::string_view path) const;

    bool contains(std::string_view path) const;
    const project::ScanResult* scan_of(std::string_view path) const;
    std::vector<std::string> files() const;
    std::vector<ModuleUnit> providers(std::string_view name) const;
    // The providers a unit of `importer` sees: those of its configuration, when it has one and they do.
    std::vector<ModuleUnit> providers_for(std::string_view importer, std::string_view name) const;
    const ExternalModule* external(std::string_view name) const;
    std::vector<std::string> module_names() const;
    // Advanced whenever the set of declared modules, or the role of a unit declaring one, changes.
    std::uint64_t structure_generation() const { return structure_; }

    std::optional<ModuleHit> module_at(std::string_view path, base::Position position) const;
    // Each returns null when the position is not one this index answers for.
    nlohmann::json definition(std::string_view path, base::Position position) const;
    nlohmann::json hover(std::string_view path, base::Position position) const;
    nlohmann::json completion(std::string_view path, std::string_view text, base::Position position) const;

    nlohmann::json diagnostics(std::string_view path) const;
    nlohmann::json document_symbols(std::string_view path) const;
    nlohmann::json workspace_symbols(std::string_view query) const;
    nlohmann::json graph() const;
    nlohmann::json module_info(std::string_view name) const;
};

// External modules named by the entries of P3286 manifests; the first manifest naming a module wins.
std::vector<ExternalModule> external_modules(std::span<const std::pair<std::string, std::string>> manifests,
                                             const std::function<std::vector<spec::ModuleEntry>(std::string_view)>& reader);

nlohmann::json to_json(const base::Range& range);
nlohmann::json make_location(std::string_view path, const base::Range& range);

} // namespace mcppls::index
