module mcppls.ai.context.interface;

import std;
import nlohmann.json;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.spec.database;
import mcppls.spec.query;
import mcppls.project.scan;
import mcppls.engine.native.index;
import mcppls.engine.native.exports;
import mcppls.orchestrator.kernel;
import mcppls.orchestrator.workspace;
import mcppls.ai.query.view;
import mcppls.ai.query.symbols;

namespace mcppls::ai::context {

namespace {

using Json = nlohmann::json;

std::string kind_name(index::DeclarationKind kind) {
    switch (kind) {
    case index::DeclarationKind::function: return "function";
    case index::DeclarationKind::class_type: return "class";
    case index::DeclarationKind::struct_type: return "struct";
    case index::DeclarationKind::union_type: return "union";
    case index::DeclarationKind::enum_type: return "enum";
    case index::DeclarationKind::concept_: return "concept";
    case index::DeclarationKind::alias: return "alias";
    case index::DeclarationKind::variable: return "variable";
    case index::DeclarationKind::namespace_: return "namespace";
    case index::DeclarationKind::reexport: return "reexport";
    case index::DeclarationKind::other: return "other";
    }
    return "other";
}

std::string qualified_unit_name(const project::ModuleDeclaration& declaration) {
    return declaration.partition.empty() ? declaration.module : declaration.module + ":" + declaration.partition;
}

// The interface unit (interface or partition interface) providing `name`, if the workspace has one.
std::optional<std::string> interface_unit(const index::ModuleIndex& moduleIndex, std::string_view name) {
    for (const auto& unit : moduleIndex.providers(name)) {
        if (unit.role == spec::Role::module_interface || unit.role == spec::Role::module_partition_interface) return unit.path;
    }
    return std::nullopt;
}

std::vector<InterfaceDeclaration> declarations_of(query::View& view, const std::string& path, const std::string& unitName, std::vector<std::string>& reexports,
                                                  std::vector<std::string>& partitions) {
    std::vector<InterfaceDeclaration> declarations;
    const std::string text { view.text_of(path) };
    for (const auto& exported : index::exported_declarations(text)) {
        if (exported.kind == index::DeclarationKind::reexport) {
            // `export import :p;` brings the partition's exports; `export import m;` another module.
            if (exported.name.starts_with(':')) partitions.push_back(unitName.substr(0, unitName.find(':')) + exported.name);
            else reexports.push_back(exported.name);
            continue;
        }
        // A namespace is in every qualified name already; listing it again spends the budget on nothing.
        if (exported.kind == index::DeclarationKind::namespace_) continue;
        InterfaceDeclaration declaration;
        declaration.kind = kind_name(exported.kind);
        declaration.name = exported.name;
        declaration.qualifiedName = exported.qualifiedName;
        declaration.declaration = exported.declaration;
        declaration.documentation = exported.documentation;
        declaration.unit = unitName;
        declaration.location = spec::location_in(text, view.display(path), exported.nameRange.start, exported.nameRange.end);
        declaration.conditional = exported.conditional;
        declarations.push_back(std::move(declaration));
    }
    return declarations;
}

} // namespace

query::Outcome<ModuleInterface> module_interface(query::View& view, std::string_view module, std::size_t maxCharacters) {
    view.refresh();
    const auto& moduleIndex = view.kernel().workspace().module_index();
    const std::string primary { module.substr(0, module.find(':')) };
    ModuleInterface interface;
    interface.module = std::string { module };
    const auto primaryUnit = interface_unit(moduleIndex, module);
    if (!primaryUnit) {
        if (moduleIndex.external(primary) != nullptr) {
            return std::unexpected { query::Failure { "unavailable", std::format("{} is provided by the toolchain, not the workspace; its interface is not summarized", primary), nullptr } };
        }
        return std::unexpected { query::not_found(std::format("no interface unit provides {}", module)) };
    }
    // The primary interface, then every partition it re-exports, transitively, each once.
    std::vector<std::pair<std::string, std::string>> pending { { *primaryUnit, std::string { module } } };
    std::set<std::string> seen { std::string { module } };
    std::vector<InterfaceDeclaration> all;
    while (!pending.empty()) {
        auto [path, unitName] = pending.front();
        pending.erase(pending.begin());
        interface.files.push_back(view.display(path));
        std::vector<std::string> partitions;
        auto declarations = declarations_of(view, path, unitName, interface.reexports, partitions);
        std::ranges::move(declarations, std::back_inserter(all));
        for (const auto& partition : partitions) {
            if (!seen.insert(partition).second) continue;
            if (auto unit = interface_unit(moduleIndex, partition)) pending.emplace_back(*unit, partition);
        }
    }
    interface.total = all.size();
    std::size_t characters { 0 };
    for (const auto& declaration : all) characters += declaration.declaration.size() + declaration.documentation.size();
    if (characters > maxCharacters) {
        interface.documentationOmitted = std::ranges::any_of(all, [](const auto& declaration) { return !declaration.documentation.empty(); });
        for (auto& declaration : all) declaration.documentation.clear();
    }
    std::size_t used { 0 };
    for (auto& declaration : all) {
        used += declaration.declaration.size();
        if (used > maxCharacters && !interface.declarations.empty()) {
            interface.truncated = true;
            break;
        }
        interface.declarations.push_back(std::move(declaration));
    }
    std::ranges::sort(interface.reexports);
    interface.reexports.erase(std::unique(interface.reexports.begin(), interface.reexports.end()), interface.reexports.end());
    interface.snapshot = view.snapshot();
    return interface;
}

std::vector<InterfaceDeclaration> exported_named(query::View& view, std::string_view name) {
    const auto& moduleIndex = view.kernel().workspace().module_index();
    std::vector<InterfaceDeclaration> found;
    for (const auto& path : moduleIndex.files()) {
        const auto* scan = moduleIndex.scan_of(path);
        if (scan == nullptr || !scan->declaration || !scan->declaration->isExported) continue;
        const std::string text { view.text_of(path) };
        for (const auto& exported : index::exported_declarations(text)) {
            if (exported.kind == index::DeclarationKind::reexport || (exported.name != name && exported.qualifiedName != name)) continue;
            InterfaceDeclaration declaration;
            declaration.kind = kind_name(exported.kind);
            declaration.name = exported.name;
            declaration.qualifiedName = exported.qualifiedName;
            declaration.declaration = exported.declaration;
            declaration.documentation = exported.documentation;
            declaration.unit = qualified_unit_name(*scan->declaration);
            declaration.location = spec::location_in(text, view.display(path), exported.nameRange.start, exported.nameRange.end);
            declaration.conditional = exported.conditional;
            found.push_back(std::move(declaration));
        }
    }
    std::ranges::sort(found, {}, [](const InterfaceDeclaration& d) { return std::tuple { d.unit, d.location.file, d.location.line }; });
    return found;
}

query::Outcome<query::Symbols> locate_symbols(query::View& view, const query::SymbolTarget& target, query::Limit limit, bool describe,
                                              query::Clock::time_point deadline) {
    auto found = query::find_symbols(view, target, limit, describe, deadline);
    const bool byName { !target.name.empty() && target.id.empty() && target.line == 0 };
    const bool nothing { found ? found->symbols.empty() : found.error().code == "unavailable" };
    if (!byName || !nothing) return found;
    query::Symbols symbols;
    for (auto& declaration : exported_named(view, target.name)) {
        if (!target.kind.empty() && declaration.kind != target.kind) continue;
        if (!target.module.empty() && declaration.unit != target.module && !declaration.unit.starts_with(target.module + ":")) continue;
        ++symbols.total;
        if (symbols.symbols.size() >= limit.maxResults) continue;
        query::Symbol symbol;
        symbol.name = declaration.name;
        symbol.qualifiedName = declaration.qualifiedName;
        symbol.kind = declaration.kind;
        symbol.module = declaration.unit;
        symbol.declaration = declaration.location;
        symbol.signature = declaration.declaration;
        symbol.documentation = declaration.documentation;
        symbols.symbols.push_back(std::move(symbol));
    }
    symbols.truncated = symbols.total > symbols.symbols.size();
    symbols.snapshot = view.snapshot();
    return symbols;
}

Json to_json(const InterfaceDeclaration& declaration) {
    Json value { { "kind", declaration.kind }, { "name", declaration.name }, { "qualifiedName", declaration.qualifiedName },
                 { "declaration", declaration.declaration }, { "unit", declaration.unit }, { "location", spec::to_json(declaration.location) } };
    if (!declaration.documentation.empty()) value["documentation"] = declaration.documentation;
    if (declaration.conditional) value["conditional"] = true;
    return value;
}

Json to_json(const ModuleInterface& interface) {
    Json declarations = Json::array();
    for (const auto& declaration : interface.declarations) declarations.push_back(to_json(declaration));
    return Json { { "snapshot", spec::to_json(interface.snapshot) }, { "module", interface.module }, { "files", interface.files },
                  { "reexports", interface.reexports }, { "declarations", std::move(declarations) }, { "total", interface.total },
                  { "truncated", interface.truncated }, { "documentationOmitted", interface.documentationOmitted } };
}

} // namespace mcppls::ai::context
