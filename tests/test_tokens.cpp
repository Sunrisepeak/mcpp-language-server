// Semantic tokens (design doc 2026-09-25 K/§7): the server's legend, its mapping of a core
// engine's own token-type indices and modifier bits (including duplicates), the relative
// encode/decode round trip, and the merge that lets the core engine win every position it covers.
import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.orchestrator.tokens;
import mcppls.orchestrator.routing;

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

    "remap maps types and every set modifier bit in one pass, as remap_core_tokens did"_test = [] {
        const Json clangdCapabilities { { "semanticTokensProvider", Json { { "legend",
            Json { { "tokenTypes", Json::array({ "variable", "namespace", "function" }) },
                   { "tokenModifiers", Json::array({ "declaration", "static", "readonly" }) } } } } } };
        const Legend legend { tokens::build_legend(clangdCapabilities) };
        // Three tokens on two lines: function (declaration|readonly), variable (static), namespace (none).
        const Json clangdResult { { "data", Json::array({ 0, 4, 3, 2, 5, 0, 6, 1, 0, 2, 2, 1, 5, 1, 0 }) } };
        const auto mapped = tokens::remap(clangdResult, legend);
        expect(fatal(mapped.size() == 3u));
        expect(mapped[0].line == 0 && mapped[0].startChar == 4 && legend.types[mapped[0].type] == "function");
        expect(mapped[0].modifiers == (tokens::modifier_bit("declaration") | tokens::modifier_bit("readonly")));
        expect(mapped[1].line == 0 && mapped[1].startChar == 10 && legend.types[mapped[1].type] == "variable");
        expect(mapped[1].modifiers == tokens::modifier_bit("static"));
        expect(mapped[2].line == 2 && mapped[2].startChar == 1 && legend.types[mapped[2].type] == "namespace" && mapped[2].modifiers == 0u);
        expect(tokens::decode(tokens::remap_core_tokens(clangdResult, legend)).size() == 3u);
    };

    "encode_data and decode_data are LSP's relative encoding, both ways"_test = [] {
        const std::vector<Token> sorted { { 0, 0, 6, 15, 0 }, { 0, 7, 3, 27, 1 }, { 4, 2, 5, 12, 3 } };
        const auto data = tokens::encode_data(sorted);
        expect(data == std::vector<std::uint32_t> { 0, 0, 6, 15, 0, 0, 7, 3, 27, 1, 4, 2, 5, 12, 3 });
        const auto back = tokens::decode_data(data);
        expect(fatal(back.size() == 3u));
        expect(back[2].line == 4 && back[2].startChar == 2 && back[2].length == 5 && back[2].type == 12u && back[2].modifiers == 3u);
        expect(tokens::result_of(data, "7")["resultId"] == "7");
        expect(!tokens::result_of(data).contains("resultId"));
    };

    "merge_tokens is the quadratic merge's answer, in one pass over each side"_test = [] {
        // The old rule, written out: a native token is dropped when any core token on its line overlaps it.
        const auto reference = [](const std::vector<Token>& core, const std::vector<Token>& native) {
            std::vector<Token> merged { core };
            for (const auto& candidate : native) {
                const bool covered { std::ranges::any_of(core, [&](const Token& existing) {
                    return existing.line == candidate.line && candidate.startChar < existing.startChar + existing.length
                        && existing.startChar < candidate.startChar + candidate.length;
                }) };
                if (!covered) merged.push_back(candidate);
            }
            tokens::ensure_sorted(merged);
            return tokens::encode_data(merged);
        };
        std::mt19937 random { 7 };
        for (int round { 0 }; round < 200; ++round) {
            std::vector<Token> core;
            std::vector<Token> native;
            for (int line { 0 }; line < 12; ++line) {
                int column { 0 };
                while (column < 60) {   // core tokens of a line do not overlap one another, as clangd's do not
                    column += static_cast<int>(random() % 6);
                    const int length { 1 + static_cast<int>(random() % 5) };
                    if (random() % 3 == 0) core.push_back(Token { line, column, length, 1, 0 });
                    column += length;
                }
                for (int i { 0 }; i < 4; ++i) {
                    native.push_back(Token { line, static_cast<int>(random() % 60), 1 + static_cast<int>(random() % 7), 2, 0 });
                }
            }
            tokens::ensure_sorted(native);
            std::vector<Token> nativeDisjoint;   // the module-syntax tokens never overlap one another either
            for (const auto& token : native) {
                if (nativeDisjoint.empty() || nativeDisjoint.back().line != token.line
                    || nativeDisjoint.back().startChar + nativeDisjoint.back().length <= token.startChar) nativeDisjoint.push_back(token);
            }
            expect(tokens::encode_data(tokens::merge_tokens(core, nativeDisjoint)) == reference(core, nativeDisjoint)) << "round " << round;
        }
    };

    "ensure_sorted leaves sorted tokens as they are and sorts the rest"_test = [] {
        std::vector<Token> tokens { { 2, 0, 1, 0, 0 }, { 0, 5, 1, 0, 0 }, { 0, 1, 1, 0, 0 } };
        tokens::ensure_sorted(tokens);
        expect(tokens[0].line == 0 && tokens[0].startChar == 1 && tokens[1].startChar == 5 && tokens[2].line == 2);
    };

    "provider_capability advertises full, no delta, and range only when every engine answers it"_test = [] {
        const Json capability = tokens::provider_capability(tokens::build_legend(Json::object()), true);
        expect(capability.value("full", false) == true);
        expect(capability.value("range", false) == true);
        expect(!capability.contains("delta"));
        expect(capability["legend"]["tokenTypes"].is_array() && !capability["legend"]["tokenTypes"].empty());
        expect(tokens::provider_capability(tokens::build_legend(Json::object()), false).value("range", true) == false);
        // clangd 23.1 declares full (with delta) and no range: a range request would be rejected by it.
        const Json clangd = Json::parse(R"({"semanticTokensProvider":{"full":{"delta":true},"legend":{"tokenTypes":["variable"],"tokenModifiers":[]}}})");
        expect(mcppls::orchestrator::merge_capabilities(clangd)["semanticTokensProvider"].value("range", true) == false);
        const Json ranged = Json::parse(R"({"semanticTokensProvider":{"full":true,"range":true,"legend":{"tokenTypes":[],"tokenModifiers":[]}}})");
        expect(mcppls::orchestrator::merge_capabilities(ranged)["semanticTokensProvider"].value("range", false) == true);
        expect(mcppls::orchestrator::merge_capabilities(Json::object())["semanticTokensProvider"].value("range", false) == true)
            << "mcppls's own engine alone answers range requests";
    };

    return report();
}
