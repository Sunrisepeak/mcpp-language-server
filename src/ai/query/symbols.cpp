module mcppls.ai.query.symbols;

import std;
import nlohmann.json;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.lsp.jsonrpc;
import mcppls.spec.query;
import mcppls.ai.query.view;
import mcppls.ai.query.files;
import mcppls.ai.query.modules;

namespace mcppls::ai::query {

namespace {

Json to_json(const SearchScope& scope) {
    return Json { { "searchedFiles", scope.searched }, { "complete", scope.complete() }, { "unsearched", scope.unsearched } };
}

// Where the identifiers a session produced were found, so an identifier alone finds its symbol
// again (S5 2.3). Keyed by root and USR; the queries of every kernel run on its loop's thread, and a
// process may hold kernels on several threads.
struct KnownSymbol {
    std::string path;
    int line { 0 };
    int column { 0 };
};

std::mutex knownMutex;
std::map<std::string, KnownSymbol, std::less<>>& known_symbols() {
    static std::map<std::string, KnownSymbol, std::less<>> known;
    return known;
}

void remember(const View& view, const Symbol& symbol) {
    const auto& at = symbol.declaration ? symbol.declaration : symbol.definition;
    if (symbol.id.empty() || !at) return;
    const std::lock_guard lock { knownMutex };
    auto& known = known_symbols();
    if (known.size() > 20000) known.clear();
    known[view.root() + "\n" + symbol.id] = KnownSymbol { view.path_of(at->file), at->line, at->column };
}

std::optional<KnownSymbol> recall(const View& view, std::string_view id) {
    const std::lock_guard lock { knownMutex };
    const auto& known = known_symbols();
    const auto found = known.find(view.root() + "\n" + std::string { id });
    if (found == known.end()) return std::nullopt;
    return found->second;
}

Failure timed_out(std::string_view what) { return Failure { "timeout", std::format("the engines did not answer {} in time", what), nullptr }; }

Failure no_core_engine() {
    return Failure { "unavailable", "this needs the core semantic engine, and the workspace runs without one (engine none)", nullptr };
}

std::string strip_scope_separator(std::string scope) {
    while (scope.ends_with("::")) scope.resize(scope.size() - 2);
    return scope;
}

std::pair<std::string, std::string> split_qualified(std::string_view name) {
    const std::size_t at { name.rfind("::") };
    if (at == std::string_view::npos) return { std::string {}, std::string { name } };
    return { strip_scope_separator(std::string { name.substr(0, at) }), std::string { name.substr(at + 2) } };
}

// The S5 kind for the kind a hover names (clang's index symbol kinds).
std::string kind_from_hover(std::string_view kind) {
    if (kind == "instance-method" || kind == "class-method" || kind == "static-method" || kind == "destructor") return "method";
    if (kind == "instance-property" || kind == "class-property" || kind == "static-property") return "property";
    if (kind == "enum-constant") return "enum-member";
    if (kind == "namespace-alias") return "namespace";
    if (kind == "union") return "struct";
    if (kind == "parameter") return "variable";
    if (kind == "conversion-function") return "function";
    return std::string { kind };
}

struct Candidate {
    std::string name;
    std::string container;
    int kind { 0 };
    std::string path;
    Json range;
};

std::string markdown_of(const Json& hover) {
    if (!hover.is_object()) return {};
    const Json contents = hover.value("contents", Json {});
    if (contents.is_string()) return contents.get<std::string>();
    if (contents.is_object()) return contents.value("value", std::string {});
    std::string text;
    if (contents.is_array()) {
        for (const auto& part : contents) text += part.is_string() ? part.get<std::string>() : part.value("value", std::string {});
    }
    return text;
}

Outcome<std::vector<Candidate>> candidates_by_name(View& view, std::string_view qualified, Clock::time_point deadline) {
    const auto [scope, base] = split_qualified(qualified);
    if (base.empty()) return std::unexpected { invalid_arguments("a symbol name is empty") };
    view.settle_index(deadline);
    auto result = view.request("workspace/symbol", Json { { "query", std::string { qualified } } }, deadline);
    if (!result) return std::unexpected { timed_out("workspace/symbol") };
    std::vector<Candidate> candidates;
    if (!result->is_array()) return candidates;
    for (const auto& entry : *result) {
        if (!entry.is_object()) continue;
        Candidate candidate;
        candidate.name = entry.value("name", std::string {});
        candidate.container = strip_scope_separator(entry.value("containerName", std::string {}));
        candidate.kind = entry.value("kind", 0);
        const Json location = entry.value("location", Json::object());
        const auto path = view.path_of_uri(location.value("uri", std::string {}));
        if (!path) continue;
        candidate.path = *path;
        candidate.range = location.value("range", Json::object());
        // A module is named whole ("hello.greet"); anything else by its own name within its scope.
        const bool matches { candidate.kind == 2 ? candidate.name == qualified
                                                 : candidate.name == base
                                                       && (scope.empty() || candidate.container == scope || candidate.container.ends_with("::" + scope)) };
        if (matches) candidates.push_back(std::move(candidate));
    }
    return candidates;
}

std::optional<Json> symbol_info(View& view, const std::string& path, const Json& position, Clock::time_point deadline) {
    view.open(path);
    auto result = view.request("textDocument/symbolInfo", Json { { "textDocument", view.text_document(path) }, { "position", position } }, deadline);
    if (!result || !result->is_array() || result->empty() || !(*result)[0].is_object()) return std::nullopt;
    return (*result)[0];
}

void apply_symbol_info(View& view, Symbol& symbol, const Json& info) {
    if (auto usr = lsp::string_at(info, "usr"); usr && !usr->empty()) symbol.id = *usr;
    if (auto name = lsp::string_at(info, "name"); name && !name->empty()) symbol.name = *name;
    std::string container { strip_scope_separator(info.value("containerName", std::string {})) };
    symbol.qualifiedName = container.empty() ? symbol.name : container + "::" + symbol.name;
    if (const Json* declaration = lsp::find(info, "declarationRange")) {
        if (auto location = view.location(*declaration)) symbol.declaration = *location;
    }
    if (const Json* definition = lsp::find(info, "definitionRange")) {
        if (auto location = view.location(*definition)) symbol.definition = *location;
    }
}

void describe(View& view, Symbol& symbol, Clock::time_point deadline) {
    // A declaration carries the documentation comment more often than a definition in another unit.
    const auto& at = symbol.declaration ? symbol.declaration : symbol.definition;
    if (!at) return;
    const std::string path { view.path_of(at->file) };
    view.open(path);
    auto hover = view.request("textDocument/hover", Json { { "textDocument", view.text_document(path) }, { "position", view.lsp_position(path, at->line, at->column) } },
                              deadline);
    if (!hover) return;
    const HoverParts parts { parse_hover(markdown_of(*hover)) };
    symbol.signature = parts.signature;
    symbol.type = parts.type;
    symbol.documentation = parts.documentation;
    if (symbol.kind.empty() || symbol.kind == "unknown") symbol.kind = kind_from_hover(parts.kind);
    if (symbol.name.empty()) symbol.name = parts.name;
    if (symbol.qualifiedName.empty()) symbol.qualifiedName = parts.scope.empty() ? symbol.name : parts.scope + "::" + symbol.name;
}

void finish(View& view, Symbol& symbol) {
    const auto& at = symbol.declaration ? symbol.declaration : symbol.definition;
    if (at && symbol.module.empty()) symbol.module = view.module_of(view.path_of(at->file));
    remember(view, symbol);
}

bool before(const Symbol& a, const Symbol& b) {
    const auto key = [](const Symbol& symbol) {
        const auto& at = symbol.declaration ? symbol.declaration : symbol.definition;
        return std::tuple { symbol.module, at ? at->file : std::string {}, at ? at->line : 0, at ? at->column : 0, symbol.qualifiedName };
    };
    return key(a) < key(b);
}

// Opens the units that can use a symbol declared in `path` and waits until the core engine built
// them, so what they reference is in its index of open files.
SearchScope open_search_scope(View& view, const std::string& path, Clock::time_point deadline) {
    SearchScope scope;
    std::vector<std::string> files;
    const std::string declaredIn { view.module_of(path) };
    if (declaredIn.empty()) {
        files.push_back(path);
    } else {
        auto neighbourhood = module_neighbourhood(view, declaredIn, SEARCH_BUDGET);
        files = std::move(neighbourhood.files);
        for (const auto& left : neighbourhood.left) scope.unsearched.push_back(view.display(left));
        if (std::ranges::none_of(files, [&](const std::string& file) { return base::path_key(file) == base::path_key(path); })) files.push_back(path);
    }
    for (const auto& file : files) view.open(file);
    const auto remaining = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()), std::chrono::milliseconds { 0 });
    (void)view.kernel().wait_until([&] { return std::ranges::all_of(files, [&](const std::string& file) { return diagnostics_fresh(view, file).first; }); },
                                   remaining);
    scope.searched = files.size();
    return scope;
}

