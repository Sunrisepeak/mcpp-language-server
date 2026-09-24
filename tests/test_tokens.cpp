// Semantic tokens (design doc 2026-09-25 K/§7): the server's legend, its mapping of a core
// engine's own token-type indices and modifier bits (including duplicates), the relative
// encode/decode round trip, and the merge that lets the core engine win every position it covers.
import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.orchestrator.tokens;

using Json = nlohmann::json;
using mcppls::orchestrator::tokens::Legend;
using mcppls::orchestrator::tokens::Token;
namespace tokens = mcppls::orchestrator::tokens;

int main() {
    using namespace mcppls::testing;

    "the base legend has the fixed types and modifiers, in order"_test = [] {
        const Legend legend { tokens::build_legend(Json::object()) };
        expect(legend.types.front() == "namespace");
        expect(legend.types.back() == "module");
        expect(std::ranges::find(legend.types, "keyword") != legend.types.end());
        expect(std::ranges::find(legend.types, "unknown") != legend.types.end());   // clangd's own extra
        expect(legend.modifiers.front() == "declaration");
        expect(legend.modifiers.back() == "partition");
        expect(legend.coreTypeToServer.empty() && legend.coreModifierBitToServer.empty());
        expect(tokens::type_index("keyword") == static_cast<std::size_t>(std::ranges::find(legend.types, "keyword") - legend.types.begin()));
        expect(tokens::modifier_bit("partition") == (1u << (legend.modifiers.size() - 1)));
    };

    "clangd's legend, including its own duplicates, maps by name"_test = [] {
        // Measured 2026-09-25 on the bundled clangd 23.1: 'variable' appears twice.
        const Json clangdCapabilities { { "semanticTokensProvider",
            Json { { "legend", Json { { "tokenTypes", Json::array({ "variable", "variable", "namespace" }) },
                                       { "tokenModifiers", Json::array({ "declaration", "declaration", "static" }) } } } } } };
        const Legend legend { tokens::build_legend(clangdCapabilities) };
        // No name clangd declares here is missing from the fixed base, so the legend is unchanged.
        expect(legend.types.size() == tokens::build_legend(Json::object()).types.size());
        expect(fatal(legend.coreTypeToServer.size() == 3u));
        expect(legend.coreTypeToServer[0] == legend.coreTypeToServer[1]);   // both "variable" collapse to the same index
        expect(legend.types[legend.coreTypeToServer[0]] == "variable");
        expect(legend.types[legend.coreTypeToServer[2]] == "namespace");
        expect(fatal(legend.coreModifierBitToServer.size() == 3u));
        expect(legend.coreModifierBitToServer[0] == legend.coreModifierBitToServer[1]);
        expect(legend.modifiers[legend.coreModifierBitToServer[0]] == "declaration");
    };

    "a name the core engine declares that mcppls does not already have is appended"_test = [] {
        const Json capabilities { { "semanticTokensProvider", Json { { "legend", Json { { "tokenTypes", Json::array({ "namespace", "somethingNew" }) },
                                                                                          { "tokenModifiers", Json::array({ "somethingElse" }) } } } } } };
        const Legend legend { tokens::build_legend(capabilities) };
        const auto base = tokens::build_legend(Json::object());
        expect(legend.types.size() == base.types.size() + 1);
        expect(legend.types.back() == "somethingNew");
        expect(legend.coreTypeToServer[1] == legend.types.size() - 1);
        expect(legend.modifiers.size() == base.modifiers.size() + 1);
        expect(legend.modifiers.back() == "somethingElse");
    };

    "encode and decode round-trip, sorted and relative"_test = [] {
        std::vector<Token> input { { 3, 5, 4, 2, 0 }, { 1, 0, 6, 1, 0 }, { 1, 7, 3, 0, 5 } };
        const Json encoded = tokens::encode(input);
        expect(fatal(encoded.contains("data") && encoded["data"].is_array()));
        const auto decoded = tokens::decode(encoded);
        expect(fatal(decoded.size() == 3u));
        // Sorted ascending by line then column.
        expect(decoded[0].line == 1 && decoded[0].startChar == 0 && decoded[0].type == 1u);
        expect(decoded[1].line == 1 && decoded[1].startChar == 7 && decoded[1].modifiers == 5u);
        expect(decoded[2].line == 3 && decoded[2].startChar == 5 && decoded[2].length == 4);
    };

    "decode tolerates a missing or malformed data array"_test = [] {
        expect(tokens::decode(Json(nullptr)).empty());
        expect(tokens::decode(Json::object()).empty());
        expect(tokens::decode(Json { { "data", Json::array({ 0, 0, 1 }) } }).empty());        // short entry
        expect(tokens::decode(Json { { "data", Json::array({ 0, 0, 1, "x", 0 }) } }).empty()); // wrong type
    };

    "remap_core_tokens drops resultId and maps into the server's indices"_test = [] {
        const Json clangdCapabilities { { "semanticTokensProvider", Json { { "legend",
            Json { { "tokenTypes", Json::array({ "variable", "namespace" }) }, { "tokenModifiers", Json::array({ "declaration" }) } } } } } };
        const Legend legend { tokens::build_legend(clangdCapabilities) };
        const Json clangdResult { { "resultId", "1" }, { "data", Json::array({ 0, 0, 5, 1, 1 }) } };   // namespace, declaration
        const Json remapped = tokens::remap_core_tokens(clangdResult, legend);
        expect(!remapped.contains("resultId"));
        const auto decoded = tokens::decode(remapped);
        expect(fatal(decoded.size() == 1u));
        expect(legend.types[decoded[0].type] == "namespace");
        expect((decoded[0].modifiers & tokens::modifier_bit("declaration")) != 0);
    };

    "merge: the core engine wins every position it covers, native fills the gaps"_test = [] {
        // core: one token at [0,0)-[0,6) ("import" as keyword, say); native additionally offers a
        // token that overlaps it (dropped) and one that does not (kept).
        const Json core = tokens::encode(std::vector<Token> { { 0, 0, 6, tokens::type_index("keyword"), 0 } });
        const Json native = tokens::encode(std::vector<Token> { { 0, 0, 6, tokens::type_index("module"), 0 },      // overlaps core: dropped
                                                                 { 0, 7, 3, tokens::type_index("module"), 0 } });  // does not: kept
        const Json merged = tokens::merge(core, native);
        const auto decoded = tokens::decode(merged);
        expect(fatal(decoded.size() == 2u));
        expect(decoded[0].startChar == 0 && decoded[0].type == tokens::type_index("keyword"));
        expect(decoded[1].startChar == 7 && decoded[1].type == tokens::type_index("module"));
        // Sorted, and adjacent tokens (touching but not overlapping) are not treated as covered.
        expect(decoded[0].line == decoded[1].line);
    };

    "merge: with no core engine, native's tokens answer alone (not null)"_test = [] {
        const Json native = tokens::encode(std::vector<Token> { { 2, 0, 6, tokens::type_index("keyword"), 0 } });
        const Json merged = tokens::merge(Json(nullptr), native);
        expect(!merged.is_null());
        expect(tokens::decode(merged).size() == 1u);
    };

    "merge: with neither engine, the answer is null"_test = [] { expect(tokens::merge(Json(nullptr), Json(nullptr)).is_null()); };

    "provider_capability advertises full and range, no delta"_test = [] {
        const Json capability = tokens::provider_capability(tokens::build_legend(Json::object()));
        expect(capability.value("full", false) == true);
        expect(capability.value("range", false) == true);
        expect(!capability.contains("delta"));
        expect(capability["legend"]["tokenTypes"].is_array() && !capability["legend"]["tokenTypes"].empty());
    };

    return report();
}
