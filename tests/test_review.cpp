// The review's deterministic parts (overall design 7.4, S5 5): line hunks, the semantic diff of a unit,
// uses of a name, and the forms findings take.
import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.spec.query;
import mcppls.ai.review.changes;
import mcppls.ai.review.semantic;
import mcppls.ai.review.impact;
import mcppls.ai.review.report;
import mcppls.ai.verify.toolchains;

using Json = nlohmann::json;
namespace spec = mcppls::spec;
namespace review = mcppls::ai::review;
namespace verify = mcppls::ai::verify;

int main() {
    using namespace mcppls::testing;

    "hunks are the lines that differ"_test = [] {
        expect(review::line_hunks("a\nb\nc\n", "a\nb\nc\n").empty());
        const auto changed = review::line_hunks("a\nb\nc\nd\n", "a\nB\nc\nd\ne\n");
        expect(fatal(changed.size() == 2u)) << changed.size();
        expect(changed[0].baseStart == 2 && changed[0].baseCount == 1 && changed[0].headStart == 2 && changed[0].headCount == 1);
        expect(changed[1].baseStart == 5 && changed[1].baseCount == 0 && changed[1].headStart == 5 && changed[1].headCount == 1);
        const auto removed = review::line_hunks("keep\ndrop\nkeep too\n", "keep\nkeep too\n");
        expect(removed.size() == 1u && removed[0].baseStart == 2 && removed[0].baseCount == 1 && removed[0].headCount == 0);
        const auto created = review::line_hunks("", "one\ntwo\n");
        expect(created.size() == 1u && created[0].headStart == 1 && created[0].headCount == 2);

        review::FileChange file;
        file.base = "a\nb\nc\nd\n";
        file.head = "a\nB\nc\nd\ne\n";
        file.hunks = changed;
        expect(!file.changed_head_line(1) && file.changed_head_line(2) && !file.changed_head_line(3) && file.changed_head_line(5));
    };

    "a semantic diff compares exports, imports and the module a unit provides"_test = [] {
        const std::string base { "export module hello.greet:format;\nimport std;\nexport namespace hello {\nstd::string decorate(std::string_view text);\nint width();\n}\n" };
        const std::string head { "export module hello.greet:format;\nimport std;\nimport missing;\nexport namespace hello {\nint width(int scale);\nvoid added();\n}\n" };
        const auto diff = review::semantic_diff("src/greet/format.cppm", base, head);
        expect(diff.baseModule == "hello.greet:format" && diff.headModule == "hello.greet:format");
        expect(diff.interface_changed());
        expect(fatal(diff.exports.size() == 3u)) << review::to_json(diff).dump();
        expect(diff.exports[0].qualifiedName == "hello::added" && diff.exports[0].kind == review::ExportChangeKind::added);
        expect(diff.exports[1].qualifiedName == "hello::decorate" && diff.exports[1].kind == review::ExportChangeKind::removed);
        expect(diff.exports[1].baseLocation && diff.exports[1].baseLocation->line == 4 && !diff.exports[1].headLocation);
        expect(diff.exports[2].qualifiedName == "hello::width" && diff.exports[2].kind == review::ExportChangeKind::changed);
        expect(diff.exports[2].before == "int width()" && diff.exports[2].after == "int width(int scale)") << diff.exports[2].before << " | " << diff.exports[2].after;
        expect(fatal(diff.imports.size() == 1u));
        expect(diff.imports[0].added && diff.imports[0].module == "missing" && diff.imports[0].location.line == 3);

        const auto renamed = review::semantic_diff("src/a.cppm", std::string { "export module a;\n" }, std::string { "export module b;\n" });
        expect(renamed.baseModule == "a" && renamed.headModule == "b" && renamed.interface_changed());
        const auto implementation = review::semantic_diff("src/a.cpp", std::string { "module a;\nint f() { return 1; }\n" }, std::string { "module a;\nint f() { return 2; }\n" });
        expect(!implementation.interface_changed() && implementation.headModule == "a" && implementation.headRole == "module-implementation");
        const auto created = review::semantic_diff("src/new.cppm", std::nullopt, std::string { "export module fresh;\nexport int one();\n" });
        expect(created.headModule == "fresh" && created.exports.size() == 1u && created.exports[0].kind == review::ExportChangeKind::added);
    };

    "uses of a name are identifiers in code, qualified or in its namespace"_test = [] {
        const std::string importer { "import hello.greet;\n// decorate in a comment\nint main() {\n    auto s = \"decorate\";\n    std::println(\"{}\", hello::decorate(s));\n    other::decorate(s);\n    x.decorate();\n}\n" };
        const auto uses = review::identifier_uses(importer, "src/main.cpp", "decorate", "hello");
        expect(fatal(uses.size() == 1u)) << uses.size();
        expect(uses[0].line == 5 && uses[0].column == 31 && uses[0].text == "    std::println(\"{}\", hello::decorate(s));");
        const std::string inside { "module hello.greet;\nnamespace hello {\nstd::string decorate(std::string_view text) { return {}; }\nstd::string greet() { return decorate(\"x\"); }\n}\n" };
        const auto unqualified = review::identifier_uses(inside, "src/greet/greet.cpp", "decorate", "hello");
        expect(fatal(unqualified.size() == 1u)) << "the definition is a declaration, not a use";
        expect(unqualified[0].line == 4);
        const std::string raw { "auto text = R\"x(decorate)x\"; int n = decorate(1);\n" };
        expect(review::identifier_uses(raw, "a.cpp", "decorate", "").size() == 1u) << "a raw string is not code";
        // An encoding prefix before R, and a quote inside the raw string, change nothing.
        const std::string wide { "auto path = LR\"(C:\\Data\\\"decorate\")\"; auto s = u8R\"y(decorate)y\"; int m = decorate(2);\n" };
        expect(review::identifier_uses(wide, "a.cpp", "decorate", "").size() == 1u) << review::identifier_uses(wide, "a.cpp", "decorate", "").size();
    };

    "compiler output of every family becomes diagnostics in the workspace"_test = [] {
        const std::string output {
            "[1/3] building\n"
            "/build/copy/src/main.cpp:5:45: error: designator order for field 'x' does not match declaration order\n"
            "/usr/include/c++/16/bits/format.h:12:1: note: in a header of the toolchain\n"
            "/toolchain/include/vector:99:3: error: not the project's\n"
            "src/calc/geometry.cppm:7:8: warning: unused parameter 'v'\n"
            "src/text/text.cppm(4,10): error C2065: 'x': undeclared identifier\n"
            "/build/copy/src/main.cpp:5:45: error: designator order for field 'x' does not match declaration order\n"
        };
        const auto texts = [](std::string_view path) -> std::string {
            if (path.ends_with("main.cpp")) return "import std;\nimport calc;\n\nint main() {\n    const calc::vec2 v { .y = 4.0, .x = 3.0 };\n}\n";
            return {};
        };
        const auto diagnostics = verify::parse_compiler_output(output, "/build/copy", "/work/project", texts);
        expect(fatal(diagnostics.size() == 3u)) << diagnostics.size() << " (the toolchain's own headers and a repeated line are left out)";
        expect(diagnostics[0].severity == "error" && diagnostics[0].location.file == "src/main.cpp" && diagnostics[0].location.line == 5 && diagnostics[0].location.column == 45);
        expect(diagnostics[0].location.text == "    const calc::vec2 v { .y = 4.0, .x = 3.0 };");
        expect(diagnostics[1].severity == "warning" && diagnostics[1].location.file == "src/calc/geometry.cppm" && diagnostics[1].message == "unused parameter 'v'");
        expect(diagnostics[2].severity == "error" && diagnostics[2].location.file == "src/text/text.cppm" && diagnostics[2].location.line == 4 && diagnostics[2].location.column == 10);
        expect(diagnostics[2].message == "'x': undeclared identifier") << diagnostics[2].message;
    };

    "findings become SARIF results and LSP diagnostics"_test = [] {
        spec::Finding finding;
        finding.id = "F1";
        finding.rule = "module/export-removed-in-use";
        finding.severity = spec::Severity::error;
        finding.message = "hello.greet no longer exports format_name";
        finding.location = spec::Location { "src/greet/greet.cppm", 12, 13, "std::string f\xC3\xA9nction();", 12, 21 };
        finding.evidence.push_back(spec::Evidence { "E1", "reference", spec::Location { "src/main.cpp", 8, 17, "use(format_name(user));" }, "" });
        finding.fingerprint = spec::fingerprint_of(finding);
        const Json sarif = review::to_sarif(std::span { &finding, 1 }, "/work/project", "HEAD");
        expect(sarif["version"] == "2.1.0" && sarif["runs"][0]["columnKind"] == "unicodeCodePoints");
        const Json& result = sarif["runs"][0]["results"][0];
        expect(result["ruleId"] == "module/export-removed-in-use" && result["level"] == "error" && result["ruleIndex"] == 0);
        expect(result["locations"][0]["physicalLocation"]["artifactLocation"]["uriBaseId"] == "%SRCROOT%");
        expect(result["locations"][0]["physicalLocation"]["region"]["startLine"] == 12);
        expect(result["relatedLocations"][0]["physicalLocation"]["region"]["startColumn"] == 17);
        expect(result["partialFingerprints"]["mcppls/v1"] == finding.fingerprint);
        expect(sarif["runs"][0]["tool"]["driver"]["rules"][0]["defaultConfiguration"]["level"] == "error") << sarif["runs"][0]["tool"].dump();

        const Json diagnostic = review::to_lsp_diagnostic(finding, [](std::string_view file) { return "file:///work/project/" + std::string { file }; });
        expect(diagnostic["code"] == "module/export-removed-in-use" && diagnostic["severity"] == 1 && diagnostic["source"] == "mcppls review");
        expect(diagnostic["range"]["start"]["line"] == 11 && diagnostic["range"]["start"]["character"] == 12) << diagnostic.dump();
        expect(diagnostic["relatedInformation"][0]["location"]["uri"] == "file:///work/project/src/main.cpp");
        expect(diagnostic["data"]["fingerprint"] == finding.fingerprint);
        const std::string markdown { review::to_markdown(std::span { &finding, 1 }, "HEAD", Json { { "counts", Json { { "error", 1 } } } }) };
        expect(markdown.find("F1 error — module/export-removed-in-use") != std::string::npos) << markdown;
    };

    return report();
}
