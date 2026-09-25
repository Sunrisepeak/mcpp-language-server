import std;
import mcppls.testing;
import mcppls.base.text;
import mcppls.spec.database;
import mcppls.project.scan;

using namespace mcppls::project;
using mcppls::base::Position;
using mcppls::base::Range;
using mcppls::spec::Role;

int main() {
    using namespace mcppls::testing;

    "the fixture's primary interface"_test = [] {
        const auto result = scan_source("export module hello.greet;\nexport import :detail;\nimport std;\n\n"
                                        "export namespace hello {\n  std::string greet(std::string_view who) { return detail::prefix() + std::string(who); }\n}\n");
        expect(fatal(result.declaration.has_value()));
        expect(result.declaration->module == "hello.greet");
        expect(result.declaration->isExported);
        expect(result.declaration->nameRange == Range { Position { 0, 14 }, Position { 0, 25 } });
        expect(role_of(result) == Role::module_interface);
        expect(provided_name(result) == "hello.greet");
        expect(fatal(result.imports.size() == 2u));
        expect(result.imports[0].isExported && result.imports[0].partition == "detail" && result.imports[0].module.empty());
        expect(result.imports[0].nameRange == Range { Position { 1, 14 }, Position { 1, 21 } });
        expect(required_names(result) == std::vector<std::string> { "hello.greet:detail", "std" });
        expect(!result.uncertain);
    };

    "a module name is dotted identifiers with at most one partition"_test = [] {
        for (const std::string_view name : { "std", "hello.greet", "a.b:c", "a:b.c", "_x.y2", "m\u00e9.a" }) expect(is_module_name(name)) << name;
        for (const std::string_view name : { "", "hello.", ".x", "a..b", "a:b:c", ":p", "a:", "1a", "a.1b", "a-b", "a b" }) expect(!is_module_name(name)) << name;
    };

    "partitions and implementation units"_test = [] {
        expect(role_of(scan_source("export module a.b:c;")) == Role::module_partition_interface);
        expect(provided_name(scan_source("export module a.b:c;")) == "a.b:c");
        const auto partitionImpl = scan_source("module a.b:impl;\nimport :c;");
        expect(role_of(partitionImpl) == Role::module_partition_implementation);
        expect(provided_name(partitionImpl) == "a.b:impl");
        expect(required_names(partitionImpl) == std::vector<std::string> { "a.b:c" });
        const auto implementation = scan_source("module a.b;\nimport std;\nvoid f() {}\n");
        expect(role_of(implementation) == Role::module_implementation);
        expect(provided_name(implementation).empty());
        expect(required_names(implementation) == std::vector<std::string> { "a.b", "std" });
    };

    "non-module units import"_test = [] {
        const auto result = scan_source("import std;\nimport hello.greet;\n\nint main(int argc, char* argv[]) {\n    std::println(\"{}\", hello::greet(\"mcpp\"));\n}\n");
        expect(role_of(result) == Role::non_module);
        expect(!result.declaration.has_value());
        expect(fatal(result.imports.size() == 2u));
        expect(result.imports[1].module == "hello.greet");
        expect(result.imports[1].nameRange == Range { Position { 1, 7 }, Position { 1, 18 } });
    };

    "comments, strings and raw strings hide keywords"_test = [] {
        const auto result = scan_source(R"src(// import commented;
/* export module fake;
   import also.fake; */
const char* s = "import in.string;";
const char* r = R"x(
import in.raw;
)x";
auto c = u8R"(
module in.raw.two;
)";
import real;
)src");
        expect(!result.declaration.has_value());
        expect(fatal(result.imports.size() == 1u));
        expect(result.imports[0].module == "real");
        expect(result.imports[0].nameRange.start.line == 10_i);
    };

    "contextual keywords that are not declarations"_test = [] {
        const auto result = scan_source("int module = 3;\nstruct import { int x; };\nvoid f() { module = 4; }\nauto v = obj.\nmodule;\nimport\n  = 2;\n");
        expect(!result.declaration.has_value());
        expect(result.imports.empty());
        const auto braces = scan_source("namespace n {\nimport inside.braces;\n}\n");
        expect(braces.imports.empty());
    };

    "header units"_test = [] {
        const auto result = scan_source("import <vector>;\nexport import \"config.h\";\nimport std;");
        expect(fatal(result.imports.size() == 3u));
        expect(result.imports[0].isHeaderUnit && result.imports[0].header == "<vector>");
        expect(result.imports[1].isHeaderUnit && result.imports[1].isExported);
        expect(required_names(result) == std::vector<std::string> { "std" });
    };

    "conditional declarations are uncertain"_test = [] {
        const auto result = scan_source("#ifdef USE_STD\nimport std;\n#else\n#include <string>\n#endif\nimport always;\n");
        expect(result.uncertain);
        expect(fatal(result.imports.size() == 2u));
        expect(result.imports[0].conditional);
        expect(!result.imports[1].conditional);
        const auto declaration = scan_source("#if 1\nexport module m;\n#endif\n");
        expect(role_of(declaration) == Role::unknown);
    };

    "directives with continuations and odd spacing"_test = [] {
        const auto result = scan_source("#define X \\\n  import not.real;\nexport   module\n  spaced . name : part ;\nimport  other . mod [[deprecated]];\n");
        expect(fatal(result.declaration.has_value()));
        expect(result.declaration->module == "spaced.name");
        expect(result.declaration->partition == "part");
        expect(fatal(result.imports.size() == 1u));
        expect(result.imports[0].module == "other.mod");
    };

    "UTF-16 ranges after non-ASCII text"_test = [] {
        const auto result = scan_source("// \xF0\x9F\x98\x80 emoji\n/*\xC3\xA9*/ import m\xC3\xA9;\n");
        expect(fatal(result.imports.size() == 1u));
        expect(result.imports[0].nameRange.start == Position { 1, 13 });
        expect(result.imports[0].nameRange.end == Position { 1, 15 });
    };

    "digit separators do not open character literals"_test = [] {
        const auto result = scan_source("int n = 1'000'000;\nimport after.number;\n");
        expect(result.imports.size() == 1u);
    };

    "source names"_test = [] {
        expect(is_cxx_source_name("/a/b.cppm") && is_cxx_source_name("x.CPP") && is_cxx_source_name("m.ixx"));
        expect(!is_cxx_source_name("a.h") && !is_cxx_source_name("CMakeLists.txt"));
    };

    // design doc 2026-09-25 K/§7: syntax tokens for the server's native semantic tokens, produced
    // from the text alone -- complete or not -- and used by the native engine's tokenizer.
    const auto text_at = [](std::string_view text, Range range) -> std::string {
        if (range.start.line != range.end.line) return "<crosses lines>";
        const auto lines = mcppls::base::split_lines(text);
        if (static_cast<std::size_t>(range.start.line) >= lines.size()) return "<out of range>";
        const auto line = lines[static_cast<std::size_t>(range.start.line)];
        return std::string { line.substr(static_cast<std::size_t>(range.start.character),
                                         static_cast<std::size_t>(range.end.character - range.start.character)) };
    };

    "syntax tokens: a plain import"_test = [text_at] {
        const std::string text { "import std;\n" };
        const auto tokens = scan_syntax_tokens(text);
        expect(fatal(tokens.size() == 2u));
        expect(tokens[0].kind == SyntaxTokenKind::keyword && text_at(text, tokens[0].range) == "import");
        expect(tokens[1].kind == SyntaxTokenKind::moduleName && text_at(text, tokens[1].range) == "std" && !tokens[1].isDeclaration);
    };

    "syntax tokens: export module with a partition"_test = [text_at] {
        const std::string text { "export module a.b:part;\n" };
        const auto tokens = scan_syntax_tokens(text);
        expect(fatal(tokens.size() == 4u));
        expect(tokens[0].kind == SyntaxTokenKind::keyword && text_at(text, tokens[0].range) == "export");
        expect(tokens[1].kind == SyntaxTokenKind::keyword && text_at(text, tokens[1].range) == "module");
        expect(tokens[2].kind == SyntaxTokenKind::moduleName && text_at(text, tokens[2].range) == "a.b" && tokens[2].isDeclaration);
        expect(tokens[3].kind == SyntaxTokenKind::partitionName && text_at(text, tokens[3].range) == "part" && tokens[3].isDeclaration);
    };

    "syntax tokens: module fragments with no name"_test = [] {
        expect(scan_syntax_tokens("module;\n").size() == 1u);   // just the keyword
        const auto privateFragment = scan_syntax_tokens("module :private;\n");
        expect(fatal(privateFragment.size() == 1u));
        expect(privateFragment[0].kind == SyntaxTokenKind::keyword);
    };

    "syntax tokens: a partition-only import"_test = [text_at] {
        const std::string text { "import :part;\n" };
        const auto tokens = scan_syntax_tokens(text);
        expect(fatal(tokens.size() == 2u));
        expect(tokens[0].kind == SyntaxTokenKind::keyword && text_at(text, tokens[0].range) == "import");
        expect(tokens[1].kind == SyntaxTokenKind::partitionName && text_at(text, tokens[1].range) == "part" && !tokens[1].isDeclaration);
    };

    "syntax tokens: export import (a re-export) and header imports"_test = [text_at] {
        const std::string text { "export import x.y;\nimport <vector>;\nimport \"config.h\";\n" };
        const auto tokens = scan_syntax_tokens(text);
        expect(fatal(tokens.size() == 5u));
        expect(tokens[0].kind == SyntaxTokenKind::keyword && text_at(text, tokens[0].range) == "export");
        expect(tokens[1].kind == SyntaxTokenKind::keyword && text_at(text, tokens[1].range) == "import");
        expect(tokens[2].kind == SyntaxTokenKind::moduleName && text_at(text, tokens[2].range) == "x.y");
        expect(tokens[3].kind == SyntaxTokenKind::keyword && text_at(text, tokens[3].range) == "import");   // <vector>
        expect(tokens[4].kind == SyntaxTokenKind::keyword && text_at(text, tokens[4].range) == "import");   // "config.h"
    };

    "syntax tokens: an incomplete import while typing never crashes and never spans lines"_test = [text_at] {
        const std::string trailingDot { "import hello.\n" };
        const auto tokens = scan_syntax_tokens(trailingDot);
        expect(fatal(tokens.size() == 2u));
        expect(tokens[0].kind == SyntaxTokenKind::keyword && text_at(trailingDot, tokens[0].range) == "import");
        expect(tokens[1].kind == SyntaxTokenKind::moduleName && text_at(trailingDot, tokens[1].range) == "hello");

        // The dot's continuation must not reach across the newline into the next statement.
        const std::string nextLine { "import hello.\nint x;\n" };
        const auto acrossLines = scan_syntax_tokens(nextLine);
        expect(fatal(acrossLines.size() == 2u));
        expect(text_at(nextLine, acrossLines[1].range) == "hello");
        for (const auto& token : acrossLines) expect(token.range.start.line == token.range.end.line);

        expect(scan_syntax_tokens("export module a.\n").size() == 3u);       // export, module, "a"
        expect(scan_syntax_tokens("import hello. ;\n").size() == 2u);        // import, "hello"
        expect(scan_syntax_tokens("import\n  = 2;\n").size() == 1u);         // just "import"; "=" is not a name
    };

    "syntax tokens: inside braces are not module syntax"_test = [] {
        expect(scan_syntax_tokens("void f() {\n  import x;\n}\n").empty());
    };

    // Fix plan F2: a UTF-8 byte order mark before the module declaration, as editors on Windows save it.
    "a byte order mark is skipped, and takes no column"_test = [text_at] {
        const std::string mark { "\xEF\xBB\xBF" };
        const auto interface = scan_source(mark + "export module hello.greet;\nimport std;\n");
        expect(fatal(interface.declaration.has_value()));
        expect(interface.declaration->module == "hello.greet" && interface.declaration->isExported);
        expect(interface.declaration->nameRange == Range { Position { 0, 14 }, Position { 0, 25 } });
        expect(provided_name(interface) == "hello.greet");
        const auto fragment = scan_source(mark + "module;\n#include <cstdio>\nexport module m;\n");
        expect(fatal(fragment.declaration.has_value()));
        expect(fragment.declaration->module == "m");
        const auto implementation = scan_source(mark + "module m;\n");
        expect(role_of(implementation) == Role::module_implementation && required_names(implementation) == std::vector<std::string> { "m" });
        const auto importer = scan_source(mark + "import hello.greet;\n");
        expect(fatal(importer.imports.size() == 1u));
        expect(importer.imports[0].nameRange == Range { Position { 0, 7 }, Position { 0, 18 } });
        for (const std::string_view body : { "export module a.b:part;\nexport int f();\n", "module;\nexport module m;\n", "import hello.\n" }) {
            const auto plain = scan_syntax_tokens(body);
            const auto marked = scan_syntax_tokens(mark + std::string { body });
            expect(plain.size() == marked.size()) << body;
            for (std::size_t i { 0 }; i < std::min(plain.size(), marked.size()); ++i) {
                expect(plain[i].kind == marked[i].kind && plain[i].range == marked[i].range) << body << " token " << i;
            }
        }
    };

    return report();
}
