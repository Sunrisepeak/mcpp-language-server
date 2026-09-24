// A lexical scanner for module declarations and imports. It does not
// preprocess: declarations inside conditional blocks are reported and marked
// conditional, and the result is marked uncertain.
export module mcppls.project.scan;

import std;
import mcppls.base.text;
import mcppls.spec.database;

export namespace mcppls::project {

struct ModuleDeclaration {
    std::string module;            // "hello.greet"
    std::string partition;         // "detail" or empty
    bool isExported { false };
    bool conditional { false };    // inside #if / #ifdef / #ifndef
    base::Range nameRange;         // covers "hello.greet:detail"
};

struct ImportDeclaration {
    std::string module;            // empty for a partition import of the current module
    std::string partition;
    bool isExported { false };
    bool isHeaderUnit { false };
    std::string header;            // "<vector>" or "\"a.h\"" for header units
    bool conditional { false };
    base::Range nameRange;         // covers the module name, ":partition" or the header
};

struct ScanResult {
    std::optional<ModuleDeclaration> declaration;
    std::vector<ImportDeclaration> imports;
    bool uncertain { false };
};

ScanResult scan_source(std::string_view text);

spec::Role role_of(const ScanResult& result);
// "m" or "m:p" for units that can be imported; empty otherwise.
std::string provided_name(const ScanResult& result);
// Imported module names with partitions qualified ("m:p"); an implementation unit
// `module m;` implicitly requires "m". Header units are not included.
std::vector<std::string> required_names(const ScanResult& result);
// "a.b" or "a.b:c.d": dotted identifiers, with at most one partition. A build tool's scan of a file
// saved mid-edit can report `hello.` (import-hang plan §5); such a name is no module.
bool is_module_name(std::string_view name);
// The full name an import refers to, given the importing unit's declaration.
std::string imported_name(const ScanResult& result, const ImportDeclaration& import);

// True when the file name has a conventional C++ source or module extension.
bool is_cxx_source_name(std::string_view path);

} // namespace mcppls::project
