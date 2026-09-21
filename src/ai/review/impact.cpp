module mcppls.ai.review.impact;

import std;
import nlohmann.json;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.fs;
import mcppls.spec.database;
import mcppls.spec.query;
import mcppls.project.model;
import mcppls.project.scan;
import mcppls.engine.native.index;
import mcppls.orchestrator.kernel;
import mcppls.orchestrator.workspace;
import mcppls.ai.query.view;
import mcppls.ai.query.modules;
import mcppls.ai.query.symbols;
import mcppls.ai.review.changes;
import mcppls.ai.review.semantic;

namespace mcppls::ai::review {

namespace {

using Json = nlohmann::json;

struct Token {
    std::string_view text;
    std::size_t offset { 0 };
    bool identifier { false };
};

// Identifiers and punctuation of C++ source, comments and literals left out.
std::vector<Token> tokenize(std::string_view text) {
    std::vector<Token> tokens;
    std::size_t i { 0 };
    const std::size_t n { text.size() };
    while (i < n) {
        const char c { text[i] };
        if (c == '/' && i + 1 < n && text[i + 1] == '/') {
            while (i < n && text[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && text[i + 1] == '*') {
            const std::size_t end { text.find("*/", i + 2) };
            i = end == std::string_view::npos ? n : end + 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            ++i;
            while (i < n && text[i] != c && text[i] != '\n') {
                if (text[i] == '\\') ++i;
                ++i;
            }
            ++i;
            continue;
        }
        if (base::is_identifier_start(c)) {
            const std::size_t start { i };
            while (i < n && base::is_identifier_char(text[i])) ++i;
            const std::string_view word { text.substr(start, i - start) };
            // A raw string literal, with or without an encoding prefix (R, LR, uR, UR, u8R), is not code.
            const bool rawPrefix { word == "R" || word == "LR" || word == "uR" || word == "UR" || word == "u8R" };
            if (rawPrefix && i < n && text[i] == '"') {
                const std::size_t open { text.find('(', i + 1) };
                if (open != std::string_view::npos) {
                    const std::string close { ")" + std::string { text.substr(i + 1, open - i - 1) } + "\"" };
                    const std::size_t end { text.find(close, open + 1) };
                    i = end == std::string_view::npos ? n : end + close.size();
                    continue;
                }
            }
            tokens.push_back(Token { word, start, true });
            continue;
        }
        if (c >= '0' && c <= '9') {
            while (i < n && (base::is_identifier_char(text[i]) || text[i] == '.' || text[i] == '\'')) ++i;
            continue;
        }
        if (c == ':' && i + 1 < n && text[i + 1] == ':') {
            tokens.push_back(Token { text.substr(i, 2), i, false });
            i += 2;
            continue;
        }
        if (c == '-' && i + 1 < n && text[i + 1] == '>') {
            tokens.push_back(Token { text.substr(i, 2), i, false });
            i += 2;
            continue;
        }
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') tokens.push_back(Token { text.substr(i, 1), i, false });
        ++i;
    }
    return tokens;
}

std::vector<std::string> split_scope(std::string_view scope) {
    std::vector<std::string> parts;
    std::size_t start { 0 };
    while (start <= scope.size()) {
        const std::size_t at { scope.find("::", start) };
        const std::string_view part { scope.substr(start, at == std::string_view::npos ? std::string_view::npos : at - start) };
        if (!part.empty()) parts.emplace_back(part);
        if (at == std::string_view::npos) break;
        start = at + 2;
    }
    return parts;
}

// Whether the text opens `scope` itself, by a namespace definition or a using-directive.
bool opens_scope(std::span<const Token> tokens, std::string_view scope) {
    for (std::size_t i { 0 }; i + 1 < tokens.size(); ++i) {
        if (tokens[i].text != "namespace") continue;
        std::string name;
        std::size_t j { i + 1 };
        while (j < tokens.size() && tokens[j].identifier) {
            name += tokens[j].text;
            if (j + 1 < tokens.size() && tokens[j + 1].text == "::") {
                name += "::";
                j += 2;
            } else {
                ++j;
            }
        }
        if (name == scope || name.ends_with("::" + std::string { scope }) || std::string { scope }.starts_with(name + "::")) return true;
    }
    return false;
}

// Whether the name at `i` is being declared, not used: a type before it, a parameter list after it
// (`std::string format_name(std::string_view)`). A keyword before it makes it an expression again.
bool declares(std::span<const Token> tokens, std::size_t i) {
    if (i == 0 || i + 1 >= tokens.size() || tokens[i + 1].text != "(") return false;
    const Token& previous { tokens[i - 1] };
    static constexpr std::array<std::string_view, 16> EXPRESSION_KEYWORDS { "return", "co_return", "co_yield", "co_await", "throw", "case", "else",
                                                                            "do", "new", "delete", "sizeof", "typeid", "not", "and", "or", "decltype" };
    if (previous.identifier) return std::ranges::find(EXPRESSION_KEYWORDS, previous.text) == EXPRESSION_KEYWORDS.end();
    return previous.text == "*" || previous.text == "&";
}

std::string scope_of(std::string_view qualifiedName) {
    const std::size_t at { qualifiedName.rfind("::") };
    return at == std::string_view::npos ? std::string {} : std::string { qualifiedName.substr(0, at) };
}

} // namespace

std::vector<spec::Location> identifier_uses(std::string_view text, std::string display, std::string_view name, std::string_view scope) {
    std::vector<spec::Location> uses;
    const auto tokens = tokenize(text);
    const auto parts = split_scope(scope);
    const bool open { parts.empty() || opens_scope(tokens, scope) };
    for (std::size_t i { 0 }; i < tokens.size(); ++i) {
        const Token& token { tokens[i] };
        if (!token.identifier || token.text != name) continue;
        if (i > 0 && (tokens[i - 1].text == "." || tokens[i - 1].text == "->")) continue;
        if (declares(tokens, i)) continue;
        const bool qualified { i >= 2 && tokens[i - 1].text == "::" && tokens[i - 2].identifier && !parts.empty() && tokens[i - 2].text == parts.back() };
        if (!qualified && !open) continue;
        const auto start = base::position_at(text, token.offset);
        const auto end = base::position_at(text, token.offset + token.text.size());
        uses.push_back(spec::location_in(text, display, start, end));
    }
    return uses;
}

Impact analyze_impact(query::View& view, const ChangeSet& changes, std::span<const UnitDiff> diffs, std::size_t budget, query::Clock::time_point deadline) {
    Impact impact;
    auto& workspace = view.kernel().workspace();
    std::set<std::string> modules;
    std::set<std::string> changedPaths;
    for (const auto& file : changes.files) changedPaths.insert(base::path_key(file.path));
    for (const auto& diff : diffs) {
        if (!diff.interface_changed()) continue;
        for (const auto& name : { diff.baseModule, diff.headModule }) {
            if (!name.empty()) modules.insert(name);
        }
    }
    impact.modules.assign(modules.begin(), modules.end());

    // The units that can use the changed interfaces, nearest first.
    std::vector<std::string> files;
    std::set<std::string> seen;
    for (const auto& name : impact.modules) {
        auto neighbourhood = query::module_neighbourhood(view, name, budget);
        for (auto& path : neighbourhood.files) {
            if (seen.insert(base::path_key(path)).second) files.push_back(std::move(path));
        }
        for (auto& path : neighbourhood.left) {
            if (seen.insert(base::path_key(path)).second) impact.unsearched.push_back(view.display(path));
        }
    }
    for (const auto& path : files) impact.files.push_back(view.display(path));

    // Uses of every export that went away or changed, in the units that can use it. A changed
    // declaration still exists, so the core engine finds its uses; a removed one only the text can.
    for (const auto& diff : diffs) {
        for (const auto& change : diff.exports) {
            if (change.kind == ExportChangeKind::added) continue;
            NameUses uses;
            uses.qualifiedName = change.qualifiedName;
            uses.module = diff.baseModule.empty() ? diff.headModule : diff.baseModule;
            if (change.kind == ExportChangeKind::changed && change.headLocation && view.has_core_engine()) {
                query::SymbolTarget target;
                target.file = view.path_of(change.headLocation->file);
                target.line = change.headLocation->line;
                target.column = change.headLocation->column;
                if (auto found = query::find_references(view, target, false, query::Limit { 500 }, deadline)) {
                    uses.semantic = true;
                    for (const auto& group : found->groups) {
                        if (group.file == diff.file) continue;
                        for (const auto& location : group.references) uses.uses.push_back(location);
                    }
                }
            }
            // The text as well: the engine does not see units it left out of its database (an import
            // that does not resolve), and a use there is still a use.
            const std::string scope { scope_of(change.qualifiedName) };
            for (const auto& path : files) {
                const std::string display { view.display(path) };
                if (display == diff.file) continue;
                const std::string text { view.text_of(path) };
                for (auto& location : identifier_uses(text, display, change.name, scope)) {
                    const bool known { std::ranges::any_of(uses.uses, [&](const spec::Location& other) {
                        return other.file == location.file && other.line == location.line && other.column == location.column;
                    }) };
                    if (!known) uses.uses.push_back(std::move(location));
                }
            }
            std::ranges::sort(uses.uses, {}, [](const spec::Location& location) { return std::tuple { location.file, location.line, location.column }; });
            impact.names.push_back(std::move(uses));
        }
    }

    // Sets and tests.
    if (const auto model = workspace.project_model()) {
        std::set<std::string> involved { changedPaths };
        for (const auto& path : files) involved.insert(base::path_key(path));
        for (const auto& set : model->database.sets) {
            TestCoverage coverage;
            coverage.set = set.name;
            bool builds { false };
            for (const auto& unit : set.units) {
                const std::string path { spec::absolute_source(unit) };
                const std::string key { base::path_key(path) };
                if (!involved.contains(key)) continue;
                builds = true;
                if (changedPaths.contains(key)) coverage.changed = true;
                if (seen.contains(key)) coverage.files.push_back(view.display(path));
            }
            if (!builds) continue;
            impact.sets.push_back(set.name);
            if (set.kind == "test") impact.tests.push_back(std::move(coverage));
        }
    }
    return impact;
}

Json to_json(const Impact& impact) {
    Json names = Json::array();
    for (const auto& name : impact.names) {
        Json uses = Json::array();
        for (const auto& use : name.uses) uses.push_back(spec::to_json(use));
        names.push_back(Json { { "qualifiedName", name.qualifiedName }, { "module", name.module }, { "semantic", name.semantic }, { "uses", std::move(uses) } });
    }
    Json tests = Json::array();
    for (const auto& test : impact.tests) tests.push_back(Json { { "set", test.set }, { "files", test.files }, { "changed", test.changed } });
    return Json { { "modules", impact.modules }, { "files", impact.files }, { "unsearched", impact.unsearched }, { "sets", impact.sets },
                  { "tests", std::move(tests) }, { "names", std::move(names) } };
}

} // namespace mcppls::ai::review
