// Code templates (S3-6.4): `auto main() -> int { }` and `int main() { }` where a declaration begins
// at file scope and what was typed of it is a prefix of `main`, alone or after its type; none in a
// file that declares `main` or in a named module unit; a snippet for a client that declared
// snippets and the same text, plain, for any other; merged with the core engine's answer after it.
import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.base.text;
import mcppls.project.scan;
import mcppls.engine.native.templates;
import mcppls.orchestrator.completion;

using Json = nlohmann::json;
using mcppls::base::Position;
namespace completion = mcppls::orchestrator::completion;
namespace idx = mcppls::index;

namespace {

constexpr std::string_view AUTO_MAIN { "auto main() -> int { }" };
constexpr std::string_view INT_MAIN { "int main() { }" };

std::vector<std::string> labels_of(const Json& items) {
    std::vector<std::string> labels;
    for (const auto& item : items) labels.push_back(item.value("label", std::string {}));
    return labels;
}

Position end_of(std::string_view text) {
    const auto lines = mcppls::base::split_lines(text);
    const int line { static_cast<int>(lines.empty() ? 0 : lines.size() - 1) };
    const int character { static_cast<int>(lines.empty() ? 0 : mcppls::base::utf16_length(lines.back())) };
    return Position { line, character };
}

// The templates offered with the cursor at the end of `text`, read with its scan unless `scanned` is false.
Json templates_at(std::string_view text, Position position, bool scanned = true, bool snippets = false) {
    const auto scan = mcppls::project::scan_source(text);
    return idx::template_completion(text, position, scanned ? &scan : nullptr, idx::TemplateOptions { .snippets = snippets });
}

std::vector<std::string> at_end(std::string_view text, bool scanned = true) { return labels_of(templates_at(text, end_of(text), scanned)); }

// Functions, not constants: nothing is allocated before main (the platform runtime is set up there).
std::vector<std::string> both() { return { std::string { AUTO_MAIN }, std::string { INT_MAIN } }; }
std::vector<std::string> only(std::string_view label) { return { std::string { label } }; }

} // namespace

