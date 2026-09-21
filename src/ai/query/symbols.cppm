// Symbol queries (overall design 7.1, S5 3): find a symbol by name, position or identifier,
// describe it, and list its references, callers and callees.
export module mcppls.ai.query.symbols;

import std;
import nlohmann.json;
import mcppls.spec.query;
import mcppls.ai.query.view;

export namespace mcppls::ai::query {

// What a symbol query names: an identifier from an earlier result, a name ("greet", "hello::greet"),
// or a position (file, line, column, all from 1). Kind and module narrow a name.
struct SymbolTarget {
    std::string id;
    std::string name;
    std::string file;
    int line { 0 };
    int column { 0 };
    std::string kind;
    std::string module;
};

struct Symbol {
    std::string id;              // the USR (S5 2.3)
    std::string name;
    std::string qualifiedName;
    std::string kind;
    std::string module;          // of the unit that declares it
    std::optional<spec::Location> declaration;
    std::optional<spec::Location> definition;
    std::string signature;
    std::string type;            // a return type or a variable's type
    std::string documentation;
};

struct Symbols {
    spec::Snapshot snapshot;
    std::vector<Symbol> symbols;
    std::size_t total { 0 };
    bool truncated { false };
};

struct ReferenceGroup {
    std::string module;
    std::string file;
    std::vector<spec::Location> references;
};

// S5 3.4: where a symbol's references were looked for. The core engine's index does not see into
// units whose imports it cannot build, so the units that can use the symbol are opened and built:
// those of its module and its importers. `unsearched` lists the ones beyond the budget.
struct SearchScope {
    std::size_t searched { 0 };
    std::vector<std::string> unsearched;
    bool complete() const { return unsearched.empty(); }
};

struct References {
    spec::Snapshot snapshot;
    Symbol symbol;
    std::vector<ReferenceGroup> groups;
    std::size_t total { 0 };
    bool truncated { false };
    SearchScope scope;
};

enum class CallDirection { incoming, outgoing };

struct Call {
    Symbol symbol;                           // the caller, or the callee
    std::vector<spec::Location> sites;       // where the call is, in the caller
};

struct Calls {
    spec::Snapshot snapshot;
    Symbol symbol;
    CallDirection direction { CallDirection::incoming };
    std::vector<Call> calls;
    std::size_t total { 0 };
    bool truncated { false };
    SearchScope scope;
};

// How many units a reference or caller search opens at most.
inline constexpr std::size_t SEARCH_BUDGET { 48 };

// Every symbol matching the target; described (signature, type, documentation) when `describe`.
Outcome<Symbols> find_symbols(View& view, const SymbolTarget& target, Limit limit, bool describe, Clock::time_point deadline);
// Exactly one symbol, or not-found, or ambiguous with the candidates.
Outcome<Symbol> resolve_symbol(View& view, const SymbolTarget& target, Clock::time_point deadline);
Outcome<References> find_references(View& view, const SymbolTarget& target, bool includeDeclaration, Limit limit, Clock::time_point deadline);
Outcome<Calls> find_calls(View& view, const SymbolTarget& target, CallDirection direction, Limit limit, Clock::time_point deadline);

// Hover content of the core engine, taken apart: a heading "### kind `name`", then a return type,
// parameters, a type and documentation, then the declaration in a code block after "// In scope".
struct HoverParts {
    std::string kind;
    std::string name;
    std::string type;
    std::string documentation;
    std::string signature;
    std::string scope;
};
HoverParts parse_hover(std::string_view markdown);
// The name a USR ends with, when it can be read from it ("c:@N@hello@F@greet#..." -> "greet").
std::string name_from_usr(std::string_view usr);

nlohmann::json to_json(const Symbol& symbol);
nlohmann::json to_json(const Symbols& symbols);
nlohmann::json to_json(const References& references);
nlohmann::json to_json(const Calls& calls);

} // namespace mcppls::ai::query
