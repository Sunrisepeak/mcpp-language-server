// S5 data (docs/specs/s5-semantic-query.md): locations, fingerprints, findings, and reading the core
// engine's hover and identifiers.
import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.base.text;
import mcppls.spec.query;
import mcppls.ai.query.symbols;

using Json = nlohmann::json;
namespace spec = mcppls::spec;
namespace query = mcppls::ai::query;

int main() {
    using namespace mcppls::testing;

    "columns count Unicode scalar values, LSP characters UTF-16 code units"_test = [] {
        // "int a😀 = 1;": the emoji is one column and two UTF-16 units.
        const std::string line { "int a\xF0\x9F\x98\x80 = 1;" };
        expect(spec::column_from_utf16(line, 0) == 1);
        expect(spec::column_from_utf16(line, 5) == 6);
        expect(spec::column_from_utf16(line, 7) == 7) << spec::column_from_utf16(line, 7);
        expect(spec::utf16_from_column(line, 7) == 7);
        expect(spec::utf16_from_column(line, 6) == 5);
        // "é" is two UTF-8 bytes, one column and one unit.
        expect(spec::column_from_utf16("caf\xC3\xA9 x", 5) == 6);
        const std::string text { "import std;\r\nint a\xF0\x9F\x98\x80 = 1;\n" };
        const spec::Location location = spec::location_in(text, "src/a.cpp", mcppls::base::Position { 1, 8 }, mcppls::base::Position { 1, 9 });
        expect(location.line == 2 && location.column == 8 && location.text == line) << location.text;
        expect(location.endLine == 2 && location.endColumn == 9);
        const auto back = spec::lsp_position(text, 2, 8);
        expect(back.line == 1 && back.character == 8);
        expect(spec::line_of(text, 0) == "import std;") << "a carriage return is not part of the line";
    };

    "a finding's fingerprint survives lines moving and reindenting"_test = [] {
        spec::Finding finding;
        finding.rule = "module/export-removed-in-use";
        finding.severity = spec::Severity::error;
        finding.message = "hello.greet no longer exports format_name";
        finding.location = spec::Location { "src/greet/greet.cppm", 12, 1, "export std::string format_name(std::string_view name);" };
        finding.evidence.push_back(spec::Evidence { "E1", "reference", spec::Location { "src/main.cpp", 8, 17, "std::println(\"{}\", format_name(user));" }, "" });
        const std::string original { spec::fingerprint_of(finding) };
        expect(original.starts_with("sha256:") && original.size() == 7 + 64) << original;
        finding.location.line = 40;
        finding.location.text = "    export   std::string format_name(std::string_view name);";
        expect(spec::fingerprint_of(finding) == original);
        finding.evidence[0].location.text = "std::println(\"{}\", format_name(other));";
        expect(spec::fingerprint_of(finding) != original);

        const Json json = spec::to_json(finding);
        expect(json["severity"] == "error" && json["origin"] == "rule" && json["fix"].is_null()) << json.dump();
        const auto parsed = spec::finding_from_json(json);
        expect(fatal(parsed.has_value()));
        expect(parsed->rule == finding.rule && parsed->evidence.size() == 1u && parsed->evidence[0].location == finding.evidence[0].location);
        expect(!spec::finding_from_json(Json::parse(R"({"rule": "r", "severity": "fatal", "message": "m", "location": {"file": "a", "line": 1}})")).has_value())
            << "an unknown severity is not a finding";
    };

    "the core engine's hover is taken apart"_test = [] {
        const std::string markdown {
            "### function `greet`\n\n---\n→ `std::string`\nParameters:\n- `std::string_view who`\n\nGreets someone by name.\n\n---\n```cpp\n// In namespace hello\nstd::string greet(std::string_view who)\n```"
        };
        const auto parts = query::parse_hover(markdown);
        expect(parts.kind == "function" && parts.name == "greet") << parts.kind << " " << parts.name;
        expect(parts.type == "std::string") << parts.type;
        expect(parts.documentation == "Greets someone by name.") << parts.documentation;
        expect(parts.signature == "std::string greet(std::string_view who)") << parts.signature;
        expect(parts.scope == "hello") << parts.scope;
        const auto field = query::parse_hover("### field `count`\n\n---\nType: `int`\n\n---\n```cpp\n// In Counter\npublic: int count\n```");
        expect(field.type == "int" && field.scope == "Counter" && field.signature == "public: int count") << field.signature;
    };

    "a USR names the symbol it ends with"_test = [] {
        expect(query::name_from_usr("c:@N@hello@F@greet#$@N@std@N@__1@S@basic_string_view>#C#") == "greet");
        expect(query::name_from_usr("c:@N@hello@S@Greeter@F@hi#") == "hi");
        expect(query::name_from_usr("c:@N@calc@S@Vec@FI@x") == "x");
        expect(query::name_from_usr("c:@N@hello") == "hello");
    };

    return report();
}
