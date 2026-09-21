// Lexical extraction of a module interface's exported declarations: stage N2
// of mcppls's own engine (design section 5.3), feeding the module interface
// summary and the semantic diff (design sections 7.2, 7.4) without clangd.
// Like scan_source, this does not preprocess: declarations inside conditional
// blocks are still reported, marked conditional.
export module mcppls.engine.native.exports;

import std;
import mcppls.base.text;

export namespace mcppls::index {

enum class DeclarationKind {
    function,
    class_type,
    struct_type,
    union_type,
    enum_type,
    concept_,
    alias,
    variable,
    namespace_,
    reexport,
    other,
};

std::string_view to_string(DeclarationKind kind);

struct ExportedDeclaration {
    DeclarationKind kind { DeclarationKind::other };
    std::string name;            // unqualified name; the module name ("m" or ":p" as written) for a re-export
    std::string qualifiedName;   // with the enclosing namespaces, e.g. "hello::greet"; equals name at global scope
    std::string declaration;     // the declaration without body or initializer, whitespace collapsed to single spaces
    std::string documentation;   // comment lines immediately above it, markers and common indentation stripped
    base::Range nameRange;       // the name token (for a re-export, the module name)
    bool conditional { false };  // inside #if / #ifdef / #ifndef
};

// Declarations the source exports, in source order. Lexical, like scan_source: no preprocessing.
std::vector<ExportedDeclaration> exported_declarations(std::string_view source);

} // namespace mcppls::index
