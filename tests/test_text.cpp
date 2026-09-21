import std;
import mcppls.testing;
import mcppls.base.text;

using namespace mcppls::base;

int main() {
    using namespace mcppls::testing;

    "UTF-16 columns count surrogate pairs twice"_test = [] {
        const std::string_view text { "a\xF0\x9F\x98\x80" "b" };   // a😀b
        expect(position_at(text, 5) == Position { 0, 3 });
        expect(offset_at(text, Position { 0, 3 }) == std::optional<std::size_t> { 5 });
        expect(utf16_length(text) == 4u);
    };

    "two-byte and three-byte sequences are one unit"_test = [] {
        const std::string_view text { "\xC3\xA9x\xE4\xB8\xAD" "y" };   // éx中y
        expect(position_at(text, 2) == Position { 0, 1 });
        expect(position_at(text, 6) == Position { 0, 3 });
        expect(offset_at(text, Position { 0, 3 }) == std::optional<std::size_t> { 6 });
    };

    "lines and CRLF"_test = [] {
        const std::string_view text { "one\r\ntwo\nthree" };
        expect(position_at(text, 5) == Position { 1, 0 });
        expect(offset_at(text, Position { 2, 2 }) == std::optional<std::size_t> { 11 });
        expect(offset_at(text, Position { 0, 99 }) == std::optional<std::size_t> { 3 });
        expect(!offset_at(text, Position { 9, 0 }).has_value());
        expect(split_lines(text).size() == 3u);
    };

    "trim split join"_test = [] {
        expect(trim("  a b \t\n") == "a b");
        expect(split("a;b;;c", ';').size() == 4u);
        const std::vector<std::string> parts { "x", "y", "z" };
        expect(join(parts, ", ") == "x, y, z");
        expect(replace_all("a.b.c", ".", "::") == "a::b::c");
        expect(iequals_ascii("Hello", "hELLO"));
        expect(to_lower_ascii("MiXeD") == "mixed");
    };

    "identifiers"_test = [] {
        expect(is_identifier_start('_') && is_identifier_start('z') && !is_identifier_start('1'));
        expect(is_identifier_char('1') && !is_identifier_char('.'));
    };

    return report();
}
