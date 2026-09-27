// Definitions in implementation units. clangd finds where a function is defined only in a file it has built, and its
// background index cannot build module imports (clangd 23.1), so a function declared in a module interface and defined in
// an implementation unit nobody opened resolves to its declaration. The engine builds that module's other units and asks
// again (robustness design C10); what is decided here is whether a location is a declaration only, and which units to build.
//
// N-8 (design plan §9.1): picking those units by file stem and directory alone misses a module laid out like mcpp's own
// `mcpp.build.prepare` (17 units, the declaration in a partition, the definition in whichever implementation unit happens
// to hold that phase) -- the module's declaration and its callers do not say which unit defines a name. The rest of this
// module is a lexical scanner that answers that, by name: `declared_function_at` reads a declaration's shape, `function_
// definitions` finds every definition of a name in a file's text, `same_function` decides whether one matches the other,
// and `units_defining` ranks a module's units by that instead of by file stem. It is lexical, not a parse: it can be fooled
// by a type alias, an unusual macro, or a name reused across unrelated scopes, and it is built to fail closed in that case
// (no match) rather than to guess -- clangd's own answer, or a declaration-only fallback, is always what asked for it.
export module mcppls.engine.clangd.definition;

import std;
import mcppls.base.text;

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

// A function or member function's declaration, as written: its unqualified name (a destructor's is `~Class`, matching how
// `function_definitions` names one; operator overloads are not recognized), the namespaces and classes enclosing it in
// that file (outer to inner, `namespace a::b {`/`export namespace`/`inline namespace` and `class X {`/`struct X {` bodies;
// an anonymous namespace or class contributes no name but still nests), and its parameter list, normalized as
// `function_definitions` normalizes one (so the two compare equal when they agree).
struct DeclaredFunction {
    std::string name;
    std::vector<std::string> scopes;
    std::vector<std::string> parameters;
};

// `declared_function_at(text, nameOffset)` reads the declaration whose name begins at byte `nameOffset` of `text` (as
// `declaration_kind` is given the name's end); nullopt when it is not a function or member function declaration, or when
// `nameOffset` is not the start of an identifier.
std::optional<DeclaredFunction> declared_function_at(std::string_view text, std::size_t nameOffset);

// One definition of a function or member function found by `function_definitions`: its name qualified by the namespace
// blocks enclosing it plus whatever qualification the declarator itself wrote (`namespace a::b { void f(...) {`  ->
// `a::b::f`; `void hello::add(...) {` at file scope -> `hello::add`), the unqualified name, the name token's byte range
// and its line/character range, and its normalized parameter list.
struct FunctionDefinition {
    std::string qualifiedName;
    std::string name;
    std::size_t nameOffset { 0 };
    std::size_t nameEnd { 0 };
    base::Range nameRange;
    std::vector<std::string> parameters;
};

// Every definition of a function or member function named `name` (unqualified; a destructor is `~Class`) found lexically
// in `text` -- a body, a constructor's `: ` initializer list, or `= default`/`= delete`, consistent with `declaration_kind`.
// Comments, string/character literals (raw strings included) and preprocessor lines are skipped, and a call, a declaration
// without a body, and a name used inside a function body are not definitions. No preprocessing is done: a name hidden or
// changed by a macro is read as written.
std::vector<FunctionDefinition> function_definitions(std::string_view text, std::string_view name);

// True when `definition` is plausibly what `declaration` declares: the same name, the same parameters once both are
// normalized, and the scopes each side wrote agree on as many of the innermost levels as the shorter side spelled out (a
// declaration inside `namespace mcpp::build {` matches a definition qualified `mcpp::build::f`, one qualified only `f`
// inside a reopened `namespace mcpp::build { ... }`, and also one qualified only `f` with no enclosing namespace written
// at all -- lexically it cannot tell that last one from a same-named `f` in a different, unrelated scope). Parameters that
// differ only because one side used a type alias the other spelled out are never equal here, so they never match: this is
// a lexical comparison, not a resolution of what the alias names.
bool same_function(const DeclaredFunction& declaration, const FunctionDefinition& definition);

// `units_to_search`'s ranking, refined by whether each unit's text (as `read` returns it; nullopt -- unreadable -- ranks a
// unit as not defining it, never as an error) holds a definition of `name`: those units come first, in path order; the
// rest keep the relative order `units` was given in (pass `units_to_search`'s own result to keep its stem/directory
// ranking for them -- this helper does not have the interface path that ranking needs). At most `limit` paths.
std::vector<std::string> units_defining(std::string_view name, std::span<const UnitOfModule> units,
                                        const std::function<std::optional<std::string>(const std::string&)>& read,
                                        std::size_t limit);

} // namespace mcppls::engine::clangd
