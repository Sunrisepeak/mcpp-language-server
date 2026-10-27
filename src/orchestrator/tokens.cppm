// Semantic tokens (design doc 2026-09-25 K/§7, contract T0): the legend mcppls owns, mapping the
// core engine's own legend into it by name, the LSP relative encoding, and the merge that lets the
// core engine's tokens win every position they cover while mcppls's own module-syntax tokens fill
// the gaps. Pure functions only: the workspace (which knows the core engine's capabilities and the
// document text) is what has state, this module only transforms what it is given.
export module mcppls.orchestrator.tokens;

import std;
import nlohmann.json;

export namespace mcppls::orchestrator::tokens {

using Json = nlohmann::json;

// The server's fixed base legend, before any name the core engine declares that is not already in
// it gets appended (build_legend). Positions never change, so the native tokenizer can use
// type_index/modifier_bit as compile-time-stable facts rather than looking anything up at runtime.
std::span<const std::string_view> base_types();
std::span<const std::string_view> base_modifiers();

// The index (types) or bit (modifiers) of a fixed-base name; 0 for a name not in the base (a
// programming error, since every name the native tokenizer asks for is one of the base's own).
std::size_t type_index(std::string_view name);
std::uint32_t modifier_bit(std::string_view name);

// The server's legend, and how to map a core engine's own token-type indices and modifier bits
// into it, by name (duplicates in the core engine's own list collapse to the first entry with that
// name, since they all map into the same, deduplicated, target). A name the core engine declares
// that is not already in the base is appended, so its position is stable across a restart of the
// core engine, or none at all, only for as long as this Legend itself is kept.
struct Legend {
    std::vector<std::string> types;
    std::vector<std::string> modifiers;
    std::vector<std::size_t> coreTypeToServer;         // core engine's token-type index -> this Legend's
    std::vector<std::size_t> coreModifierBitToServer;  // core engine's modifier bit position -> this Legend's
};

// `coreCapabilities` is the core engine's own `initialize` capabilities (or an empty object: no
// core engine, or it declared no semanticTokensProvider.legend): the base alone, then.
Legend build_legend(const Json& coreCapabilities);

// The `semanticTokensProvider` entry this server advertises: `legend`'s types and modifiers,
// `full: { delta: true }` (every whole answer carries a resultId, and a `full/delta` naming the
// last one is answered with its edits: mcpp design 2026-10-28 §11.1, H3), and `range` only when
// `range` says every engine can answer one: clangd 23.1 has no range request, and advertising it
// had clients send one clangd rejects with "method not found".
Json provider_capability(const Legend& legend, bool range);

// One token, decoded to absolute position: `length` and `startChar` are UTF-16 code units, as LSP
// requires; a token never spans two lines.
struct Token {
    int line { 0 };
    int startChar { 0 };
    int length { 0 };
    std::size_t type { 0 };
    std::uint32_t modifiers { 0 };
};

// Decodes a `SemanticTokens` result's `data` (LSP's relative encoding) into absolute tokens, in
// the order given. The type/modifier numbers are given back exactly as they came: legend-agnostic,
// so it decodes either a core engine's own result or one already in this server's legend.
// Tolerates a missing or malformed `data`: never throws, gives back what it could decode.
std::vector<Token> decode(const Json& semanticTokensResult);

// Encodes tokens (any order) into a `SemanticTokens` result: sorted ascending by line then column,
// relative-encoded, no `resultId`.
Json encode(std::vector<Token> tokens);

// Decodes a core engine's own `SemanticTokens` result and re-encodes it with every token's
// type/modifiers mapped into `legend`'s indices (build_legend's mapping), dropping its `resultId`.
Json remap_core_tokens(const Json& coreResult, const Legend& legend);

// The merged answer: `core` (already mapped into the server's legend, e.g. by remap_core_tokens)
// wins every position it covers; `native`'s tokens fill in only where `core` has none. Sorted,
// disjoint, relative-encoded. Null only when both inputs are null (neither engine answered).
Json merge(const Json& core, const Json& native);

// ---- the same, without a document in between (design 2026-10-28 §11.1, H4) -----------------------
//
// A large file's answer is some 70 000 tokens. Each step above decodes JSON and encodes JSON again,
// and the merge compared every native token with every core token; on the event loop that was a
// third of a second per answer of a 10 000-line file in a debug build. The steps below take and give
// tokens and integers: one pass each, and the JSON of the reply is made once, from the integers.

// `coreResult`'s tokens, decoded and mapped into `legend` in one pass over its data. Sorted when the
// engine's data is (LSP's relative encoding cannot express anything else).
std::vector<Token> remap(const Json& coreResult, const Legend& legend);

// Sorts `tokens` by line then column when they are not already: O(N) for sorted input.
void ensure_sorted(std::vector<Token>& tokens);

// `core` and `native` merged as `merge` does, both sorted: one pass over each, O(N + M).
std::vector<Token> merge_tokens(std::span<const Token> core, std::span<const Token> native);

// LSP's relative encoding of sorted tokens, as the integers of a reply's `data`.
std::vector<std::uint32_t> encode_data(std::span<const Token> sorted);

// The tokens of `data` (LSP's relative encoding) at their absolute positions.
std::vector<Token> decode_data(std::span<const std::uint32_t> data);

// A `SemanticTokens` result of `data`, with `resultId` when it is not empty.
Json result_of(std::span<const std::uint32_t> data, std::string_view resultId = {});

} // namespace mcppls::orchestrator::tokens
