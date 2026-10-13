// What a file declares, read from its text alone (answer-layer design 4.7): the members of a class, and where a name
// is declared. Lexical, and cautious: what cannot be read for certain -- a preprocessor conditional inside the body, a
// body that does not close, a head that is not a class -- gives no answer rather than a guess. The answer layer uses
// it to bring a cached answer up to a file that changed since: a member added or removed, a declaration moved.
export module mcppls.orchestrator.outline;

import std;

export namespace mcppls::orchestrator::outline {

// The members of the class, struct or union whose name is on `line` (0-based) of `text`, at or after `column`: data
// members, member functions, member types and using-declarations, each once, in order. nullopt when the class's body
// cannot be read for certain.
std::optional<std::vector<std::string>> class_members(std::string_view text, int line, int column = 0);

// Where `name` is declared in `text` at namespace or class scope -- a function, a variable, a type, a namespace, an
// alias -- as (line, column), 0-based, in byte columns. Uses in function bodies are not declarations.
struct Place {
    int line { 0 };
    int column { 0 };
    bool operator==(const Place&) const = default;
};
std::vector<Place> declarations(std::string_view text, std::string_view name);

} // namespace mcppls::orchestrator::outline
