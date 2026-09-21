// Module queries (overall design 7.1, S5 3): what a module is made of, what it imports and who
// imports it, and the module graph, all from mcppls's own module index and the project model.
export module mcppls.ai.query.modules;

import std;
import nlohmann.json;
import mcppls.spec.query;
import mcppls.ai.query.view;

export namespace mcppls::ai::query {

struct ModuleUnitInfo {
    std::string file;
    std::string name;                  // "m" or "m:p"
    std::string role;
    std::vector<std::string> sets;     // the S1 sets that build the unit
};

struct ModuleDescription {
    spec::Snapshot snapshot;
    std::string name;
    bool external { false };           // provided by a standard library or module metadata, not by the workspace
    std::string resolvedFrom;          // set | stdlib | module-metadata
    std::vector<ModuleUnitInfo> units;           // the primary interface, partitions and implementation units
    std::vector<std::string> partitions;         // "m:p"
    std::vector<std::string> exportedPartitions; // re-exported by the primary interface
    std::vector<std::string> imports;            // modules its units import, its own partitions excepted
    std::vector<std::string> importedBy;         // modules whose units import it
    std::vector<std::string> importingFiles;     // units that import it and belong to no module
};

// A module by name, or the module of a file.
Outcome<ModuleDescription> describe_module(View& view, std::string_view name, std::string_view file);

struct GraphNode {
    std::string name;
    bool external { false };
    std::vector<std::string> files;
};

struct GraphEdge {
    std::string from;   // a module, or a file for a unit that belongs to no module
    std::string to;
};

struct ModuleGraph {
    spec::Snapshot snapshot;
    std::vector<GraphNode> modules;
    std::vector<GraphEdge> imports;
    std::size_t total { 0 };
    bool truncated { false };
};

// Modules whose name contains `filter` (all when empty), and the imports between them and into them.
ModuleGraph module_graph(View& view, std::string_view filter, Limit limit);

// The files that import a module (its own partitions' importers included), by canonical path.
std::vector<std::string> importers_of(View& view, std::string_view module);

// Where a module's names can be used: the units of the module, every unit that imports it, and the
// importers of modules that re-export it, nearest first. At most `budget` files; `complete` says
// whether that was all of them.
struct Neighbourhood {
    std::vector<std::string> files;
    std::vector<std::string> left;   // beyond the budget
};
Neighbourhood module_neighbourhood(View& view, std::string_view module, std::size_t budget);

nlohmann::json to_json(const ModuleDescription& description);
nlohmann::json to_json(const ModuleGraph& graph);

} // namespace mcppls::ai::query