// A definition in another unit of the module (an implementation unit) is known to the engine only
// once that unit is built: the module's own units are opened, and the declaration asked for it.
void find_definition(View& view, Symbol& symbol, Clock::time_point deadline) {
    if (symbol.definition || !symbol.declaration) return;
    const std::string declared { view.path_of(symbol.declaration->file) };
    const std::string declaredIn { view.module_of(declared) };
    if (declaredIn.empty()) return;
    const std::string primary { declaredIn.substr(0, declaredIn.find(':')) };
    const auto& index = view.kernel().workspace().module_index();
    std::vector<std::string> units;
    for (const auto& file : index.files()) {
        const auto* scan = index.scan_of(file);
        if (scan != nullptr && scan->declaration && scan->declaration->module == primary && units.size() < SEARCH_BUDGET) units.push_back(file);
    }
    for (const auto& unit : units) view.open(unit);
    const auto remaining = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()), std::chrono::milliseconds { 0 });
    (void)view.kernel().wait_until([&] { return std::ranges::all_of(units, [&](const std::string& unit) { return diagnostics_fresh(view, unit).first; }); },
                                   remaining);
    auto result = view.request("textDocument/definition",
                               Json { { "textDocument", view.text_document(declared) },
                                      { "position", view.lsp_position(declared, symbol.declaration->line, symbol.declaration->column) } },
                               deadline);
    if (!result) return;
    const Json first = result->is_array() ? (result->empty() ? Json {} : (*result)[0]) : *result;
    auto location = view.location(first);
    if (location && !(location->file == symbol.declaration->file && location->line == symbol.declaration->line)) symbol.definition = std::move(*location);
}

