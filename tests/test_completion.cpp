// Completion routing (fix plan 2026-09-26 F9, F15, D4, D5): the space-trigger gate and who is told
// about the space, the module-syntax keywords and where each one fits, their merge with the core
// engine's answer, and the module names import completion keeps between edits.
import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.base.text;
import mcppls.project.scan;
import mcppls.engine.native.index;
import mcppls.engine.native.keywords;
import mcppls.orchestrator.completion;
import mcppls.orchestrator.routing;

using Json = nlohmann::json;
using mcppls::base::Position;
namespace completion = mcppls::orchestrator::completion;
namespace idx = mcppls::index;

namespace {

std::vector<std::string> labels_of(const Json& result) {
    std::vector<std::string> labels;
    const Json items = result.is_object() ? result.value("items", Json::array()) : result;
    if (!items.is_array()) return labels;
    for (const auto& item : items) labels.push_back(item.value("label", std::string {}));
    return labels;
}

bool has(const std::vector<std::string>& labels, std::string_view label) { return std::ranges::find(labels, label) != labels.end(); }

// The keywords offered with the cursor at the end of `text`.
std::vector<std::string> keywords_at_end(std::string_view text, bool scanned = true) {
    const auto lines = mcppls::base::split_lines(text);
    const int line { static_cast<int>(lines.empty() ? 0 : lines.size() - 1) };
    const int character { static_cast<int>(lines.empty() ? 0 : lines.back().size()) };
    const auto scan = mcppls::project::scan_source(text);
    return labels_of(idx::keyword_completion(text, Position { line, character }, scanned ? &scan : nullptr));
}

} // namespace

