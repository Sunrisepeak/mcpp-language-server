// Semantic tokens over time (mcpp design 2026-10-28 §11.1, H1): the edits a didChange makes, kept tokens moved
// with them, the tokens of a range, and the store's rule for a compiler answer that is older than the text.
import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.base.text;
import mcppls.orchestrator.tokens;
import mcppls.orchestrator.highlights;
import mcppls.base.path;
import mcpplibs.os;

namespace os = mcpplibs::os;
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

    "a delta is the one edit between the common prefix and suffix, and applies back"_test = [] {
        const std::vector<std::uint32_t> previous { 0, 0, 6, 15, 0, 1, 4, 3, 2, 0, 2, 0, 5, 12, 0 };
        // A line inserted after the first token: one token added, the next one's deltaLine grows by one.
        const std::vector<std::uint32_t> next { 0, 0, 6, 15, 0, 1, 2, 4, 7, 0, 1, 4, 3, 2, 0, 2, 0, 5, 12, 0 };
        const Json edits = highlights::edits_between(previous, next);
        expect(fatal(edits.is_array() && edits.size() == 1u));
        std::vector<std::uint32_t> applied { previous };
        const std::size_t start { edits[0]["start"].get<std::size_t>() };
        const std::size_t count { edits[0]["deleteCount"].get<std::size_t>() };
        std::vector<std::uint32_t> inserted;
        for (const auto& value : edits[0]["data"]) inserted.push_back(value.get<std::uint32_t>());
        applied.erase(applied.begin() + static_cast<std::ptrdiff_t>(start), applied.begin() + static_cast<std::ptrdiff_t>(start + count));
        applied.insert(applied.begin() + static_cast<std::ptrdiff_t>(start), inserted.begin(), inserted.end());
        expect(applied == next);
        expect(inserted.size() <= 6u) << "the edit carries the change, not the file";
        expect(highlights::edits_between(next, next).empty());
    };

    "every whole answer is remembered under a new resultId"_test = [] {
        highlights::Store store;
        const std::string first { store.remember("file:///a.cpp", { 0, 0, 1, 2, 0 }) };
        const std::string second { store.remember("file:///a.cpp", { 0, 0, 1, 3, 0 }) };
        expect(first != second);
        expect(store.find("file:///a.cpp")->resultId == second);
        expect(store.find("file:///a.cpp")->sent == std::vector<std::uint32_t> { 0, 0, 1, 3, 0 });
    };

    "kept tokens round-trip through the disk's form, and only under their key"_test = [] {
        const highlights::CacheFile file { "key-1", { { 0, 0, 6, 15, 0 }, { 0, 7, 3, 27, 1 }, { 900, 4, 128, 12, 0x30000 } } };
        const std::string bytes { highlights::encode_cache(file) };
        const auto back = highlights::decode_cache(bytes);
        expect(fatal(back.has_value()));
        expect(back->key == "key-1");
        expect(fatal(back->tokens.size() == 3u));
        expect(back->tokens[2].line == 900 && back->tokens[2].length == 128 && back->tokens[2].modifiers == 0x30000u);
        // Relative LEB128 integers: well under the twenty bytes per token the integers take as JSON text.
        expect(bytes.size() < 40u);
    };

    "a damaged or foreign file is no tokens at all"_test = [] {
        const std::string bytes { highlights::encode_cache(highlights::CacheFile { "k", { { 1, 2, 3, 4, 5 } } }) };
        expect(!highlights::decode_cache(bytes.substr(0, bytes.size() - 1)).has_value()) << "truncated";
        expect(!highlights::decode_cache(bytes + "x").has_value()) << "trailing bytes";
        expect(!highlights::decode_cache("not a tokens file").has_value());
        std::string otherFormat { bytes };
        otherFormat[8] = 9;
        expect(!highlights::decode_cache(otherFormat).has_value()) << "another format version";
    };

    "the disk keeps the newest files within its count and size"_test = [] {
        std::error_code ignored;
        const std::string directory { mcppls::base::join_path(base::spelled(os::dirs::temp()),
                                                              std::format("mcppls-test-tokens-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        expect(fatal([&] { std::error_code failed; std::filesystem::create_directories(directory, failed); return !failed; }()));
        for (int i { 0 }; i < 5; ++i) {
            expect(os::fs::write(mcppls::base::join_path(directory, std::format("{}.tokens", i)), std::string(100, 'x')).has_value());
            std::this_thread::sleep_for(std::chrono::milliseconds { 20 });
        }
        expect(os::fs::write(mcppls::base::join_path(directory, "other.txt"), "kept").has_value());
        expect(highlights::prune_cache(directory, 3, 1 << 20) == 200u);
        expect(!std::filesystem::exists(mcppls::base::join_path(directory, "0.tokens"), ignored) && !std::filesystem::exists(mcppls::base::join_path(directory, "1.tokens"), ignored));
        expect(std::filesystem::exists(mcppls::base::join_path(directory, "4.tokens"), ignored) && std::filesystem::exists(mcppls::base::join_path(directory, "other.txt"), ignored));
        expect(highlights::prune_cache(directory, 10, 150) == 200u) << "beyond the bytes, the oldest go too";
        expect(std::filesystem::exists(mcppls::base::join_path(directory, "4.tokens"), ignored));
        std::filesystem::remove_all(directory, ignored);
    };

    "the file of a document is named by its path, the same every time"_test = [] {
        expect(highlights::cache_file_name("/a/b.cpp") == highlights::cache_file_name("/a/b.cpp"));
        expect(highlights::cache_file_name("/a/b.cpp") != highlights::cache_file_name("/a/c.cpp"));
        expect(highlights::cache_file_name("/a/b.cpp").ends_with(".tokens"));
    };

    return report();
}