Outcome<Symbol> symbol_at(View& view, const std::string& path, int line, int column, Clock::time_point deadline) {
    const Json position = view.lsp_position(path, line, column);
    const auto info = symbol_info(view, path, position, deadline);
    if (!info) return std::unexpected { not_found(std::format("no symbol at {}:{}:{}", view.display(path), line, column)) };
    Symbol symbol;
    apply_symbol_info(view, symbol, *info);
    find_definition(view, symbol, deadline);
    if (!symbol.declaration && !symbol.definition) symbol.declaration = spec::location_in(view.text_of(path), view.display(path), spec::lsp_position(view.text_of(path), line, column));
    describe(view, symbol, deadline);
    finish(view, symbol);
    return symbol;
}

} // namespace

HoverParts parse_hover(std::string_view markdown) {
    HoverParts parts;
    const auto lines = base::split_lines(markdown);
    std::size_t i { 0 };
    // "### function `greet`"
    for (; i < lines.size(); ++i) {
        const std::string_view line { base::trim(lines[i]) };
        if (line.empty()) continue;
        if (line.starts_with("###")) {
            std::string_view heading { base::trim(line.substr(3)) };
            const std::size_t tick { heading.find('`') };
            parts.kind = std::string { base::trim(heading.substr(0, tick)) };
            if (tick != std::string_view::npos) {
                const std::size_t close { heading.find('`', tick + 1) };
                parts.name = std::string { heading.substr(tick + 1, close == std::string_view::npos ? std::string_view::npos : close - tick - 1) };
            }
            ++i;
        }
        break;
    }
    std::string documentation;
    bool inCode { false };
    std::string code;
    for (; i < lines.size(); ++i) {
        const std::string_view raw { lines[i] };
        const std::string_view line { base::trim(raw) };
        if (inCode) {
            if (line.starts_with("```")) {
                inCode = false;
                continue;
            }
            code += std::string { raw } + "\n";
            continue;
        }
        if (line.starts_with("```")) {
            inCode = true;
            code.clear();
            continue;
        }
        if (line == "---" || line.empty()) {
            if (!documentation.empty() && !documentation.ends_with("\n\n")) documentation += "\n";
            continue;
        }
        if (line.starts_with("→ ")) {
            parts.type = std::string { base::trim(base::replace_all(line.substr(std::string_view { "→ " }.size()), "`", "")) };
            continue;
        }
        if (line.starts_with("Type:")) {
            parts.type = std::string { base::trim(base::replace_all(line.substr(5), "`", "")) };
            continue;
        }
        if (line.starts_with("Parameters:") || line.starts_with("- `") || line.starts_with("Value =") || line.starts_with("Offset:")
            || line.starts_with("Size:") || line.starts_with("Passed ") || line.starts_with("provided by")) {
            continue;
        }
        documentation += std::string { line } + "\n";
    }
    parts.documentation = std::string { base::trim(documentation) };
    // "// In namespace hello" or "// In Greeter" before the declaration.
    std::string signature;
    for (auto line : base::split_lines(code)) {
        const std::string_view trimmed { base::trim(line) };
        if (trimmed.starts_with("// In ")) {
            std::string scope { trimmed.substr(6) };
            if (scope.starts_with("namespace ")) scope = scope.substr(10);
            parts.scope = std::move(scope);
            continue;
        }
        if (!signature.empty()) signature += "\n";
        signature += std::string { line };
    }
    parts.signature = std::string { base::trim(signature) };
    return parts;
}

