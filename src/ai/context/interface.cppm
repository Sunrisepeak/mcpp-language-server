// A module's interface summary (overall design 7.2, S5 4.2): what `import m;` brings in — the
// declarations the primary interface and the partitions it re-exports export, with their
// documentation, and the modules it re-exports — without any implementation, within a budget.
export module mcppls.ai.context.interface;

import std;
import nlohmann.json;
import mcppls.spec.query;
import mcppls.ai.query.view;
import mcppls.ai.query.symbols;

export namespace mcppls::ai::context {

struct InterfaceDeclaration {
    std::string kind;            // function | class | struct | union | enum | concept | alias | variable | namespace | other
    std::string name;
    std::string qualifiedName;
    std::string declaration;     // without body or initializer
    std::string documentation;
    std::string unit;            // "m" or "m:p": the unit that exports it
    spec::Location location;
    bool conditional { false };  // inside #if, #ifdef or #ifndef
};

struct ModuleInterface {
    spec::Snapshot snapshot;
    std::string module;
    std::vector<std::string> files;          // the interface units read, primary first
    std::vector<std::string> reexports;      // other modules `export import` brings in
    std::vector<InterfaceDeclaration> declarations;
    std::size_t total { 0 };
    bool truncated { false };                // declarations were left out for the budget
    bool documentationOmitted { false };     // documentation was left out for the budget
};

// At most `maxCharacters` of declaration and documentation text: documentation goes first, then declarations.
query::Outcome<ModuleInterface> module_interface(query::View& view, std::string_view module, std::size_t maxCharacters);

// Every exported declaration of the workspace's module interface units whose name, or qualified
// name, is `name`: what mcppls's own engine knows of a symbol without a core engine.
std::vector<InterfaceDeclaration> exported_named(query::View& view, std::string_view name);

// query::find_symbols, and for a name the core engine does not know (or with no core engine) the
// exported declarations of that name (S5 3.1): a module's exports are found without clangd.
query::Outcome<query::Symbols> locate_symbols(query::View& view, const query::SymbolTarget& target, query::Limit limit, bool describe,
                                              query::Clock::time_point deadline);

nlohmann::json to_json(const ModuleInterface& interface);
nlohmann::json to_json(const InterfaceDeclaration& declaration);

} // namespace mcppls::ai::context