int main() {
    using namespace mcppls::testing;

    "the space gate passes an import directive's keyword and one blank, and nothing else"_test = [] {
        for (const std::string_view passes : { "import ", "export import ", "  import ", "\timport\t", "export  import ", "\texport\timport " }) {
            expect(completion::is_import_line_prefix(passes)) << passes;
        }
        for (const std::string_view fails : { "import  ", "import", "import a", "import a ", "int x = ", "", " ", "exportimport ",
                                              "export ", "export module ", "module ", "importer ", "// import ", "x import " }) {
            expect(!completion::is_import_line_prefix(fails)) << fails;
        }
    };

    "the line before the cursor, by LSP position"_test = [] {
        const std::string text { "int a;\nexport import \nb" };
        expect(completion::line_prefix(text, Position { 1, 14 }).value_or("?") == "export import ");
        expect(completion::line_prefix(text, Position { 1, 7 }).value_or("?") == "export ");
        expect(completion::line_prefix(text, Position { 0, 0 }).value_or("?").empty());
        expect(completion::line_prefix(text, Position { 2, 1 }).value_or("?") == "b");
        expect(!completion::line_prefix(text, Position { 9, 0 }).has_value());
    };

    "a space-triggered request is told from every other"_test = [] {
        expect(completion::is_space_trigger(Json::parse(R"({"context": {"triggerKind": 2, "triggerCharacter": " "}})")));
        expect(!completion::is_space_trigger(Json::parse(R"({"context": {"triggerKind": 2, "triggerCharacter": "."}})")));
        expect(!completion::is_space_trigger(Json::parse(R"({"context": {"triggerKind": 1}})")));
        expect(!completion::is_space_trigger(Json::parse(R"({"position": {"line": 0, "character": 0}})")));
    };

    "the space is advertised to VS Code and its forks, or to a client that asks"_test = [] {
        const auto client = [](std::string_view name, const Json& options = nullptr) {
            Json params { { "clientInfo", Json { { "name", std::string { name } }, { "version", "1.100.0" } } } };
            if (!options.is_null()) params["initializationOptions"] = options;
            return params;
        };
        for (const std::string_view name : { "Visual Studio Code", "Visual Studio Code - Insiders", "Code - OSS", "VSCodium", "Cursor", "Windsurf", "Trae" }) {
            expect(completion::vscode_like(client(name))) << name;
            expect(completion::space_trigger_wanted(client(name))) << name;
        }
        for (const std::string_view name : { "Neovim", "Zed", "helix", "CLion", "mcppls-conformance", "" }) {
            expect(!completion::space_trigger_wanted(client(name))) << name;
        }
        expect(!completion::space_trigger_wanted(Json::object())) << "no clientInfo";
        expect(completion::space_trigger_wanted(client("Neovim", Json::parse(R"({"completion": {"triggerOnSpace": true}})"))));
        expect(!completion::space_trigger_wanted(client("Visual Studio Code", Json::parse(R"({"completion": {"triggerOnSpace": false}})"))));
        expect(completion::space_trigger_wanted(client("Visual Studio Code", Json::parse(R"({"completion": {}})"))));

        Json clangd = Json::parse(R"({"completionProvider": {"triggerCharacters": [".", "<", ">", ":", "\"", "/", "*"], "resolveProvider": false}})");
        completion::add_space_trigger(clangd);
        completion::add_space_trigger(clangd);
        const Json& triggers = clangd["completionProvider"]["triggerCharacters"];
        expect(triggers.size() == 8u && triggers.back() == " ") << triggers.dump();
        expect(clangd["completionProvider"]["resolveProvider"] == false);
        Json own = mcppls::orchestrator::merge_capabilities(Json::object());
        completion::add_space_trigger(own);
        expect(own["completionProvider"]["triggerCharacters"] == Json::parse(R"([".", ":", " "])"));
    };

    "module keywords where a declaration can begin, filtered by what was typed"_test = [] {
        // A plain file: import only; the module declarations while it has none; module; while nothing precedes.
        const auto first = keywords_at_end("i");
        expect(first == std::vector<std::string> { "import" }) << first.size();
        const auto m = keywords_at_end("m");
        expect(has(m, "module;") && has(m, "module") && !has(m, "module :private;"));
        const auto e = keywords_at_end("e");
        expect(has(e, "export module") && !has(e, "export import")) << "no interface unit, nothing to export";
        expect(keywords_at_end("#include <x>\nm") == std::vector<std::string> { "module" }) << "module; comes first or not at all";
        expect(has(keywords_at_end("// a comment\n/* another */\nm"), "module;"));
        // Everything, invoked on an empty line of an empty file.
        expect(keywords_at_end("").size() == 4u);   // import, module;, export module, module

        // An interface unit: export import, and the private fragment once.
        const std::string interface { "export module hello;\nimport std;\n" };
        const auto afterDeclaration = keywords_at_end(interface + "e");
        expect(afterDeclaration == std::vector<std::string> { "export import" }) << "the declaration is there already";
        expect(keywords_at_end(interface + "export i") == std::vector<std::string> { "export import" });
        expect(keywords_at_end(interface + "export  i") == std::vector<std::string> { "export import" });
        expect(keywords_at_end(interface + "export") == std::vector<std::string> { "export import" });
        expect(keywords_at_end(interface + "mod") == std::vector<std::string> { "module :private;" });
        expect(keywords_at_end(interface + "module :private;\nint x;\nmod").empty());
        expect(keywords_at_end("export module hello:part;\nmod").empty()) << "a partition has no private fragment";
        // An implementation unit exports nothing.
        expect(keywords_at_end("module hello;\ne").empty());
        expect(keywords_at_end("module hello;\ni") == std::vector<std::string> { "import" });
    };

    "no keywords inside braces, comments, literals, or the middle of a word"_test = [] {
        const Json none = idx::keyword_completion("int f() {\n  i", Position { 1, 3 }, nullptr);
        expect(none.is_null()) << none.dump();
        expect(idx::keyword_completion("namespace n {\ni", Position { 1, 1 }, nullptr).is_null());
        expect(!idx::keyword_completion("namespace n {\n}\ni", Position { 2, 1 }, nullptr).is_null());
        expect(idx::keyword_completion("/* open\ni", Position { 1, 1 }, nullptr).is_null());
        expect(!idx::keyword_completion("const char* s = \"{\";\nchar c = '{';\n// {\ni", Position { 3, 1 }, nullptr).is_null());
        expect(!idx::keyword_completion("auto r = R\"x({ \")x\";\ni", Position { 1, 1 }, nullptr).is_null());
        expect(!idx::keyword_completion("#define OPEN {\ni", Position { 1, 1 }, nullptr).is_null()) << "a macro's brace is not the file's";
        expect(!idx::keyword_completion("int n = 1'000;\ni", Position { 1, 1 }, nullptr).is_null());
        expect(idx::keyword_completion("import", Position { 0, 3 }, nullptr).is_null()) << "the cursor inside a word";
        expect(idx::keyword_completion("x = i", Position { 0, 5 }, nullptr).is_null());
        expect(idx::keyword_completion("import ", Position { 0, 7 }, nullptr).is_null()) << "the module names' place, not the keywords'";
        expect(idx::keyword_completion("export module ", Position { 0, 14 }, nullptr).is_null()) << "D5: no name is suggested";
        expect(idx::keyword_completion("q", Position { 0, 1 }, nullptr).is_null()) << "no keyword starts with it";
    };

    "a keyword replaces what was typed, and opens the module list where the client can"_test = [] {
        const std::string text { "export module a;\n  export im" };
        const auto scan = mcppls::project::scan_source(text);
        const Json plain = idx::keyword_completion(text, Position { 1, 11 }, &scan);
        expect(fatal(plain.is_array() && plain.size() == 1u)) << plain.dump();
        const Json& item = plain[0];
        expect(item["label"] == "export import" && item["kind"] == 14 && item["filterText"] == "export import");
        expect(item["textEdit"]["newText"] == "export import ");
        expect(item["textEdit"]["range"]["start"] == Json::parse(R"({"line": 1, "character": 2})"));
        expect(item["textEdit"]["range"]["end"] == Json::parse(R"({"line": 1, "character": 11})"));
        expect(!item.contains("command"));
        const Json vscode = idx::keyword_completion(text, Position { 1, 11 }, &scan, idx::KeywordOptions { .suggestModulesAfterImport = true });
        expect(vscode[0]["command"]["command"] == "editor.action.triggerSuggest");
        const Json module = idx::keyword_completion("m", Position { 0, 1 }, nullptr, idx::KeywordOptions { .suggestModulesAfterImport = true });
        for (const auto& entry : module) expect(!entry.contains("command")) << entry.dump();
    };

    "keywords merge with the core engine's answer without duplicates"_test = [] {
        const Json keywords = idx::keyword_completion("i", Position { 0, 1 }, nullptr);
        const Json list = Json::parse(R"({"isIncomplete": true, "items": [{"label": " int"}, {"label": "import"}]})");
        const Json merged = completion::merge(list, keywords);
        expect(merged["isIncomplete"] == true);
        expect(labels_of(merged) == std::vector<std::string> { " int", "import" }) << "clangd's own `import` item is kept, not doubled";
        const Json array = completion::merge(Json::parse(R"([{"label": "if"}])"), keywords);
        expect(array["isIncomplete"] == false && labels_of(array) == std::vector<std::string> { "if", "import" });
        const Json withDefaults = completion::merge(Json::parse(R"({"isIncomplete": false, "itemDefaults": {"insertTextFormat": 2}, "items": []})"), keywords);
        expect(withDefaults.contains("itemDefaults") && labels_of(withDefaults) == std::vector<std::string> { "import" });
        expect(completion::merge(nullptr, Json::array()).is_null());
        expect(completion::merge(list, Json(nullptr)) == list);
        const Json alone = completion::keywords_only(keywords);
        expect(alone["isIncomplete"] == true && labels_of(alone) == std::vector<std::string> { "import" });
        expect(completion::empty_list() == Json::parse(R"({"isIncomplete": false, "items": []})"));
    };

    "module names for completion are kept until a declaration changes"_test = [] {
        idx::ModuleIndex index;
        index.update("/p/a.cppm", "export module a;\n");
        index.update("/p/b.cppm", "export module b;\nexport import a;\n");
        index.update("/p/main.cpp", "import a;\n");
        const auto names = [&] { return labels_of(index.completion("/p/x.cpp", "import ", Position { 0, 7 })); };
        expect(names() == std::vector<std::string> { "a", "b" });
        const auto generation = index.structure_generation();
        // Edits that leave every declaration as it was: the same list, not rebuilt.
        index.update("/p/main.cpp", "import a;\nimport b;\nint main() {}\n");
        index.update("/p/b.cppm", "export module b;\nexport import a;\nexport int f();\n");
        index.update("/p/other.cpp", "int g();\n");
        expect(index.structure_generation() == generation);
        expect(names() == std::vector<std::string> { "a", "b" });
        // A new module, a role that changes, a unit removed: rebuilt.
        index.update("/p/c.cppm", "export module c;\n");
        expect(index.structure_generation() != generation);
        expect(names() == std::vector<std::string> { "a", "b", "c" });
        const auto withC = index.structure_generation();
        index.update("/p/c.cppm", "module c;\n");
        expect(index.structure_generation() != withC);
        expect(names() == std::vector<std::string> { "a", "b" }) << "an implementation unit provides nothing to import";
        index.remove("/p/b.cppm");
        expect(names() == std::vector<std::string> { "a" });
        const Json detail = index.completion("/p/x.cpp", "import ", Position { 0, 7 });
        expect(detail["items"][0]["detail"] == "primary module interface unit") << detail.dump();
    };

    return report();
}