std::string name_from_usr(std::string_view usr) {
    std::string_view prefix { usr.substr(0, usr.find('#')) };
    std::string last;
    std::size_t i { 0 };
    while (i < prefix.size()) {
        if (!base::is_identifier_start(prefix[i])) {
            ++i;
            continue;
        }
        std::size_t end { i };
        while (end < prefix.size() && base::is_identifier_char(prefix[end])) ++end;
        const std::string_view word { prefix.substr(i, end - i) };
        static constexpr std::array<std::string_view, 16> MARKERS { "c", "N", "F", "FT", "S", "ST", "C", "CT", "U", "E", "EC", "FI", "T", "TT", "V", "a" };
        // A marker is always between '@'s; a name of one of those spellings would be too.
        const bool marker { std::ranges::find(MARKERS, word) != MARKERS.end() && (i == 0 || prefix[i - 1] == '@' || prefix[i - 1] == ':')
                            && (end == prefix.size() || prefix[end] == '@' || prefix[end] == ':' || prefix[end] == '>') };
        if (!marker) last = std::string { word };
        i = end;
    }
    return last;
}

Outcome<Symbols> find_symbols(View& view, const SymbolTarget& target, Limit limit, bool doDescribe, Clock::time_point deadline) {
    view.refresh();
    (void)view.settle(deadline);
    Symbols found;
    // A position names one symbol.
    if (!target.file.empty() && target.line > 0) {
        if (!view.has_core_engine()) return std::unexpected { no_core_engine() };
        const std::string path { view.path_of(target.file) };
        if (view.text_of(path).empty()) return std::unexpected { invalid_arguments(std::format("no such file: {}", target.file)) };
        auto symbol = symbol_at(view, path, target.line, std::max(target.column, 1), deadline);
        if (!symbol) return std::unexpected { symbol.error() };
        found.symbols.push_back(std::move(*symbol));
        found.total = 1;
        found.snapshot = view.snapshot();
        return found;
    }
    // An identifier this session produced is found where it was; otherwise by the name it ends with.
    if (!target.id.empty()) {
        if (!view.has_core_engine()) return std::unexpected { no_core_engine() };
        if (auto known = recall(view, target.id)) {
            auto symbol = symbol_at(view, known->path, known->line, known->column, deadline);
            if (symbol && symbol->id == target.id) {
                found.symbols.push_back(std::move(*symbol));
                found.total = 1;
                found.snapshot = view.snapshot();
                return found;
            }
        }
        const std::string name { target.name.empty() ? name_from_usr(target.id) : target.name };
        if (name.empty()) return std::unexpected { not_found(std::format("no symbol with identifier {}", target.id)) };
        auto candidates = candidates_by_name(view, name, deadline);
        if (!candidates) return std::unexpected { candidates.error() };
        for (const auto& candidate : *candidates) {
            const auto info = symbol_info(view, candidate.path, candidate.range.value("start", Json::object()), deadline);
            if (!info || info->value("usr", std::string {}) != target.id) continue;
            Symbol symbol;
            symbol.kind = std::string { symbol_kind_name(candidate.kind) };
            apply_symbol_info(view, symbol, *info);
            if (doDescribe) describe(view, symbol, deadline);
            finish(view, symbol);
            found.symbols.push_back(std::move(symbol));
            found.total = 1;
            break;
        }
        if (found.symbols.empty()) return std::unexpected { not_found(std::format("no symbol with identifier {}", target.id)) };
        found.snapshot = view.snapshot();
        return found;
    }
    if (target.name.empty()) return std::unexpected { invalid_arguments("name a symbol by id, by name, or by file, line and column") };
    auto candidates = candidates_by_name(view, target.name, deadline);
    if (!candidates) return std::unexpected { candidates.error() };
    std::vector<Candidate> matching;
    for (auto& candidate : *candidates) {
        if (!target.kind.empty() && symbol_kind_name(candidate.kind) != target.kind) continue;
        if (!target.module.empty()) {
            const std::string module { candidate.kind == 2 ? candidate.name : view.module_of(candidate.path) };
            if (module != target.module && !module.starts_with(target.module + ":")) continue;
        }
        matching.push_back(std::move(candidate));
    }
    found.total = matching.size();
    found.truncated = matching.size() > limit.maxResults;
    for (const auto& candidate : matching) {
        if (found.symbols.size() >= limit.maxResults) break;
        Symbol symbol;
        symbol.name = candidate.name;
        symbol.kind = std::string { symbol_kind_name(candidate.kind) };
        symbol.qualifiedName = candidate.container.empty() ? candidate.name : candidate.container + "::" + candidate.name;
        const spec::Location at { view.location(candidate.path, candidate.range) };
        if (candidate.kind == 2) {
            // A module: mcppls's own engine names its primary interface; there is no USR.
            symbol.module = candidate.name;
            symbol.declaration = at;
        } else if (view.has_core_engine()) {
            if (const auto info = symbol_info(view, candidate.path, candidate.range.value("start", Json::object()), deadline)) apply_symbol_info(view, symbol, *info);
            if (!symbol.declaration && !symbol.definition) symbol.declaration = at;
            find_definition(view, symbol, deadline);
            if (doDescribe) describe(view, symbol, deadline);
        } else {
            symbol.declaration = at;
        }
        finish(view, symbol);
        // The engine's index can name one symbol twice, by its declaration and by its definition.
        const auto same = std::ranges::find_if(found.symbols, [&](const Symbol& other) { return !symbol.id.empty() && other.id == symbol.id; });
        if (same != found.symbols.end()) {
            if (!same->declaration) same->declaration = symbol.declaration;
            if (!same->definition) same->definition = symbol.definition;
            if (same->signature.empty()) same->signature = symbol.signature;
            if (same->documentation.empty()) same->documentation = symbol.documentation;
            --found.total;
            continue;
        }
        found.symbols.push_back(std::move(symbol));
    }
    found.truncated = found.total > found.symbols.size();
    std::ranges::sort(found.symbols, before);
    found.snapshot = view.snapshot();
    return found;
}

