module mcppls.ai.query.modules;

import std;
import nlohmann.json;
import mcppls.base.path;
import mcppls.spec.database;
import mcppls.spec.query;
import mcppls.project.model;
import mcppls.project.scan;
import mcppls.engine.native.index;
import mcppls.orchestrator.kernel;
import mcppls.orchestrator.workspace;
import mcppls.ai.query.view;

namespace mcppls::ai::query {

namespace {

std::string qualified_name(const project::ModuleDeclaration& declaration) {
    return declaration.partition.empty() ? declaration.module : declaration.module + ":" + declaration.partition;
}

std::map<std::string, std::vector<std::string>> sets_by_path(View& view) {
    std::map<std::string, std::vector<std::string>> sets;
    const auto model = view.kernel().workspace().project_model();
    if (!model) return sets;
    for (const auto& set : model->database.sets) {
        for (const auto& unit : set.units) sets[base::path_key(spec::absolute_source(unit))].push_back(set.name);
    }
    return sets;
}

void sort_unique(std::vector<std::string>& values) {
    std::ranges::sort(values);
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

} // namespace

std::vector<std::string> importers_of(View& view, std::string_view module) {
    const auto& index = view.kernel().workspace().module_index();
    std::vector<std::string> files;
    for (const auto& path : index.files()) {
        const auto* scan = index.scan_of(path);
        if (scan == nullptr) continue;
        for (const auto& import : scan->imports) {
            if (import.isHeaderUnit) continue;
            if (project::imported_name(*scan, import) == module) {
                files.push_back(path);
                break;
            }
        }
    }
    std::ranges::sort(files);
    return files;
}

Neighbourhood module_neighbourhood(View& view, std::string_view module, std::size_t budget) {
    const auto& index = view.kernel().workspace().module_index();
    const std::string primary { module.substr(0, module.find(':')) };
    std::vector<std::string> ordered;
    std::set<std::string> seen;
    auto add = [&](const std::string& path) {
        if (seen.insert(base::path_key(path)).second) ordered.push_back(path);
    };
    // The module's own units first: its partitions and implementation units use its names too.
    for (const auto& path : index.files()) {
        const auto* scan = index.scan_of(path);
        if (scan != nullptr && scan->declaration && scan->declaration->module == primary) add(path);
    }
    // Then importers, breadth first through modules that re-export what they import.
    std::deque<std::string> modules { primary };
    std::set<std::string> visited { primary };
    while (!modules.empty()) {
        const std::string current { modules.front() };
        modules.pop_front();
        for (const auto& path : importers_of(view, current)) {
            add(path);
            const auto* scan = index.scan_of(path);
            if (scan == nullptr || !scan->declaration || scan->declaration->module == current) continue;
            const bool reexports { std::ranges::any_of(scan->imports, [&](const auto& import) {
                return import.isExported && !import.isHeaderUnit && project::imported_name(*scan, import) == current;
            }) };
            if (reexports && visited.insert(scan->declaration->module).second) modules.push_back(scan->declaration->module);
        }
    }
    Neighbourhood neighbourhood;
    for (auto& path : ordered) {
        if (neighbourhood.files.size() < budget) neighbourhood.files.push_back(std::move(path));
        else neighbourhood.left.push_back(std::move(path));
    }
    return neighbourhood;
}

Outcome<ModuleDescription> describe_module(View& view, std::string_view name, std::string_view file) {
    view.refresh();
    const auto& index = view.kernel().workspace().module_index();
    std::string named { name };
    if (named.empty()) {
        if (file.empty()) return std::unexpected { invalid_arguments("name a module, or a file of one") };
        named = view.module_of(view.path_of(file));
        if (named.empty()) return std::unexpected { not_found(std::format("{} is not a module unit", file)) };
    }
    // A partition is described as part of its module.
    const std::string primary { named.substr(0, named.find(':')) };

    ModuleDescription description;
    description.name = primary;
    const auto sets = sets_by_path(view);
    for (const auto& path : index.files()) {
        const auto* scan = index.scan_of(path);
        if (scan == nullptr || !scan->declaration || scan->declaration->module != primary) continue;
        ModuleUnitInfo unit;
        unit.file = view.display(path);
        unit.name = qualified_name(*scan->declaration);
        unit.role = std::string { spec::to_string(project::role_of(*scan)) };
        if (const auto found = sets.find(base::path_key(path)); found != sets.end()) unit.sets = found->second;
        if (!scan->declaration->partition.empty()) description.partitions.push_back(unit.name);
        for (const auto& required : project::required_names(*scan)) {
            if (required != primary && !required.starts_with(primary + ":")) description.imports.push_back(required);
        }
        if (scan->declaration->partition.empty() && scan->declaration->isExported) {
            for (const auto& import : scan->imports) {
                if (import.isExported && import.module.empty() && !import.partition.empty()) description.exportedPartitions.push_back(primary + ":" + import.partition);
            }
        }
        description.units.push_back(std::move(unit));
    }
    if (description.units.empty()) {
        if (const auto* external = index.external(primary)) {
            description.external = true;
            description.resolvedFrom = external->origin == "stdlib" ? "stdlib" : "module-metadata";
            description.units.push_back(ModuleUnitInfo { external->path, primary, "module-interface", {} });
        } else {
            return std::unexpected { not_found(std::format("no module named {}", primary)) };
        }
    } else {
        description.resolvedFrom = "set";
    }
    std::ranges::sort(description.units, {}, [](const ModuleUnitInfo& unit) { return std::pair { unit.name, unit.file }; });
    for (const auto& path : importers_of(view, primary)) {
        const std::string importer { view.module_of(path) };
        if (importer.empty()) description.importingFiles.push_back(view.display(path));
        else if (importer.substr(0, importer.find(':')) != primary) description.importedBy.push_back(importer.substr(0, importer.find(':')));
    }
    sort_unique(description.partitions);
    sort_unique(description.exportedPartitions);
    sort_unique(description.imports);
    sort_unique(description.importedBy);
    sort_unique(description.importingFiles);
    description.snapshot = view.snapshot();
    return description;
}

ModuleGraph module_graph(View& view, std::string_view filter, Limit limit) {
    view.refresh();
    const auto& index = view.kernel().workspace().module_index();
    ModuleGraph graph;
    std::set<std::string> included;
    for (const auto& name : index.module_names()) {
        if (!filter.empty() && name.find(filter) == std::string::npos) continue;
        GraphNode node;
        node.name = name;
        for (const auto& unit : index.providers(name)) node.files.push_back(view.display(unit.path));
        if (node.files.empty()) {
            if (const auto* external = index.external(name)) {
                node.external = true;
                node.files.push_back(external->path);
            }
        }
        included.insert(name);
        graph.modules.push_back(std::move(node));
    }
    std::set<std::pair<std::string, std::string>> edges;
    for (const auto& path : index.files()) {
        const auto* scan = index.scan_of(path);
        if (scan == nullptr) continue;
        const std::string from { scan->declaration ? qualified_name(*scan->declaration) : view.display(path) };
        for (const auto& import : scan->imports) {
            if (import.isHeaderUnit) continue;
            const std::string to { project::imported_name(*scan, import) };
            if (!filter.empty() && !included.contains(to) && !(scan->declaration && included.contains(from))) continue;
            edges.emplace(from, to);
        }
    }
    for (const auto& [from, to] : edges) graph.imports.push_back(GraphEdge { from, to });
    graph.total = graph.modules.size();
    graph.truncated = graph.modules.size() > limit.maxResults;
    if (graph.truncated) graph.modules.resize(limit.maxResults);
    graph.snapshot = view.snapshot();
    return graph;
}

Json to_json(const ModuleDescription& description) {
    Json units = Json::array();
    for (const auto& unit : description.units) {
        Json value { { "file", unit.file }, { "name", unit.name }, { "role", unit.role } };
        if (!unit.sets.empty()) value["sets"] = unit.sets;
        units.push_back(std::move(value));
    }
    return Json { { "snapshot", spec::to_json(description.snapshot) },
                  { "name", description.name },
                  { "external", description.external },
                  { "resolvedFrom", description.resolvedFrom },
                  { "units", std::move(units) },
                  { "partitions", description.partitions },
                  { "exportedPartitions", description.exportedPartitions },
                  { "imports", description.imports },
                  { "importedBy", description.importedBy },
                  { "importingFiles", description.importingFiles } };
}

Json to_json(const ModuleGraph& graph) {
    Json modules = Json::array();
    for (const auto& node : graph.modules) modules.push_back(Json { { "name", node.name }, { "external", node.external }, { "files", node.files } });
    Json imports = Json::array();
    for (const auto& edge : graph.imports) imports.push_back(Json { { "from", edge.from }, { "to", edge.to } });
    return Json { { "snapshot", spec::to_json(graph.snapshot) }, { "modules", std::move(modules) }, { "imports", std::move(imports) },
                  { "total", graph.total }, { "truncated", graph.truncated } };
}

} // namespace mcppls::ai::query
