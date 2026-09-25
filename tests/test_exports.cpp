import std;
import mcppls.testing;
import mcppls.base.text;
import mcppls.engine.native.exports;

using namespace mcppls::index;
using mcppls::base::Position;
using mcppls::base::Range;

int main() {
    using namespace mcppls::testing;

    "module declarations are not declarations"_test = [] {
        expect(exported_declarations("export module m;\n").empty());
        expect(exported_declarations("export module m:p;\nimport std;\n").empty());
        expect(exported_declarations("module m;\nvoid f() {}\n").empty());
    };

    "export import is a re-export"_test = [] {
        const auto result = exported_declarations("export module m;\nexport import other;\nexport import :part;\n");
        expect(fatal(result.size() == 2u));
        expect(result[0].kind == DeclarationKind::reexport);
        expect(result[0].name == "other");
        expect(result[0].qualifiedName == "other");
        expect(result[0].declaration == "import other");
        expect(result[0].nameRange == Range { Position { 1, 14 }, Position { 1, 19 } });
        expect(result[1].kind == DeclarationKind::reexport);
        expect(result[1].name == ":part");
        expect(result[1].declaration == "import :part");
        expect(result[1].nameRange == Range { Position { 2, 14 }, Position { 2, 19 } });
        expect(to_string(result[1].kind) == "reexport");
    };

    // Fix plan F2: a UTF-8 byte order mark before the module declaration is skipped and takes no column.
    "a byte order mark changes nothing"_test = [] {
        const std::string mark { "\xEF\xBB\xBF" };
        const auto result = exported_declarations(mark + "export module m;\nexport int answer();\n");
        expect(fatal(result.size() == 1u)) << result.size();
        expect(result[0].name == "answer" && result[0].nameRange == Range { Position { 1, 11 }, Position { 1, 17 } });
        const auto first = exported_declarations(mark + "export int first();\n");
        expect(fatal(first.size() == 1u));
        expect(first[0].nameRange == Range { Position { 0, 11 }, Position { 0, 16 } });
    };

    "a plain import is not a declaration"_test = [] {
        expect(exported_declarations("export module m;\nimport std;\nimport other;\n").empty());
    };

    "export block exports every declaration inside it; a non-exported one is not reported"_test = [] {
        const auto result = exported_declarations(
            "export module m;\n"
            "export {\n"
            "int a();\n"
            "int b();\n"
            "}\n"
            "int c();\n");
        expect(fatal(result.size() == 2u));
        expect(result[0].kind == DeclarationKind::function && result[0].name == "a");
        expect(result[1].kind == DeclarationKind::function && result[1].name == "b");
    };

    "export namespace exports itself and its members, qualified"_test = [] {
        const auto result = exported_declarations(
            "export module m;\n"
            "export namespace hello {\n"
            "std::string greet(std::string_view who);\n"
            "namespace detail {\n"
            "void helper();\n"
            "}\n"
            "}\n");
        expect(fatal(result.size() == 4u));
        expect(result[0].kind == DeclarationKind::namespace_);
        expect(result[0].name == "hello");
        expect(result[0].qualifiedName == "hello");
        expect(result[0].declaration == "namespace hello");
        expect(result[1].kind == DeclarationKind::function);
        expect(result[1].name == "greet");
        expect(result[1].qualifiedName == "hello::greet");
        expect(result[2].kind == DeclarationKind::namespace_);
        expect(result[2].name == "detail");
        expect(result[2].qualifiedName == "hello::detail");
        expect(result[3].name == "helper");
        expect(result[3].qualifiedName == "hello::detail::helper");
    };

    "export namespace a::b qualifies as a::b::"_test = [] {
        const auto result = exported_declarations("export module m;\nexport namespace a::b {\nvoid f();\n}\n");
        expect(fatal(result.size() == 2u));
        expect(result[0].kind == DeclarationKind::namespace_);
        expect(result[0].name == "b");
        expect(result[0].qualifiedName == "a::b");
        expect(result[1].qualifiedName == "a::b::f");
    };

    "function prefixes and trailing specifiers"_test = [] {
        const auto result = exported_declarations(
            "export module m;\n"
            "export template <typename T> T twice(T value);\n"
            "export inline constexpr int compute() noexcept;\n"
            "export static int helper();\n"
            "export [[nodiscard]] int query();\n"
            "export auto make() -> int;\n"
            "export void reset() = delete;\n"
            "export void use() = default;\n");
        expect(fatal(result.size() == 7u));
        expect(std::ranges::all_of(result, [](const auto& entry) { return entry.kind == DeclarationKind::function; }));
        expect(result[0].name == "twice");
        expect(result[0].qualifiedName == "twice");
        expect(result[0].declaration == "template <typename T> T twice(T value)");
        expect(result[1].declaration == "inline constexpr int compute() noexcept");
        expect(result[2].declaration == "static int helper()");
        expect(result[3].name == "query");
        expect(result[3].declaration == "[[nodiscard]] int query()");
        expect(result[4].name == "make");
        expect(result[4].declaration == "auto make() -> int");
        expect(result[5].name == "reset");
        expect(result[5].declaration == "void reset() = delete");
        expect(result[6].declaration == "void use() = default");
    };

    "class struct union enum, with and without a body, base clauses kept"_test = [] {
        const auto result = exported_declarations(
            "export module m;\n"
            "export struct Point { double x; double y; };\n"
            "export struct Derived : Point { int tag; };\n"
            "export class Empty;\n"
            "export union U { int i; float f; };\n"
            "export enum class Color { Red, Green, Blue };\n"
            "export enum Legacy { A, B };\n");
        expect(fatal(result.size() == 6u));
        expect(result[0].kind == DeclarationKind::struct_type && result[0].name == "Point");
        expect(result[0].declaration == "struct Point");
        expect(result[0].nameRange == Range { Position { 1, 14 }, Position { 1, 19 } });
        expect(result[1].kind == DeclarationKind::struct_type);
        expect(result[1].declaration == "struct Derived : Point");
        expect(result[2].kind == DeclarationKind::class_type && result[2].name == "Empty");
        expect(result[2].declaration == "class Empty");
        expect(result[3].kind == DeclarationKind::union_type);
        expect(result[3].declaration == "union U");
        expect(result[4].kind == DeclarationKind::enum_type);
        expect(result[4].declaration == "enum class Color");
        expect(to_string(result[4].kind) == "enum");
        expect(result[5].kind == DeclarationKind::enum_type);
        expect(result[5].declaration == "enum Legacy");
    };

    "using alias"_test = [] {
        const auto result = exported_declarations("export module m;\nexport using Name = std::vector<int>;\n");
        expect(fatal(result.size() == 1u));
        expect(result[0].kind == DeclarationKind::alias);
        expect(result[0].name == "Name");
        expect(result[0].declaration == "using Name");
        expect(to_string(result[0].kind) == "alias");
    };

    "concept"_test = [] {
        const auto result = exported_declarations(
            "export module m;\n"
            "export template <typename T> concept Addable = requires(T a, T b) { a + b; };\n");
        expect(fatal(result.size() == 1u));
        expect(result[0].kind == DeclarationKind::concept_);
        expect(result[0].name == "Addable");
        expect(result[0].declaration == "template <typename T> concept Addable");
        expect(to_string(result[0].kind) == "concept");
    };

    "variable"_test = [] {
        const auto result = exported_declarations("export module m;\nexport inline constexpr int limit = 3;\n");
        expect(fatal(result.size() == 1u));
        expect(result[0].kind == DeclarationKind::variable);
        expect(result[0].name == "limit");
        expect(result[0].qualifiedName == result[0].name);   // equals name at global scope
        expect(result[0].declaration == "inline constexpr int limit");
        expect(to_string(result[0].kind) == "variable");
    };

    "multiple declarators report only the first (known gap)"_test = [] {
        const auto result = exported_declarations("export module m;\nexport int a, b;\n");
        expect(fatal(result.size() == 1u));
        expect(result[0].name == "a");
        expect(result[0].declaration == "int a");
    };

    "bodies and initializers are skipped, not scanned for further exports"_test = [] {
        const auto result = exported_declarations(
            "export module m;\n"
            "export int compute() {\n"
            "    export int nested_would_be_illegal_anyway;\n"
            "    return 1;\n"
            "}\n"
            "export std::vector<int> data { 1, 2, 3 };\n");
        expect(fatal(result.size() == 2u));
        expect(result[0].kind == DeclarationKind::function && result[0].name == "compute");
        expect(result[0].declaration == "int compute()");
        expect(result[1].kind == DeclarationKind::variable && result[1].name == "data");
        expect(result[1].declaration == "std::vector<int> data");
    };

    "conditional declarations are marked, unconditional ones are not"_test = [] {
        const auto result = exported_declarations(
            "export module m;\n"
            "#ifdef FEATURE\n"
            "export int feature_flag();\n"
            "#endif\n"
            "export int always();\n");
        expect(fatal(result.size() == 2u));
        expect(result[0].name == "feature_flag");
        expect(result[0].conditional);
        expect(result[1].name == "always");
        expect(!result[1].conditional);
    };

    "documentation from // and /// line comments, stripped and chained"_test = [] {
        const auto result = exported_declarations(
            "export module m;\n"
            "// Greets somebody by name.\n"
            "export std::string greet(std::string_view name);\n"
            "\n"
            "/// Counts calls so far.\n"
            "/// Thread-unsafe.\n"
            "export int count();\n"
            "int not_documented();\n");
        expect(fatal(result.size() == 2u));
        expect(result[0].documentation == "Greets somebody by name.");
        expect(result[1].documentation == "Counts calls so far.\nThread-unsafe.");
    };

    "documentation from a /* */ block, markers and common indentation stripped"_test = [] {
        const auto result = exported_declarations(
            "export module m;\n"
            "export namespace hello {\n"
            "/*\n"
            "   Counts calls.\n"
            "   Extra detail line.\n"
            "*/\n"
            "int count();\n"
            "}\n");
        expect(fatal(result.size() == 2u));
        expect(result[0].documentation.empty());   // no comment directly above "export namespace hello"
        expect(result[1].name == "count");
        expect(result[1].documentation == "Counts calls.\nExtra detail line.");
    };

    "no documentation across a blank line, or from a trailing same-line comment"_test = [] {
        const auto blankLine = exported_declarations("export module m;\n// Not adjacent.\n\nexport int f();\n");
        expect(fatal(blankLine.size() == 1u));
        expect(blankLine[0].documentation.empty());
        const auto trailing = exported_declarations("export module m;\nexport int g(); // trailing, not documentation\n");
        expect(fatal(trailing.size() == 1u));
        expect(trailing[0].documentation.empty());
    };

    "UTF-16 ranges after a non-ASCII identifier"_test = [] {
        const auto result = exported_declarations("export module m;\nexport int caf\xC3\xA9();\n");
        expect(fatal(result.size() == 1u));
        expect(result[0].name == "caf\xC3\xA9");
        expect(result[0].nameRange.start == Position { 1, 11 });
        expect(result[0].nameRange.end == Position { 1, 15 });   // "café" is 4 UTF-16 units
    };

    "to_string covers every kind"_test = [] {
        expect(to_string(DeclarationKind::function) == "function");
        expect(to_string(DeclarationKind::class_type) == "class");
        expect(to_string(DeclarationKind::struct_type) == "struct");
        expect(to_string(DeclarationKind::union_type) == "union");
        expect(to_string(DeclarationKind::enum_type) == "enum");
        expect(to_string(DeclarationKind::concept_) == "concept");
        expect(to_string(DeclarationKind::alias) == "alias");
        expect(to_string(DeclarationKind::variable) == "variable");
        expect(to_string(DeclarationKind::namespace_) == "namespace");
        expect(to_string(DeclarationKind::reexport) == "reexport");
        expect(to_string(DeclarationKind::other) == "other");
    };

    // Realistic inputs copied from the conformance fixtures.

    "fixture: mcpp-split greet.cppm"_test = [] {
        const auto result = exported_declarations(
            "export module hello.greet;\n"
            "\n"
            "import std;\n"
            "export import :format;\n"
            "\n"
            "export namespace hello {\n"
            "std::string greet(std::string_view who);\n"
            "int count();\n"
            "}\n");
        expect(fatal(result.size() == 4u));
        expect(result[0].kind == DeclarationKind::reexport);
        expect(result[0].name == ":format");
        expect(result[0].nameRange == Range { Position { 3, 14 }, Position { 3, 21 } });
        expect(result[1].kind == DeclarationKind::namespace_);
        expect(result[1].name == "hello");
        expect(result[1].nameRange == Range { Position { 5, 17 }, Position { 5, 22 } });
        expect(result[2].kind == DeclarationKind::function);
        expect(result[2].name == "greet");
        expect(result[2].qualifiedName == "hello::greet");
        expect(result[2].declaration == "std::string greet(std::string_view who)");
        expect(result[2].nameRange == Range { Position { 6, 12 }, Position { 6, 17 } });
        expect(result[3].kind == DeclarationKind::function);
        expect(result[3].name == "count");
        expect(result[3].qualifiedName == "hello::count");
        expect(result[3].declaration == "int count()");
        expect(result[3].nameRange == Range { Position { 7, 4 }, Position { 7, 9 } });
    };

    "fixture: mcpp-split format.cppm"_test = [] {
        const auto result = exported_declarations(
            "export module hello.greet:format;\n"
            "\n"
            "import std;\n"
            "\n"
            "export namespace hello {\n"
            "std::string decorate(std::string_view text);\n"
            "}\n");
        expect(fatal(result.size() == 2u));
        expect(result[0].kind == DeclarationKind::namespace_ && result[0].name == "hello");
        expect(result[1].kind == DeclarationKind::function);
        expect(result[1].name == "decorate");
        expect(result[1].qualifiedName == "hello::decorate");
        expect(result[1].declaration == "std::string decorate(std::string_view text)");
    };

    "fixture: mcpp-split counter.cppm is a partition implementation unit, nothing exported"_test = [] {
        const auto result = exported_declarations(
            "module hello.greet:counter;\n"
            "\n"
            "namespace hello::detail {\n"
            "int& calls() {\n"
            "    static int value = 0;\n"
            "    return value;\n"
            "}\n"
            "\n"
            "void bump() {\n"
            "    ++calls();\n"
            "}\n"
            "\n"
            "int value() {\n"
            "    return calls();\n"
            "}\n"
            "}\n");
        expect(result.empty());
    };

    "fixture: mcpp-all-cppm calc.cppm re-exports its parts"_test = [] {
        const auto result = exported_declarations(
            "export module calc;\n"
            "\n"
            "export import :types;\n"
            "export import :geometry;\n"
            "export import calc.text;\n");
        expect(fatal(result.size() == 3u));
        expect(result[0].kind == DeclarationKind::reexport && result[0].name == ":types");
        expect(result[0].nameRange == Range { Position { 2, 14 }, Position { 2, 20 } });
        expect(result[1].kind == DeclarationKind::reexport && result[1].name == ":geometry");
        expect(result[2].kind == DeclarationKind::reexport && result[2].name == "calc.text");
        expect(result[2].declaration == "import calc.text");
        expect(result[2].nameRange == Range { Position { 4, 14 }, Position { 4, 23 } });
    };

    "fixture: mcpp-all-cppm geometry.cppm"_test = [] {
        const auto result = exported_declarations(
            "export module calc:geometry;\n"
            "\n"
            "import std;\n"
            "import :types;\n"
            "\n"
            "export namespace calc {\n"
            "double length(const vec2& v) {\n"
            "    return std::sqrt(v.x * v.x + v.y * v.y);\n"
            "}\n"
            "}\n");
        expect(fatal(result.size() == 2u));
        expect(result[0].kind == DeclarationKind::namespace_ && result[0].name == "calc");
        expect(result[1].kind == DeclarationKind::function);
        expect(result[1].name == "length");
        expect(result[1].qualifiedName == "calc::length");
        expect(result[1].declaration == "double length(const vec2& v)");
    };

    "fixture: mcpp-all-cppm types.cppm"_test = [] {
        const auto result = exported_declarations(
            "export module calc:types;\n"
            "\n"
            "export namespace calc {\n"
            "struct vec2 {\n"
            "    double x;\n"
            "    double y;\n"
            "};\n"
            "}\n");
        expect(fatal(result.size() == 2u));
        expect(result[0].kind == DeclarationKind::namespace_ && result[0].name == "calc");
        expect(result[1].kind == DeclarationKind::struct_type);
        expect(result[1].name == "vec2");
        expect(result[1].qualifiedName == "calc::vec2");
        expect(result[1].declaration == "struct vec2");
    };

    "fixture: mcpp-all-cppm text.cppm"_test = [] {
        const auto result = exported_declarations(
            "export module calc.text;\n"
            "\n"
            "import std;\n"
            "\n"
            "export namespace calc {\n"
            "std::string format_pair(double x, double y) {\n"
            "    return std::format(\"({}, {})\", x, y);\n"
            "}\n"
            "}\n");
        expect(fatal(result.size() == 2u));
        expect(result[0].kind == DeclarationKind::namespace_ && result[0].name == "calc");
        expect(result[1].kind == DeclarationKind::function);
        expect(result[1].name == "format_pair");
        expect(result[1].qualifiedName == "calc::format_pair");
        expect(result[1].declaration == "std::string format_pair(double x, double y)");
    };

    return report();
}
