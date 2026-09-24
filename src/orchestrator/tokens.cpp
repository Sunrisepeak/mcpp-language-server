module mcppls.orchestrator.tokens;

import std;
import nlohmann.json;

namespace mcppls::orchestrator::tokens {

namespace {

// The LSP standard token types (metaModel 3.17's SemanticTokenTypes), clangd 23.1's own extras
// (measured 2026-09-25: `unknown`, `concept`, `bracket`, `label` -- everything else it declares,
// including its duplicates, is already an LSP standard name), and `module`, mcppls's own.
constexpr std::array<std::string_view, 28> BASE_TYPES {
    "namespace", "type", "class", "enum", "interface", "struct", "typeParameter", "parameter", "variable", "property",
    "enumMember", "event", "function", "method", "macro", "keyword", "modifier", "comment", "string", "number",
    "regexp", "operator", "decorator",
    "unknown", "concept", "bracket", "label",
    "module",
};

// The LSP standard modifiers, clangd's own extras, and `partition`, mcppls's own.
constexpr std::array<std::string_view, 22> BASE_MODIFIERS {
    "declaration", "definition", "readonly", "static", "deprecated", "abstract", "async", "modification", "documentation", "defaultLibrary",
    "deduced", "virtual", "dependentName", "usedAsMutableReference", "usedAsMutablePointer", "constructorOrDestructor", "userDefined",
    "functionScope", "classScope", "fileScope", "globalScope",
    "partition",
};

std::size_t index_of_or_append(std::vector<std::string>& list, const std::string& name) {
    if (const auto found = std::ranges::find(list, name); found != list.end()) return static_cast<std::size_t>(std::distance(list.begin(), found));
    list.push_back(name);
    return list.size() - 1;
}

} // namespace

std::span<const std::string_view> base_types() { return BASE_TYPES; }
std::span<const std::string_view> base_modifiers() { return BASE_MODIFIERS; }

std::size_t type_index(std::string_view name) {
    for (std::size_t i = 0; i < BASE_TYPES.size(); ++i) {
        if (BASE_TYPES[i] == name) return i;
    }
    return 0;
}

std::uint32_t modifier_bit(std::string_view name) {
    for (std::size_t i = 0; i < BASE_MODIFIERS.size(); ++i) {
        if (BASE_MODIFIERS[i] == name) return 1u << i;
    }
    return 0;
}

Legend build_legend(const Json& coreCapabilities) {
    Legend legend;
    legend.types.assign(BASE_TYPES.begin(), BASE_TYPES.end());
    legend.modifiers.assign(BASE_MODIFIERS.begin(), BASE_MODIFIERS.end());

    Json coreTypes = Json::array();
    Json coreModifiers = Json::array();
    if (coreCapabilities.is_object()) {
        if (const auto provider = coreCapabilities.find("semanticTokensProvider"); provider != coreCapabilities.end() && provider->is_object()) {
            if (const auto declared = provider->find("legend"); declared != provider->end() && declared->is_object()) {
                coreTypes = declared->value("tokenTypes", Json::array());
                coreModifiers = declared->value("tokenModifiers", Json::array());
            }
        }
    }
    if (!coreTypes.is_array()) coreTypes = Json::array();
    if (!coreModifiers.is_array()) coreModifiers = Json::array();

    legend.coreTypeToServer.reserve(coreTypes.size());
    for (const auto& entry : coreTypes) {
        legend.coreTypeToServer.push_back(entry.is_string() ? index_of_or_append(legend.types, entry.get<std::string>()) : 0);
    }
    legend.coreModifierBitToServer.reserve(coreModifiers.size());
    for (const auto& entry : coreModifiers) {
        legend.coreModifierBitToServer.push_back(entry.is_string() ? index_of_or_append(legend.modifiers, entry.get<std::string>()) : 0);
    }
    return legend;
}

Json provider_capability(const Legend& legend) {
    return Json { { "legend", Json { { "tokenTypes", legend.types }, { "tokenModifiers", legend.modifiers } } },
                  { "full", true }, { "range", true } };
}

std::vector<Token> decode(const Json& semanticTokensResult) {
    std::vector<Token> tokens;
    if (!semanticTokensResult.is_object()) return tokens;
    const auto found = semanticTokensResult.find("data");
    if (found == semanticTokensResult.end() || !found->is_array()) return tokens;
    const Json& data = *found;
    int line { 0 };
    int column { 0 };
    for (std::size_t i = 0; i + 5 <= data.size(); i += 5) {
        if (!data[i].is_number_integer() || !data[i + 1].is_number_integer() || !data[i + 2].is_number_integer()
            || !data[i + 3].is_number_integer() || !data[i + 4].is_number_integer()) {
            break;   // malformed: stop rather than guess at the rest.
        }
        const int deltaLine { data[i].get<int>() };
        const int deltaStart { data[i + 1].get<int>() };
        const int length { data[i + 2].get<int>() };
        const auto type { static_cast<std::size_t>(std::max<std::int64_t>(0, data[i + 3].get<std::int64_t>())) };
        const auto modifiers { static_cast<std::uint32_t>(std::max<std::int64_t>(0, data[i + 4].get<std::int64_t>())) };
        if (deltaLine == 0) column += deltaStart; else { line += deltaLine; column = deltaStart; }
        tokens.push_back(Token { line, column, length, type, modifiers });
    }
    return tokens;
}

Json encode(std::vector<Token> tokens) {
    std::ranges::stable_sort(tokens, [](const Token& a, const Token& b) {
        return a.line != b.line ? a.line < b.line : a.startChar < b.startChar;
    });
    Json data = Json::array();
    int line { 0 };
    int column { 0 };
    for (const auto& token : tokens) {
        const int deltaLine { token.line - line };
        const int deltaStart { deltaLine == 0 ? token.startChar - column : token.startChar };
        data.push_back(deltaLine);
        data.push_back(deltaStart);
        data.push_back(token.length);
        data.push_back(static_cast<std::int64_t>(token.type));
        data.push_back(static_cast<std::int64_t>(token.modifiers));
        line = token.line;
        column = token.startChar;
    }
    return Json { { "data", std::move(data) } };
}

Json remap_core_tokens(const Json& coreResult, const Legend& legend) {
    std::vector<Token> tokens { decode(coreResult) };
    for (auto& token : tokens) {
        token.type = token.type < legend.coreTypeToServer.size() ? legend.coreTypeToServer[token.type] : token.type;
        std::uint32_t remapped { 0 };
        for (std::size_t bit = 0; bit < legend.coreModifierBitToServer.size(); ++bit) {
            if ((token.modifiers & (1u << bit)) != 0) remapped |= (1u << legend.coreModifierBitToServer[bit]);
        }
        token.modifiers = remapped;
    }
    return encode(std::move(tokens));
}

Json merge(const Json& core, const Json& native) {
    if (core.is_null() && native.is_null()) return Json(nullptr);
    std::vector<Token> coreTokens { decode(core) };
    std::vector<Token> merged { coreTokens };
    for (const auto& candidate : decode(native)) {
        const bool covered { std::ranges::any_of(coreTokens, [&](const Token& existing) {
            if (existing.line != candidate.line) return false;
            return candidate.startChar < existing.startChar + existing.length && existing.startChar < candidate.startChar + candidate.length;
        }) };
        if (!covered) merged.push_back(candidate);
    }
    return encode(std::move(merged));
}

} // namespace mcppls::orchestrator::tokens