Outcome<Symbol> resolve_symbol(View& view, const SymbolTarget& target, Clock::time_point deadline) {
    auto found = find_symbols(view, target, Limit { 20 }, false, deadline);
    if (!found) return std::unexpected { found.error() };
    // Modules are not what references and calls are about.
    std::erase_if(found->symbols, [&](const Symbol& symbol) { return symbol.kind == "module" && target.kind != "module"; });
    if (found->symbols.empty()) {
        return std::unexpected { not_found(std::format("no symbol named {}", target.name.empty() ? target.id : target.name)) };
    }
    if (found->symbols.size() > 1) {
        Json candidates = Json::array();
        for (const auto& symbol : found->symbols) candidates.push_back(to_json(symbol));
        return std::unexpected { Failure { "ambiguous", std::format("{} symbols are named {}; name one by its id", found->symbols.size(), target.name), std::move(candidates) } };
    }
    return std::move(found->symbols.front());
}

namespace {

struct Reference {
    std::string path;
    Json range;              // LSP
    spec::Location location;
    std::string module;
};

// The references to a symbol in its search scope, in S5 order.
Outcome<std::pair<std::vector<Reference>, SearchScope>> references_of(View& view, const Symbol& symbol, bool includeDeclaration, Clock::time_point deadline) {
    const auto& at = symbol.declaration ? symbol.declaration : symbol.definition;
    if (!at) return std::unexpected { not_found(std::format("{} has no location to look from", symbol.qualifiedName)) };
    view.settle_index(deadline);
    const std::string path { view.path_of(at->file) };
    SearchScope scope { open_search_scope(view, path, deadline) };
    auto result = view.request("textDocument/references",
                               Json { { "textDocument", view.text_document(path) }, { "position", view.lsp_position(path, at->line, at->column) },
                                      { "context", Json { { "includeDeclaration", includeDeclaration } } } },
                               deadline);
    if (!result) return std::unexpected { timed_out("textDocument/references") };
    std::vector<Reference> references;
    if (result->is_array()) {
        for (const auto& entry : *result) {
            const auto referencePath = view.path_of_uri(entry.value("uri", std::string {}));
            if (!referencePath) continue;
            const Json range = entry.value("range", Json::object());
            Reference reference { *referencePath, range, view.location(*referencePath, range), view.module_of(*referencePath) };
            references.push_back(std::move(reference));
        }
    }
    std::ranges::sort(references, {}, [](const Reference& r) { return std::tuple { r.module, r.location.file, r.location.line, r.location.column }; });
    references.erase(std::unique(references.begin(), references.end(), [](const Reference& a, const Reference& b) { return a.location == b.location; }),
                     references.end());
    return std::pair { std::move(references), std::move(scope) };
}

bool contains(const Json& range, const Json& position) {
    const auto key = [](const Json& value) { return std::pair { value.value("line", 0), value.value("character", 0) }; };
    const Json start = range.value("start", Json::object());
    const Json end = range.value("end", Json::object());
    return key(start) <= key(position) && key(position) <= key(end);
}

struct Enclosing {
    std::string name;
    std::string qualifiedName;
    int kind { 0 };
    Json selectionRange;
};

// The innermost function, method or constructor of an outline whose range contains `position`.
void find_enclosing(const Json& symbols, const Json& position, const std::string& scope, std::optional<Enclosing>& found) {
    if (!symbols.is_array()) return;
    for (const auto& symbol : symbols) {
        if (!symbol.is_object() || !contains(symbol.value("range", Json::object()), position)) continue;
        const int kind { symbol.value("kind", 0) };
        const std::string name { symbol.value("name", std::string {}) };
        const bool callable { kind == 6 || kind == 9 || kind == 12 || kind == 25 };
        const bool scoping { kind == 3 || kind == 5 || kind == 23 || kind == 10 };
        const std::string qualified { scope.empty() ? name : scope + "::" + name };
        if (callable) found = Enclosing { name, qualified, kind, symbol.value("selectionRange", Json::object()) };
        find_enclosing(symbol.value("children", Json::array()), position, scoping ? qualified : scope, found);
    }
}

} // namespace

