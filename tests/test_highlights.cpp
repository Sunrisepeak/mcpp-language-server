// Semantic tokens over time (mcpp design 2026-10-28 §11.1, H1): the edits a didChange makes, kept tokens moved
// with them, the tokens of a range, and the store's rule for a compiler answer that is older than the text.
import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.base.text;
import mcppls.orchestrator.tokens;
import mcppls.orchestrator.highlights;

using Json = nlohmann::json;
using mcppls::orchestrator::tokens::Token;
namespace highlights = mcppls::orchestrator::highlights;
namespace base = mcppls::base;

namespace {

Json range_change(int startLine, int startCharacter, int endLine, int endCharacter, std::string text) {
    return Json { { "range", Json { { "start", Json { { "line", startLine }, { "character", startCharacter } } },
                                    { "end", Json { { "line", endLine }, { "character", endCharacter } } } } },
                  { "text", std::move(text) } };
}

std::vector<std::pair<int, int>> places(const std::vector<Token>& tokens) {
    std::vector<std::pair<int, int>> out;
    for (const auto& token : tokens) out.emplace_back(token.line, token.startChar);
    return out;
}

} // namespace

int main() {
    using namespace mcppls::testing;

    "a range change is its own edit, with the inserted text's shape"_test = [] {
        const auto edits = highlights::edits_of("a\nb\n", Json::array({ range_change(1, 0, 1, 1, "x\nyz") }));
        expect(fatal(edits.size() == 1u));
        expect(edits[0].start == base::Position { 1, 0 } && edits[0].end == base::Position { 1, 1 });
        expect(edits[0].lines == 1 && edits[0].lastLength == 2);
    };

    "a whole new text is the edit between the common beginning and end"_test = [] {
        const std::string before { "int a;\nint b;\nint c;\n" };
        const std::string after { "int a;\nint inserted;\nint b;\nint c;\n" };
        const auto edits = highlights::edits_of(before, Json::array({ Json { { "text", after } } }));
        expect(fatal(edits.size() == 1u));
        // `int ` is common to both texts at line 1: the edit inserts `inserted;\nint ` there.
        expect(edits[0].start == base::Position { 1, 4 } && edits[0].end == base::Position { 1, 4 });
        expect(edits[0].lines == 1 && edits[0].lastLength == 4);
    };

    "an inserted line moves every token after it and keeps the rest"_test = [] {
        std::vector<Token> tokens { { 0, 0, 3, 1, 0 }, { 1, 4, 1, 2, 0 }, { 2, 4, 1, 2, 0 } };
        const auto edits = highlights::edits_of("int a;\nint b;\nint c;\n",
                                                Json::array({ Json { { "text", "int a;\nint inserted;\nint b;\nint c;\n" } } }));
        for (const auto& edit : edits) highlights::shift(tokens, edit);
        // `b` was at (1, 4), right where the insertion is: the edit touches it, so it is dropped; `c` moves a line down.
        expect(places(tokens) == std::vector<std::pair<int, int>> { { 0, 0 }, { 3, 4 } });
    };

    "a token after an edit on its own line moves along the line"_test = [] {
        std::vector<Token> tokens { { 0, 0, 3, 1, 0 }, { 0, 10, 3, 1, 0 }, { 1, 2, 3, 1, 0 } };
        highlights::shift(tokens, highlights::edits_of("", Json::array({ range_change(0, 5, 0, 7, "abcd") })).front());
        expect(places(tokens) == std::vector<std::pair<int, int>> { { 0, 0 }, { 0, 12 }, { 1, 2 } });
    };

    "a deletion across lines joins them: the tail of its last line moves up and left"_test = [] {
        std::vector<Token> tokens { { 0, 0, 2, 1, 0 }, { 1, 0, 2, 1, 0 }, { 2, 6, 2, 1, 0 }, { 3, 1, 2, 1, 0 } };
        highlights::shift(tokens, highlights::edits_of("", Json::array({ range_change(0, 4, 2, 3, "") })).front());
        // (1, 0) was inside the deleted text; (2, 6) is now (0, 4 + 3); (3, 1) is now (1, 1).
        expect(places(tokens) == std::vector<std::pair<int, int>> { { 0, 0 }, { 0, 7 }, { 1, 1 } });
    };

    "within keeps the tokens a range overlaps"_test = [] {
        const std::vector<Token> tokens { { 0, 0, 3, 1, 0 }, { 1, 0, 3, 1, 0 }, { 1, 8, 3, 1, 0 }, { 4, 0, 3, 1, 0 } };
        const auto inside = highlights::within(tokens, base::Range { { 1, 2 }, { 1, 9 } });
        expect(places(inside) == std::vector<std::pair<int, int>> { { 1, 0 }, { 1, 8 } });
    };

    "a compiler answer for an older version is moved by the edits since, and is not exact"_test = [] {
        highlights::Store store;
        store.at("file:///a.cpp");
        const auto edits = highlights::edits_of("x\ny\n", Json::array({ range_change(0, 0, 0, 0, "new\n") }));
        store.edited("file:///a.cpp", 1, 2, edits);
        expect(store.engine_answered("file:///a.cpp", 1, 2, std::vector<Token> { { 1, 0, 1, 3, 0 } }, true));
        const auto* kept = store.find("file:///a.cpp");
        expect(fatal(kept != nullptr));
        expect(kept->known && !kept->exact && kept->version == 2);
        expect(places(kept->core) == std::vector<std::pair<int, int>> { { 2, 0 } });
        expect(store.engine_answered("file:///a.cpp", 2, 2, std::vector<Token> { { 2, 0, 1, 3, 0 } }, true));
        expect(store.find("file:///a.cpp")->exact);
        expect(store.engine_answered("file:///a.cpp", 2, 2, std::vector<Token> {}, false));
        expect(!store.find("file:///a.cpp")->exact) << "an answer while the compiler still prepares is not exact";
    };

    "an answer for a version the log does not reach is dropped"_test = [] {
        highlights::Store store;
        store.at("file:///a.cpp");
        expect(!store.engine_answered("file:///a.cpp", 3, 5, std::vector<Token> { { 0, 0, 1, 3, 0 } }, true));
        expect(store.counters.dropped == 1u);
        expect(!store.find("file:///a.cpp")->known);
    };

    "something the compiler reads changed: kept tokens stand in, none is exact"_test = [] {
        highlights::Store store;
        store.at("file:///a.cpp");
        store.at("file:///b.cpp");
        expect(store.engine_answered("file:///a.cpp", 1, 1, std::vector<Token> { { 0, 0, 1, 3, 0 } }, true));
        expect(store.engine_answered("file:///b.cpp", 1, 1, std::vector<Token> { { 0, 0, 1, 3, 0 } }, true));
        store.unsettle_except("file:///a.cpp");
        expect(store.find("file:///a.cpp")->exact && !store.find("file:///b.cpp")->exact);
        store.unsettle_all();
        expect(!store.find("file:///a.cpp")->exact && store.find("file:///a.cpp")->known);
    };

    return report();
}