int main() {
    using namespace mcppls::testing;

    "the templates of main where a declaration begins, filtered by what was typed"_test = [] {
        for (const std::string_view typed : { "m", "ma", "mai", "main", "  ma", "\tmain" }) {
            expect(at_end(typed) == both()) << typed;
        }
        for (const std::string_view typed : { "int m", "int ma", "int main", "int\tma", "int   main" }) {
            expect(at_end(typed) == only(INT_MAIN)) << typed;
        }
        for (const std::string_view typed : { "auto m", "auto ma", "auto main", "auto\tmai" }) {
            expect(at_end(typed) == only(AUTO_MAIN)) << typed;
        }
        // Nothing for an empty prefix, another word, or anything else before the name.
        for (const std::string_view typed : { "", "  ", "int ", "auto ", "a", "au", "auto", "i", "in", "int", "mains", "x", "maint",
                                              "static ma", "long ma", "export int ma", "int x ma", "int main(", "int *ma", "::ma", "n::ma" }) {
            expect(at_end(typed).empty()) << typed;
        }
        // After whatever ends a declaration, or nothing that is code.
        expect(at_end("import std;\n\nma") == both());
        expect(at_end("#include <cstdio>\n#include \"a.h\"\nma") == both());
        expect(at_end("// a comment\n/* another\n   one */\nma") == both());
        expect(at_end("namespace n {\nint f();\n}\nma") == both());
        expect(at_end("void f() {\n}\nint ma") == only(INT_MAIN));
        expect(at_end("const char* s = \"{\";\nchar c = '{';\nint n = 1'000;\nma") == both()) << "a literal's brace is not the file's";
        expect(at_end("auto r = R\"x({ \")x\";\nma") == both());
        expect(at_end("#define OPEN {\nma") == both()) << "a macro's brace is not the file's";
        expect(at_end("#if 1\nma") == both()) << "the line after a directive is code";
    };

    "nothing inside braces, comments, literals or preprocessor lines, nor in a declaration begun before the line"_test = [] {
        expect(at_end("int f() {\n    ma").empty()) << "a function's body";
        expect(at_end("int f() {\n    int ma").empty());
        expect(at_end("namespace n {\nma").empty()) << "file scope only";
        expect(at_end("struct S {\n    int ma").empty());
        expect(at_end("/* open\nma").empty());
        expect(at_end("// ma").empty());
        expect(at_end("/* ma").empty());
        expect(at_end("auto s = \"ma").empty());
        expect(at_end("char c = 'm").empty());
        expect(at_end("auto r = R\"x(\nma").empty()) << "inside a raw string literal";
        expect(at_end("auto r = R\"x(\n)y\"\nma").empty()) << "another delimiter does not end it";
        expect(at_end("#define ENTRY \\\nma").empty()) << "a preprocessor line's continuation";
        expect(at_end("#define ENTRY \\\r\nma").empty());
        expect(at_end("#include <ma").empty());
        expect(at_end("int x =\nma").empty()) << "the declaration began on the line before";
        expect(at_end("template <class T>\nint ma").empty());
        expect(at_end("static\nma").empty());
    };

    "the cursor ends what was typed, with nothing after it on the line"_test = [] {
        expect(labels_of(templates_at("ma", Position { 0, 2 })) == both());
        expect(labels_of(templates_at("ma   ", Position { 0, 2 })) == both()) << "blanks after it";
        expect(labels_of(templates_at("ma\r\n", Position { 0, 2 })) == both());
        expect(labels_of(templates_at("ma\nint x;\n", Position { 0, 2 })) == both()) << "on its own line";
        expect(labels_of(templates_at("mab", Position { 0, 2 })).empty()) << "inside a word";
        expect(labels_of(templates_at("int main() {}", Position { 0, 8 })).empty()) << "the rest of a declaration after it";
        expect(labels_of(templates_at("ma // entry", Position { 0, 2 })).empty());
        expect(templates_at("ma", Position { 4, 0 }).empty()) << "a position outside the text";
    };

    "none where main is declared already, the line being typed aside"_test = [] {
        expect(at_end("int main() { return 0; }\nma").empty());
        expect(at_end("auto main() -> int {\n}\n\nint ma").empty());
        expect(at_end("int main(int argc, char** argv);\nauto m").empty()) << "a declaration is enough";
        expect(labels_of(templates_at("ma\nint main() {}", Position { 0, 2 })).empty()) << "after the cursor too";
        expect(at_end("namespace n {\nint f();\n}\nint main()\n{\n}\nma").empty());
        expect(at_end("void maintain();\nint mainly = 0;\nma") == both()) << "other names";
        expect(at_end("void f() { main(); }\nma") == both()) << "a use is no declaration";
        expect(at_end("int main") == only(INT_MAIN)) << "the name being typed is not declared yet";
    };

    "none in a named module unit, before its declaration or after it"_test = [] {
        expect(at_end("export module app;\nma").empty()) << "main would be attached to the module";
        expect(at_end("export module app;\nimport std;\nint ma").empty());
        expect(at_end("module app;\nma").empty()) << "an implementation unit";
        expect(at_end("export module app:part;\nma").empty()) << "a partition";
        const std::string fragment { "module;\n#include <cstdio>\nma\nexport module app;\n" };
        expect(labels_of(templates_at(fragment, Position { 2, 2 })).empty()) << "the global module fragment holds directives only";
        expect(at_end("import std;\nimport app;\nma") == both()) << "a file that imports modules is no module unit";
        expect(at_end("export module app;\nma", false) == both()) << "the scan tells, not the text";
    };

    "a snippet with the cursor in the body where the client declared snippets, the same text plain otherwise"_test = [] {
        const std::string text { "import std;\n  int ma" };
        const Json snippet = templates_at(text, Position { 1, 8 }, true, true);
        expect(fatal(snippet.is_array() && snippet.size() == 1u)) << snippet.dump();
        const Json& item = snippet[0];
        expect(item["label"] == std::string { INT_MAIN } && item["kind"] == 15 && item["insertTextFormat"] == 2) << item.dump();
        expect(item["detail"].get<std::string>().find("template") != std::string::npos);
        expect(item["filterText"] == "int main") << "a client filters by the text from the edit's start";
        expect(item["textEdit"]["newText"] == "int main() {\n\t$0\n}");
        expect(item["textEdit"]["range"]["start"] == Json::parse(R"({"line": 1, "character": 2})")) << "from the declaration's start";
        expect(item["textEdit"]["range"]["end"] == Json::parse(R"({"line": 1, "character": 8})")) << "to the cursor";
        expect(item["sortText"].get<std::string>().starts_with("~"));

        const Json plain = templates_at(text, Position { 1, 8 });
        expect(fatal(plain.is_array() && plain.size() == 1u)) << plain.dump();
        expect(plain[0]["insertTextFormat"] == 1 && plain[0]["kind"] == 15);
        expect(plain[0]["textEdit"]["newText"] == "int main() { }") << "the template as written, and no $0";
        expect(plain[0]["textEdit"]["range"] == item["textEdit"]["range"] && plain[0]["filterText"] == item["filterText"]);

        const Json bare = templates_at("ma", Position { 0, 2 }, true, true);
        expect(fatal(bare.size() == 2u));
        for (const auto& entry : bare) expect(entry["filterText"] == "main") << entry.dump();
        expect(bare[0]["textEdit"]["newText"] == "auto main() -> int {\n\t$0\n}");
        const Json spaced = templates_at("auto\t ma", Position { 0, 8 }, true, true);
        expect(fatal(spaced.size() == 1u));
        expect(spaced[0]["filterText"] == "auto\t main" && spaced[0]["textEdit"]["range"]["start"]["character"] == 0);
    };

    "the templates join the core engine's answer and sort after its items"_test = [] {
        const Json templates = templates_at("ma", Position { 0, 2 }, true, true);
        const Json list = Json::parse(R"json({"isIncomplete": false, "items": [
            {"label": " malloc(size_t size)", "sortText": "40b4d2c5malloc"},
            {"label": " max_value", "sortText": "fffffffemax_value"}]})json");
        const Json merged = completion::merge(list, templates);
        expect(merged["isIncomplete"] == false);
        expect(labels_of(merged["items"]) == std::vector<std::string> { " malloc(size_t size)", " max_value", std::string { AUTO_MAIN }, std::string { INT_MAIN } });
        for (const auto& own : templates) {
            for (const auto& engine : list["items"]) expect(own["sortText"].get<std::string>() > engine["sortText"].get<std::string>()) << own.dump();
        }
        const Json alone = completion::keywords_only(templates);
        expect(alone["isIncomplete"] == true && labels_of(alone["items"]) == both()) << "on their own while the engine has not answered";
    };
}
