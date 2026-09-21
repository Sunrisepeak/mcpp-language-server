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

public:
    void update(std::string_view path, std::string_view text);
    void remove(std::string_view path);
    void clear();
    void set_external(std::vector<ExternalModule> modules);
    void set_profile_label(std::string label);

    bool contains(std::string_view path) const;
    const project::ScanResult* scan_of(std::string_view path) const;
    std::vector<std::string> files() const;
    std::vector<ModuleUnit> providers(std::string_view name) const;
    const ExternalModule* external(std::string_view name) const;
    std::vector<std::string> module_names() const;

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
