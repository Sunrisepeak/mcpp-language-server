module mcppls.ai.review.semantic;

import std;
import nlohmann.json;
import mcppls.base.text;
import mcppls.spec.database;
import mcppls.spec.query;
import mcppls.project.scan;
import mcppls.engine.native.exports;

namespace mcppls::ai::review {

namespace {

using Json = nlohmann::json;

struct Side {
    std::string text;
    project::ScanResult scan;
    std::vector<index::ExportedDeclaration> exports;
};

Side read_side(const std::optional<std::string>& text) {
    Side side;
    if (!text) return side;
    side.text = *text;
    side.scan = project::scan_source(side.text);
    side.exports = index::exported_declarations(side.text);
    return side;
}

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

spec::Location location_of(const Side& side, const std::string& display, const mcppls::base::Range& range) {
    return spec::location_in(side.text, display, range.start, range.end);
}

std::string role_of(const Side& side, bool present) {
    return present ? std::string { spec::to_string(project::role_of(side.scan)) } : std::string {};
}

} // namespace

std::string_view to_string(ExportChangeKind kind) {
    switch (kind) {
    case ExportChangeKind::added: return "added";
    case ExportChangeKind::removed: return "removed";
    case ExportChangeKind::changed: return "changed";
    }
    return "changed";
}

bool UnitDiff::interface_changed() const {
    return baseModule != headModule || !exports.empty() || std::ranges::any_of(imports, [](const ImportChange& import) { return import.exported; });
}

UnitDiff semantic_diff(std::string display, const std::optional<std::string>& baseText, const std::optional<std::string>& headText) {
    const Side base { read_side(baseText) };
    const Side head { read_side(headText) };
    UnitDiff diff;
    diff.file = display;
    diff.baseModule = baseText ? project::provided_name(base.scan) : std::string {};
    diff.headModule = headText ? project::provided_name(head.scan) : std::string {};
    // An implementation unit provides nothing, but it belongs to its module all the same.
    if (diff.baseModule.empty() && base.scan.declaration) diff.baseModule = base.scan.declaration->module;
    if (diff.headModule.empty() && head.scan.declaration) diff.headModule = head.scan.declaration->module;
    diff.baseRole = role_of(base, baseText.has_value());
    diff.headRole = role_of(head, headText.has_value());

    // Imports, by the full name they import and whether they re-export it.
    const auto imports_of = [](const Side& side) {
        std::vector<std::tuple<std::string, bool, mcppls::base::Range>> imports;
        for (const auto& import : side.scan.imports) {
            if (import.isHeaderUnit) continue;
            imports.emplace_back(project::imported_name(side.scan, import), import.isExported, import.nameRange);
        }
        return imports;
    };
    const auto baseImports = imports_of(base);
    const auto headImports = imports_of(head);
    for (const auto& [name, exported, range] : headImports) {
        const bool before { std::ranges::any_of(baseImports, [&](const auto& other) { return std::get<0>(other) == name && std::get<1>(other) == exported; }) };
        if (!before) diff.imports.push_back(ImportChange { true, name, exported, location_of(head, display, range) });
    }
    for (const auto& [name, exported, range] : baseImports) {
        const bool after { std::ranges::any_of(headImports, [&](const auto& other) { return std::get<0>(other) == name && std::get<1>(other) == exported; }) };
        if (!after) diff.imports.push_back(ImportChange { false, name, exported, location_of(base, display, range) });
    }

    // Exported declarations: an identical declaration is unchanged; the rest of one name pair up in
    // order as changed; what is left was added or removed. Namespaces and re-exports are not declarations others call.
    const auto relevant = [](const index::ExportedDeclaration& declaration) {
        return declaration.kind != index::DeclarationKind::namespace_ && declaration.kind != index::DeclarationKind::reexport;
    };
    std::vector<const index::ExportedDeclaration*> removed;
    std::vector<const index::ExportedDeclaration*> added;
    for (const auto& declaration : base.exports) {
        if (!relevant(declaration)) continue;
        const bool same { std::ranges::any_of(head.exports, [&](const auto& other) {
            return relevant(other) && other.qualifiedName == declaration.qualifiedName && other.declaration == declaration.declaration;
        }) };
        if (!same) removed.push_back(&declaration);
    }
    for (const auto& declaration : head.exports) {
        if (!relevant(declaration)) continue;
        const bool same { std::ranges::any_of(base.exports, [&](const auto& other) {
            return relevant(other) && other.qualifiedName == declaration.qualifiedName && other.declaration == declaration.declaration;
        }) };
        if (!same) added.push_back(&declaration);
    }
    for (auto before = removed.begin(); before != removed.end();) {
        const auto after = std::ranges::find_if(added, [&](const auto* candidate) {
            return candidate->qualifiedName == (*before)->qualifiedName && candidate->kind == (*before)->kind;
        });
        if (after == added.end()) {
            ++before;
            continue;
        }
        diff.exports.push_back(ExportChange { ExportChangeKind::changed, (*before)->qualifiedName, (*before)->name, kind_name((*before)->kind),
                                              (*before)->declaration, (*after)->declaration, location_of(base, display, (*before)->nameRange),
                                              location_of(head, display, (*after)->nameRange) });
        added.erase(after);
        before = removed.erase(before);
    }
    for (const auto* declaration : removed) {
        diff.exports.push_back(ExportChange { ExportChangeKind::removed, declaration->qualifiedName, declaration->name, kind_name(declaration->kind),
                                              declaration->declaration, {}, location_of(base, display, declaration->nameRange), std::nullopt });
    }
    for (const auto* declaration : added) {
        diff.exports.push_back(ExportChange { ExportChangeKind::added, declaration->qualifiedName, declaration->name, kind_name(declaration->kind), {},
                                              declaration->declaration, std::nullopt, location_of(head, display, declaration->nameRange) });
    }
    std::ranges::sort(diff.exports, {}, [](const ExportChange& change) { return std::pair { change.qualifiedName, change.kind }; });
    return diff;
}

Json to_json(const UnitDiff& diff) {
    Json imports = Json::array();
    for (const auto& import : diff.imports) {
        imports.push_back(Json { { "change", import.added ? "added" : "removed" }, { "module", import.module }, { "exported", import.exported },
                                 { "location", spec::to_json(import.location) } });
    }
    Json exports = Json::array();
    for (const auto& change : diff.exports) {
        Json value { { "change", std::string { to_string(change.kind) } }, { "qualifiedName", change.qualifiedName }, { "kind", change.declarationKind } };
        if (!change.before.empty()) value["before"] = change.before;
        if (!change.after.empty()) value["after"] = change.after;
        if (change.baseLocation) value["baseLocation"] = spec::to_json(*change.baseLocation);
        if (change.headLocation) value["headLocation"] = spec::to_json(*change.headLocation);
        exports.push_back(std::move(value));
    }
    Json value { { "file", diff.file }, { "imports", std::move(imports) }, { "exports", std::move(exports) }, { "interfaceChanged", diff.interface_changed() } };
    if (!diff.baseModule.empty() || !diff.headModule.empty()) value["module"] = Json { { "before", diff.baseModule }, { "after", diff.headModule } };
    if (diff.baseRole != diff.headRole) value["role"] = Json { { "before", diff.baseRole }, { "after", diff.headRole } };
    else if (!diff.headRole.empty()) value["role"] = diff.headRole;
    return value;
}

} // namespace mcppls::ai::review
