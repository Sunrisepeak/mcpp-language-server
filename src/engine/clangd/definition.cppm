// Definitions in implementation units. clangd finds where a function is defined only in a file it has built, and its
// background index cannot build module imports (clangd 23.1), so a function declared in a module interface and defined in
// an implementation unit nobody opened resolves to its declaration. The engine builds that module's other units and asks
// again (robustness design C10); what is decided here is whether a location is a declaration only, and which units to build.
export module mcppls.engine.clangd.definition;

import std;

export namespace mcppls::engine::clangd {

enum class DeclarationKind { declaration, definition, unknown };

// What the declaration whose name ends at byte `nameEnd` of `text` is: a declaration only (`void f(int);`, `struct S;`, a
// member function declared in its class), a definition (a body, an initializer, `= default`, a base clause), or unknown.
DeclarationKind declaration_kind(std::string_view text, std::size_t nameEnd);

struct UnitOfModule {
    std::string path;
    bool partition { false };   // a partition rather than an implementation unit
};

// Which of a module's units other than its interface to build, at most `limit`: implementation units before partitions,
// and among them the ones named like the interface, then the ones nearest to it.
std::vector<std::string> units_to_search(std::string_view interfacePath, std::span<const UnitOfModule> units, std::size_t limit);

} // namespace mcppls::engine::clangd