Outcome<References> find_references(View& view, const SymbolTarget& target, bool includeDeclaration, Limit limit, Clock::time_point deadline) {
    if (!view.has_core_engine()) {
        view.refresh();
        return std::unexpected { no_core_engine() };
    }
    auto symbol = resolve_symbol(view, target, deadline);
    if (!symbol) return std::unexpected { symbol.error() };
    auto found = references_of(view, *symbol, includeDeclaration, deadline);
    if (!found) return std::unexpected { found.error() };
    auto& [all, scope] = *found;
    References references;
    references.scope = std::move(scope);
    references.symbol = std::move(*symbol);
    references.total = all.size();
    references.truncated = all.size() > limit.maxResults;
    if (all.size() > limit.maxResults) all.resize(limit.maxResults);
    for (auto& reference : all) {
        if (references.groups.empty() || references.groups.back().file != reference.location.file) {
            references.groups.push_back(ReferenceGroup { reference.module, reference.location.file, {} });
        }
        references.groups.back().references.push_back(std::move(reference.location));
    }
    references.snapshot = view.snapshot();
    return references;
}

Outcome<Calls> find_calls(View& view, const SymbolTarget& target, CallDirection direction, Limit limit, Clock::time_point deadline) {
    if (!view.has_core_engine()) {
        view.refresh();
        return std::unexpected { no_core_engine() };
    }
    auto symbol = resolve_symbol(view, target, deadline);
    if (!symbol) return std::unexpected { symbol.error() };
    Calls calls;
    calls.direction = direction;
    if (direction == CallDirection::incoming) {
        // Callers are the functions enclosing the references. The engine's own call hierarchy groups
        // callers by identifier, and two executables' `main` share one: a test's call went missing.
        auto found = references_of(view, *symbol, false, deadline);
        if (!found) return std::unexpected { found.error() };
        auto& [references, scope] = *found;
        calls.scope = std::move(scope);
        std::map<std::string, Json> outlines;
        std::map<std::string, std::size_t> byCaller;   // path and the caller's position -> index in calls.calls
        for (const auto& reference : references) {
            auto outline = outlines.find(reference.path);
            if (outline == outlines.end()) {
                auto symbols = view.request("textDocument/documentSymbol", Json { { "textDocument", view.text_document(reference.path) } }, deadline);
                outline = outlines.emplace(reference.path, symbols ? *symbols : Json::array()).first;
            }
            std::optional<Enclosing> enclosing;
            find_enclosing(outline->second, reference.range.value("start", Json::object()), {}, enclosing);
            if (!enclosing) continue;   // a reference outside any function: an initializer, a using-declaration
            const std::string key { reference.path + "\n" + lsp::dump(enclosing->selectionRange) };
            auto entry = byCaller.find(key);
            if (entry == byCaller.end()) {
                Call call;
                call.symbol.name = enclosing->name;
                call.symbol.qualifiedName = enclosing->qualifiedName;
                call.symbol.kind = std::string { symbol_kind_name(enclosing->kind) };
                call.symbol.definition = view.location(reference.path, enclosing->selectionRange);
                call.symbol.module = reference.module;
                entry = byCaller.emplace(key, calls.calls.size()).first;
                calls.calls.push_back(std::move(call));
            }
            calls.calls[entry->second].sites.push_back(reference.location);
        }
    } else {
        // Callees are in the function's own body.
        const auto& at = symbol->definition ? symbol->definition : symbol->declaration;
        if (!at) return std::unexpected { not_found(std::format("{} has no location to look from", symbol->qualifiedName)) };
        const std::string path { view.path_of(at->file) };
        view.open(path);
        auto items = view.request("textDocument/prepareCallHierarchy",
                                  Json { { "textDocument", view.text_document(path) }, { "position", view.lsp_position(path, at->line, at->column) } }, deadline);
        if (!items) return std::unexpected { timed_out("textDocument/prepareCallHierarchy") };
        if (!items->is_array() || items->empty()) return std::unexpected { not_found(std::format("{} is not something that calls", symbol->qualifiedName)) };
        const Json item = (*items)[0];
        auto result = view.request("callHierarchy/outgoingCalls", Json { { "item", item } }, deadline);
        if (!result) return std::unexpected { timed_out("the call hierarchy") };
        const std::string itemPath { view.path_of_uri(item.value("uri", std::string {})).value_or(path) };
        calls.scope.searched = 1;
        if (result->is_array()) {
            for (const auto& entry : *result) {
                const Json other = entry.value("to", Json::object());
                const auto otherPath = view.path_of_uri(other.value("uri", std::string {}));
                if (!otherPath) continue;
                Call call;
                call.symbol.name = other.value("name", std::string {});
                call.symbol.kind = std::string { symbol_kind_name(other.value("kind", 0)) };
                // clangd's detail is the scope the function is in.
                const std::string scope { strip_scope_separator(other.value("detail", std::string {})) };
                call.symbol.qualifiedName = scope.empty() ? call.symbol.name : scope + "::" + call.symbol.name;
                call.symbol.definition = view.location(*otherPath, other.value("selectionRange", Json::object()));
                call.symbol.module = view.module_of(*otherPath);
                // The call sites are in the function itself.
                for (const auto& range : entry.value("fromRanges", Json::array())) call.sites.push_back(view.location(itemPath, range));
                calls.calls.push_back(std::move(call));
            }
        }
    }
    calls.symbol = std::move(*symbol);
    std::ranges::sort(calls.calls, [](const Call& a, const Call& b) { return before(a.symbol, b.symbol); });
    calls.total = calls.calls.size();
    calls.truncated = calls.calls.size() > limit.maxResults;
    if (calls.truncated) calls.calls.resize(limit.maxResults);
    calls.snapshot = view.snapshot();
    return calls;
}

