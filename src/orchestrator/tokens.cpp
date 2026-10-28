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

Json provider_capability(const Legend& legend, bool range) {
    return Json { { "legend", Json { { "tokenTypes", legend.types }, { "tokenModifiers", legend.modifiers } } },
                  { "full", Json { { "delta", true } } }, { "range", range } };
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
    ensure_sorted(tokens);
    return result_of(encode_data(tokens));
}

Json remap_core_tokens(const Json& coreResult, const Legend& legend) { return result_of(encode_data(remap(coreResult, legend))); }

Json merge(const Json& core, const Json& native) {
    if (core.is_null() && native.is_null()) return Json(nullptr);
    std::vector<Token> coreTokens { decode(core) };
    std::vector<Token> nativeTokens { decode(native) };
    ensure_sorted(coreTokens);
    ensure_sorted(nativeTokens);
    return result_of(encode_data(merge_tokens(coreTokens, nativeTokens)));
}

std::vector<Token> remap(const Json& coreResult, const Legend& legend) {
    std::vector<Token> tokens;
    if (!coreResult.is_object()) return tokens;
    const auto found = coreResult.find("data");
    if (found == coreResult.end() || !found->is_array()) return tokens;
    const Json::array_t& data { found->get_ref<const Json::array_t&>() };
    tokens.reserve(data.size() / 5);
    // Each modifier bit of the engine's legend, as this legend's mask: one lookup per set bit.
    std::array<std::uint32_t, 32> modifierMask {};
    for (std::size_t bit = 0; bit < std::min<std::size_t>(32, legend.coreModifierBitToServer.size()); ++bit) {
        const std::size_t target { legend.coreModifierBitToServer[bit] };
        modifierMask[bit] = target < 32 ? (1u << target) : 0;
    }
    int line { 0 };
    int column { 0 };
    for (std::size_t i = 0; i + 5 <= data.size(); i += 5) {
        if (!data[i].is_number_integer() || !data[i + 1].is_number_integer() || !data[i + 2].is_number_integer()
            || !data[i + 3].is_number_integer() || !data[i + 4].is_number_integer()) {
            break;   // malformed: stop rather than guess at the rest.
        }
        const int deltaLine { data[i].get<int>() };
        const int deltaStart { data[i + 1].get<int>() };
        if (deltaLine == 0) column += deltaStart; else { line += deltaLine; column = deltaStart; }
        const auto type { static_cast<std::size_t>(std::max<std::int64_t>(0, data[i + 3].get<std::int64_t>())) };
        const auto modifiers { static_cast<std::uint32_t>(std::max<std::int64_t>(0, data[i + 4].get<std::int64_t>())) };
        std::uint32_t mapped { 0 };
        for (std::uint32_t rest { modifiers }; rest != 0; rest &= rest - 1) mapped |= modifierMask[static_cast<std::size_t>(std::countr_zero(rest))];
        tokens.push_back(Token { line, column, data[i + 2].get<int>(), type < legend.coreTypeToServer.size() ? legend.coreTypeToServer[type] : type, mapped });
    }
    return tokens;
}

void ensure_sorted(std::vector<Token>& tokens) {
    const auto before = [](const Token& a, const Token& b) { return a.line != b.line ? a.line < b.line : a.startChar < b.startChar; };
    if (!std::ranges::is_sorted(tokens, before)) std::ranges::stable_sort(tokens, before);
}

std::vector<Token> merge_tokens(std::span<const Token> core, std::span<const Token> native) {
    std::vector<Token> merged;
    merged.reserve(core.size() + native.size());
    std::size_t c { 0 };
    // The first core token not wholly before the native one (on an earlier line, or ending where it starts or before) is
    // the only one that can cover it: the core tokens are sorted and do not overlap.
    const auto wholly_before = [](const Token& token, const Token& of) {
        return token.line < of.line || (token.line == of.line && token.startChar + token.length <= of.startChar);
    };
    for (const auto& candidate : native) {
        while (c < core.size() && wholly_before(core[c], candidate)) merged.push_back(core[c++]);
        const bool covered { c < core.size() && core[c].line == candidate.line && core[c].startChar < candidate.startChar + candidate.length };
        if (!covered) merged.push_back(candidate);
    }
    while (c < core.size()) merged.push_back(core[c++]);
    return merged;
}

std::vector<std::uint32_t> encode_data(std::span<const Token> sorted) {
    std::vector<std::uint32_t> data;
    data.reserve(sorted.size() * 5);
    int line { 0 };
    int column { 0 };
    for (const auto& token : sorted) {
        const int deltaLine { token.line - line };
        data.push_back(static_cast<std::uint32_t>(deltaLine));
        data.push_back(static_cast<std::uint32_t>(deltaLine == 0 ? token.startChar - column : token.startChar));
        data.push_back(static_cast<std::uint32_t>(token.length));
        data.push_back(static_cast<std::uint32_t>(token.type));
        data.push_back(token.modifiers);
        line = token.line;
        column = token.startChar;
    }
    return data;
}

std::vector<Token> decode_data(std::span<const std::uint32_t> data) {
    std::vector<Token> tokens;
    tokens.reserve(data.size() / 5);
    int line { 0 };
    int column { 0 };
    for (std::size_t i = 0; i + 5 <= data.size(); i += 5) {
        const int deltaLine { static_cast<int>(data[i]) };
        const int deltaStart { static_cast<int>(data[i + 1]) };
        if (deltaLine == 0) column += deltaStart; else { line += deltaLine; column = deltaStart; }
        tokens.push_back(Token { line, column, static_cast<int>(data[i + 2]), data[i + 3], data[i + 4] });
    }
    return tokens;
}

Json result_of(std::span<const std::uint32_t> data, std::string_view resultId) {
    Json result { { "data", Json::array_t(data.begin(), data.end()) } };
    if (!resultId.empty()) result["resultId"] = std::string { resultId };
    return result;
}

} // namespace mcppls::orchestrator::tokens
