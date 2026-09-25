module mcppls.engine.native.index;

import std;
import nlohmann.json;
import mcppls.base.text;
import mcppls.base.path;
import mcppls.base.uri;
import mcppls.spec.database;
import mcppls.spec.metadata;
import mcppls.project.scan;

namespace mcppls::index {

using Json = nlohmann::json;

namespace {

constexpr int SYMBOL_KIND_MODULE { 2 };
constexpr int COMPLETION_KIND_MODULE { 9 };
constexpr int SEVERITY_ERROR { 1 };
constexpr int SEVERITY_WARNING { 2 };

std::string_view role_label(spec::Role role) {
    switch (role) {
    case spec::Role::module_interface: return "primary module interface unit";
    case spec::Role::module_partition_interface: return "module partition interface unit";
    case spec::Role::module_partition_implementation: return "module partition implementation unit";
    case spec::Role::module_implementation: return "module implementation unit";
    case spec::Role::non_module: return "non-module unit";
    case spec::Role::unknown: return "module unit";
    case spec::Role::header_unit: return "header unit";
    }
    return "module unit";
}

std::string declared_name(const project::ModuleDeclaration& declaration) {
    return declaration.partition.empty() ? declaration.module : declaration.module + ":" + declaration.partition;
}

Json position_json(base::Position position) { return Json { { "line", position.line }, { "character", position.character } }; }

Json diagnostic(const base::Range& range, int severity, std::string_view code, std::string message) {
    return Json { { "range", to_json(range) }, { "severity", severity }, { "code", std::string { code } },
                  { "source", "mcppls" }, { "message", std::move(message) } };
}

} // namespace

std::vector<ExternalModule> external_modules(std::span<const std::pair<std::string, std::string>> manifests,
                                             const std::function<std::vector<spec::ModuleEntry>(std::string_view)>& reader) {
    std::vector<ExternalModule> modules;
    for (const auto& [path, origin] : manifests) {
        for (const auto& entry : reader(path)) {
            if (std::ranges::none_of(modules, [&](const ExternalModule& module) { return module.name == entry.logicalName; })) {
                modules.push_back(ExternalModule { entry.logicalName, entry.source, origin });
            }
        }
    }
    return modules;
}

Json to_json(const base::Range& range) {
    return Json { { "start", position_json(range.start) }, { "end", position_json(range.end) } };
}

Json make_location(std::string_view path, const base::Range& range) {
    return Json { { "uri", base::path_to_uri(path) }, { "range", to_json(range) } };
}

namespace {

// What a file contributes to the module names import completion offers: the name it declares and
// the role it has. Anything else about it can change without changing those.
std::string provided_identity(const project::ScanResult& scan) {
    if (!scan.declaration) return {};
    return std::format("{}|{}", declared_name(*scan.declaration), spec::to_string(project::role_of(scan)));
}

} // namespace

void ModuleIndex::update(std::string_view path, std::string_view text) {
    auto scan = project::scan_source(text);
    auto& entry = files_[base::path_key(path)];
    if (provided_identity(entry.second) != provided_identity(scan)) structure_changed_();
    entry = { std::string { path }, std::move(scan) };
}

void ModuleIndex::remove(std::string_view path) {
    if (const auto it = files_.find(base::path_key(path)); it != files_.end()) {
        if (it->second.second.declaration) structure_changed_();
        files_.erase(it);
    }
}

void ModuleIndex::clear() {
    files_.clear();
    structure_changed_();
}

void ModuleIndex::set_external(std::vector<ExternalModule> modules) {
    external_ = std::move(modules);
    structure_changed_();
}

void ModuleIndex::structure_changed_() {
    ++structure_;
    candidates_.reset();
}

const std::vector<ModuleIndex::Candidate>& ModuleIndex::candidates_now_() const {
    if (candidates_) return *candidates_;
    std::map<std::string, std::string, std::less<>> byName;   // sorted by name, like module_names
    for (const auto& [key, entry] : files_) {
        const auto& scan = entry.second;
        if (!scan.declaration) continue;
        const spec::Role role { project::role_of(scan) };
        if (role == spec::Role::module_implementation) continue;
        byName.try_emplace(declared_name(*scan.declaration), role_label(role));   // the first provider's role, like providers().front()
    }
    for (const auto& module : external_) byName.try_emplace(module.name, "module");
    std::vector<Candidate> built;
    built.reserve(byName.size());
    for (auto& [name, detail] : byName) built.push_back(Candidate { name, std::move(detail) });
    candidates_ = std::move(built);
    return *candidates_;
}

void ModuleIndex::set_profile_label(std::string label) { profileLabel_ = std::move(label); }

bool ModuleIndex::contains(std::string_view path) const { return files_.contains(base::path_key(path)); }

const project::ScanResult* ModuleIndex::scan_of(std::string_view path) const {
    const auto it = files_.find(base::path_key(path));
    return it == files_.end() ? nullptr : &it->second.second;
}

std::vector<std::string> ModuleIndex::files() const {
    std::vector<std::string> result;
    for (const auto& [key, entry] : files_) result.push_back(entry.first);
    return result;
}

std::vector<ModuleUnit> ModuleIndex::providers(std::string_view name) const {
    std::vector<ModuleUnit> result;
    for (const auto& [key, entry] : files_) {
        const auto& scan = entry.second;
        if (!scan.declaration) continue;
        const spec::Role role { project::role_of(scan) };
        if (role == spec::Role::module_implementation) continue;
        if (declared_name(*scan.declaration) != name) continue;
        result.push_back(ModuleUnit { entry.first, std::string { name }, role, scan.declaration->nameRange });
    }
    return result;
}

const ExternalModule* ModuleIndex::external(std::string_view name) const {
    for (const auto& module : external_) {
        if (module.name == name) return &module;
    }
    return nullptr;
}

std::vector<std::string> ModuleIndex::module_names() const {
    std::set<std::string> names;
    for (const auto& [key, entry] : files_) {
        if (entry.second.declaration && project::role_of(entry.second) != spec::Role::module_implementation) {
            names.insert(declared_name(*entry.second.declaration));
        }
    }
    for (const auto& module : external_) names.insert(module.name);
    return { names.begin(), names.end() };
}

std::optional<ModuleHit> ModuleIndex::module_at(std::string_view path, base::Position position) const {
    const auto* scan = scan_of(path);
    if (scan == nullptr) return std::nullopt;
    if (scan->declaration && scan->declaration->nameRange.contains(position)) {
        return ModuleHit { declared_name(*scan->declaration), scan->declaration->nameRange, true };
    }
    for (const auto& import : scan->imports) {
        if (import.isHeaderUnit || !import.nameRange.contains(position)) continue;
        return ModuleHit { project::imported_name(*scan, import), import.nameRange, false };
    }
    return std::nullopt;
}

Json ModuleIndex::definition(std::string_view path, base::Position position) const {
    const auto hit = module_at(path, position);
    if (!hit) return nullptr;
    Json locations = Json::array();
    std::string target { hit->name };
    // An implementation unit's declaration names its primary interface.
    if (hit->isDeclaration) {
        const auto* scan = scan_of(path);
        if (scan != nullptr && project::role_of(*scan) != spec::Role::module_implementation) {
            locations.push_back(make_location(path, hit->range));
            return locations;
        }
    }
    for (const auto& unit : providers(target)) locations.push_back(make_location(unit.path, unit.declaration));
    if (locations.empty()) {
        if (const auto* module = external(target)) locations.push_back(make_location(module->path, base::Range {}));
    }
    return locations;
}

Json ModuleIndex::hover(std::string_view path, base::Position position) const {
    const auto hit = module_at(path, position);
    if (!hit) return nullptr;
    std::string value { std::format("```cpp\nmodule {}\n```\n", hit->name) };
    const auto units = providers(hit->name);
    if (!units.empty()) {
        for (const auto& unit : units) {
            value += std::format("\nProvided by `{}` ({}).\n", base::file_name(unit.path), role_label(unit.role));
        }
        if (units.size() > 1) value += "\n**Ambiguous:** more than one unit provides this module.\n";
    } else if (const auto* module = external(hit->name)) {
        value += std::format("\nProvided by `{}` ({}).\n", base::file_name(module->path), module->origin == "stdlib" ? "standard library" : "module metadata");
    } else {
        value += "\nNo unit provides this module.\n";
    }
    if (!profileLabel_.empty()) value += std::format("\nSemantic profile: {}\n", profileLabel_);
    return Json { { "contents", Json { { "kind", "markdown" }, { "value", value } } }, { "range", to_json(hit->range) } };
}

Json ModuleIndex::completion(std::string_view path, std::string_view text, base::Position position) const {
    const auto offset = base::offset_at(text, position);
    if (!offset) return nullptr;
    std::size_t lineStart { text.rfind('\n', *offset == 0 ? 0 : *offset - 1) };
    lineStart = (lineStart == std::string_view::npos || *offset == 0) ? 0 : lineStart + 1;
    std::string_view line { text.substr(lineStart, *offset - lineStart) };
    // [export] import <partial>, where only the left side is trimmed: the cursor may follow a space.
    auto trim_left = [](std::string_view text) {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
        return text;
    };
    std::string_view rest { trim_left(line) };
    if (rest.starts_with("export")) {
        rest.remove_prefix(6);
        if (rest.empty() || (rest.front() != ' ' && rest.front() != '\t')) return nullptr;
        rest = trim_left(rest);
    }
    if (!rest.starts_with("import")) return nullptr;
    rest.remove_prefix(6);
    if (rest.empty() || (rest.front() != ' ' && rest.front() != '\t')) return nullptr;
    const std::string_view partial { trim_left(rest) };
    if (!std::ranges::all_of(partial, [](char c) { return base::is_identifier_char(c) || c == '.' || c == ':'; })) return nullptr;
    const std::size_t partialStart { *offset - partial.size() };
    const base::Range editRange { base::position_at(text, partialStart), position };

    Json items = Json::array();
    auto add = [&](const std::string& label, std::string_view detail) {
        items.push_back(Json { { "label", label }, { "kind", COMPLETION_KIND_MODULE }, { "detail", std::string { detail } },
                               { "textEdit", Json { { "range", to_json(editRange) }, { "newText", label } } },
                               { "filterText", label } });
    };
    const auto* scan = scan_of(path);
    if (partial.starts_with(':')) {
        if (scan == nullptr || !scan->declaration) return Json { { "isIncomplete", false }, { "items", items } };
        const std::string owner { scan->declaration->module };
        for (const auto& candidate : candidates_now_()) {
            if (!candidate.name.starts_with(owner + ":")) continue;
            std::string label { candidate.name.substr(owner.size()) };
            if (label.starts_with(partial) && !(scan->declaration->partition.size() && label == ":" + scan->declaration->partition)) {
                add(label, "module partition");
            }
        }
    } else {
        const std::string self { scan != nullptr && scan->declaration ? declared_name(*scan->declaration) : std::string {} };
        for (const auto& candidate : candidates_now_()) {
            if (candidate.name.find(':') != std::string::npos || candidate.name == self) continue;
            if (!candidate.name.starts_with(partial)) continue;
            add(candidate.name, candidate.detail);
        }
    }
    return Json { { "isIncomplete", false }, { "items", items } };
}

Json ModuleIndex::diagnostics(std::string_view path) const {
    Json result = Json::array();
    const auto* scan = scan_of(path);
    if (scan == nullptr) return result;
    for (const auto& import : scan->imports) {
        if (import.isHeaderUnit) continue;
        if (import.module.empty() && !scan->declaration) {
            result.push_back(diagnostic(import.nameRange, SEVERITY_ERROR, "partition-outside-module",
                std::format("partition :{} can only be imported by a unit of its own module", import.partition)));
            continue;
        }
        const std::string name { project::imported_name(*scan, import) };
        const auto units = providers(name);
        if (units.empty() && external(name) == nullptr) {
            result.push_back(diagnostic(import.nameRange, SEVERITY_ERROR, "unresolved-module", std::format("module '{}' not found", name)));
        } else if (units.size() > 1) {
            std::string list;
            for (const auto& unit : units) list += (list.empty() ? "" : ", ") + std::string { base::file_name(unit.path) };
            result.push_back(diagnostic(import.nameRange, SEVERITY_WARNING, "ambiguous-module",
                std::format("module '{}' is provided by more than one unit: {}", name, list)));
        }
    }
    if (scan->declaration) {
        const std::string name { declared_name(*scan->declaration) };
        if (project::role_of(*scan) == spec::Role::module_implementation && providers(name).empty()) {
            result.push_back(diagnostic(scan->declaration->nameRange, SEVERITY_ERROR, "unresolved-module",
                std::format("module '{}' has no primary interface unit", name)));
        }
    }
    return result;
}

Json ModuleIndex::document_symbols(std::string_view path) const {
    Json result = Json::array();
    const auto* scan = scan_of(path);
    if (scan == nullptr || !scan->declaration) return result;
    const auto& declaration = *scan->declaration;
    result.push_back(Json { { "name", declared_name(declaration) }, { "detail", std::string { role_label(project::role_of(*scan)) } },
                            { "kind", SYMBOL_KIND_MODULE }, { "range", to_json(declaration.nameRange) },
                            { "selectionRange", to_json(declaration.nameRange) } });
    return result;
}

Json ModuleIndex::workspace_symbols(std::string_view query) const {
    Json result = Json::array();
    const std::string needle { base::to_lower_ascii(query) };
    for (const auto& [key, entry] : files_) {
        const auto& scan = entry.second;
        if (!scan.declaration || project::role_of(scan) == spec::Role::module_implementation) continue;
        const std::string name { declared_name(*scan.declaration) };
        if (!needle.empty() && base::to_lower_ascii(name).find(needle) == std::string::npos) continue;
        result.push_back(Json { { "name", name }, { "kind", SYMBOL_KIND_MODULE },
                                { "location", make_location(entry.first, scan.declaration->nameRange) }, { "containerName", "" } });
    }
    return result;
}

Json ModuleIndex::graph() const {
    Json modules = Json::array();
    for (const auto& name : module_names()) {
        Json units = Json::array();
        for (const auto& unit : providers(name)) {
            units.push_back(Json { { "uri", base::path_to_uri(unit.path) }, { "role", std::string { spec::to_string(unit.role) } } });
        }
        const auto* module = external(name);
        if (module != nullptr && units.empty()) {
            units.push_back(Json { { "uri", base::path_to_uri(module->path) }, { "role", "module-interface" } });
        }
        modules.push_back(Json { { "name", name }, { "external", module != nullptr && providers(name).empty() }, { "units", units } });
    }
    Json imports = Json::array();
    for (const auto& [key, entry] : files_) {
        for (const auto& import : entry.second.imports) {
            if (import.isHeaderUnit) continue;
            imports.push_back(Json { { "from", base::path_to_uri(entry.first) }, { "module", project::imported_name(entry.second, import) },
                                     { "range", to_json(import.nameRange) } });
        }
    }
    return Json { { "modules", modules }, { "imports", imports } };
}

Json ModuleIndex::module_info(std::string_view name) const {
    Json providersJson = Json::array();
    const auto units = providers(name);
    for (const auto& unit : units) {
        providersJson.push_back(Json { { "uri", base::path_to_uri(unit.path) }, { "role", std::string { spec::to_string(unit.role) } }, { "set", "" } });
    }
    Json info { { "name", std::string { name } }, { "providers", providersJson }, { "ambiguous", units.size() > 1 } };
    if (!units.empty()) {
        info["resolvedFrom"] = "set";
    } else if (const auto* module = external(name)) {
        info["providers"].push_back(Json { { "uri", base::path_to_uri(module->path) }, { "role", "module-interface" }, { "set", "" } });
        info["resolvedFrom"] = module->origin == "stdlib" ? "stdlib" : "module-metadata";
    }
    return info;
}

} // namespace mcppls::index