Json to_json(const Symbol& symbol) {
    Json value { { "name", symbol.name }, { "qualifiedName", symbol.qualifiedName }, { "kind", symbol.kind } };
    if (!symbol.id.empty()) value["id"] = symbol.id;
    if (!symbol.module.empty()) value["module"] = symbol.module;
    if (symbol.declaration) value["declaration"] = spec::to_json(*symbol.declaration);
    if (symbol.definition) value["definition"] = spec::to_json(*symbol.definition);
    if (!symbol.signature.empty()) value["signature"] = symbol.signature;
    if (!symbol.type.empty()) value["type"] = symbol.type;
    if (!symbol.documentation.empty()) value["documentation"] = symbol.documentation;
    return value;
}

Json to_json(const Symbols& symbols) {
    Json list = Json::array();
    for (const auto& symbol : symbols.symbols) list.push_back(to_json(symbol));
    return Json { { "snapshot", spec::to_json(symbols.snapshot) }, { "symbols", std::move(list) }, { "total", symbols.total }, { "truncated", symbols.truncated } };
}

Json to_json(const References& references) {
    Json groups = Json::array();
    for (const auto& group : references.groups) {
        Json items = Json::array();
        for (const auto& location : group.references) items.push_back(Json { { "line", location.line }, { "column", location.column }, { "text", location.text } });
        Json value { { "file", group.file }, { "references", std::move(items) } };
        if (!group.module.empty()) value["module"] = group.module;
        groups.push_back(std::move(value));
    }
    return Json { { "snapshot", spec::to_json(references.snapshot) }, { "symbol", to_json(references.symbol) }, { "groups", std::move(groups) },
                  { "total", references.total }, { "truncated", references.truncated }, { "scope", to_json(references.scope) } };
}

Json to_json(const Calls& calls) {
    Json list = Json::array();
    for (const auto& call : calls.calls) {
        Json sites = Json::array();
        for (const auto& site : call.sites) sites.push_back(spec::to_json(site));
        list.push_back(Json { { "symbol", to_json(call.symbol) }, { "sites", std::move(sites) } });
    }
    return Json { { "snapshot", spec::to_json(calls.snapshot) }, { "symbol", to_json(calls.symbol) },
                  { "direction", calls.direction == CallDirection::incoming ? "callers" : "callees" }, { "calls", std::move(list) },
                  { "total", calls.total }, { "truncated", calls.truncated }, { "scope", to_json(calls.scope) } };
}

} // namespace mcppls::ai::query
