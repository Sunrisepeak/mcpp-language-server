// The answer layer (answer-layer design 2026-10-13): where the cursor is, read from the text; the three keys; what is
// kept of an engine answer and how it is kept and persisted; when a kept answer is still the engine's; and the answers
// composed from kept ones -- placed, filtered, brought up to a class that changed, marked only where in doubt. With the
// outline it reads classes and declarations by.
import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.base.text;
import mcppls.orchestrator.answer;
import mcppls.orchestrator.answering;
import mcppls.orchestrator.outline;

using Json = nlohmann::json;
using mcppls::base::Position;
namespace answer = mcppls::orchestrator::answer;
namespace outline = mcppls::orchestrator::outline;
using answer::Context;
using answer::Kind;

namespace {

// The position of the first `|` in `marked`, and the text without it.
std::pair<std::string, Position> cursor(std::string_view marked) {
    const std::size_t at { marked.find('|') };
    std::string text { marked.substr(0, at) };
    text += marked.substr(at + 1);
    return { text, mcppls::base::position_at(text, at) };
}

std::vector<std::string> labels_of(const Json& result) {
    std::vector<std::string> labels;
    for (const auto& item : result.value("items", Json::array())) labels.push_back(item.value("label", std::string {}));
    return labels;
}

Json item(std::string label, std::string sort = "1") {
    return Json { { "label", label }, { "kind", 2 }, { "sortText", sort }, { "filterText", label },
                  { "textEdit", Json { { "range", Json { { "start", Json { { "line", 0 }, { "character", 0 } } }, { "end", Json { { "line", 0 }, { "character", 0 } } } } },
                                       { "newText", label } } } };
}

answer::Entry completion_entry(std::vector<std::string> labels) {
    answer::Entry entry;
    entry.kind = Kind::completion;
    Json items = Json::array();
    for (auto& label : labels) items.push_back(item(label));
    entry.result = Json { { "items", items } };
    entry.fingerprint = "fp";
    return entry;
}

} // namespace

int main() {
    using namespace mcppls::testing;

    "the place a completion is asked at, and what it is asked about"_test = [] {
        const auto [member, at] = cursor("void f() {\n    fileOverview.push_b|\n}\n");
        const auto context = answer::context_at(member, at, Kind::completion);
        expect(fatal(context.has_value()));
        expect(context->place == Context::Place::member);
        expect(context->receiver == "fileOverview") << context->receiver;
        expect(context->word == "push_b");
        expect(context->enclosing == "f@0") << context->enclosing;

        const auto [arrow, at2] = cursor("struct S { void g(); };\nvoid S::g() {\n    m_logger->|\n}\n");
        const auto second = answer::context_at(arrow, at2, Kind::completion);
        expect(fatal(second.has_value()));
        expect(second->place == Context::Place::arrow && second->receiver == "m_logger") << second->receiver;
        expect(second->enclosing == "g@1") << second->enclosing;

        const auto [qualified, at3] = cursor("int x = std::views::|;\n");
        const auto third = answer::context_at(qualified, at3, Kind::completion);
        expect(fatal(third.has_value()));
        expect(third->place == Context::Place::qualified && third->receiver == "std::views") << third->receiver;

        const auto [chain, at4] = cursor("void f() { items[0].value().|x; }");
        const auto fourth = answer::context_at(chain, at4, Kind::completion);
        expect(fatal(fourth.has_value()));
        expect(fourth->receiver == "items[0].value()") << fourth->receiver;

        // In a comment or a string: nothing kept answers there.
        const auto [comment, at5] = cursor("// fileOverview.|\n");
        expect(!answer::context_at(comment, at5, Kind::completion).has_value());
        const auto [literal, at6] = cursor("auto s = \"a.b|\";\n");
        expect(!answer::context_at(literal, at6, Kind::completion).has_value());
    };

    "hover and jumps are about the identifier under the cursor; signature help about the call"_test = [] {
        const auto [text, at] = cursor("void f() { doc.du|mp(); }");
        const auto hover = answer::context_at(text, at, Kind::hover);
        expect(fatal(hover.has_value()));
        expect(hover->word == "dump" && hover->place == Context::Place::member && hover->receiver == "doc") << hover->word;
        const auto [blank, at2] = cursor("void f() { | }");
        expect(!answer::context_at(blank, at2, Kind::definition).has_value());

        const auto [call, at3] = cursor("void f() { spdlog::info(\"{}\", g(1, 2), |); }");
        const auto signature = answer::context_at(call, at3, Kind::signature_help);
        expect(fatal(signature.has_value()));
        expect(signature->place == Context::Place::call && signature->receiver == "spdlog::info") << signature->receiver;
        expect(signature->argument == 2) << signature->argument;
    };

    "the local hash follows the enclosing body before the word, not what follows it"_test = [] {
        const auto [a, at] = cursor("void f() {\n    int x = 1;\n    doc.|\n}\nvoid g() {}\n");
        const auto [b, bt] = cursor("void f() {\n    int x = 1;\n    doc.|\n}\nvoid g() { int changed; }\n");
        const auto [c, ct] = cursor("void f() {\n    int x = 2;\n    doc.|\n}\n");
        expect(answer::context_at(a, at, Kind::completion)->localHash == answer::context_at(b, bt, Kind::completion)->localHash);
        expect(answer::context_at(a, at, Kind::completion)->localHash != answer::context_at(c, ct, Kind::completion)->localHash);
    };

    "namespace aliases are spelled out, and a declared type is read"_test = [] {
        const std::string text { "namespace fs = std::filesystem;\nnamespace other = ::boost;\n" };
        expect(answer::expand_aliases(text, "fs") == "std::filesystem");
        expect(answer::expand_aliases(text, "fs::path") == "std::filesystem::path");
        expect(answer::expand_aliases(text, "other") == "boost");
        expect(answer::expand_aliases(text, "std::views") == "std::views");

        const std::string code { "void f(const std::vector<int>& items) {\n    ordered_json fileOverview = ordered_json::array();\n"
                                 "    auto any = 1;\n    std::shared_ptr<spdlog::logger> m_logger;\n    use(fileOverview);\n}\n" };
        expect(answer::declared_type(code, code.size(), "fileOverview") == std::optional<std::string> { "ordered_json" });
        expect(answer::declared_type(code, code.size(), "items") == std::optional<std::string> { "std::vector<int>" });
        expect(answer::declared_type(code, code.size(), "m_logger") == std::optional<std::string> { "std::shared_ptr<spdlog::logger>" });
        expect(!answer::declared_type(code, code.size(), "any").has_value());
        expect(!answer::declared_type(code, code.size(), "missing").has_value());
    };

    "keys: the position's, the name's, the meaning's"_test = [] {
        Context context;
        context.place = Context::Place::qualified;
        context.receiver = "fs";
        context.enclosing = "f@1";
        context.word = "pa";
        expect(answer::k1("file:///a.cpp", Kind::completion, context) == "1|file:///a.cpp|completion|f@1|qualified|fs");
        expect(answer::k3(Kind::completion, context, "std::filesystem") == std::optional<std::string> { "3|completion|std::filesystem" });
        expect(answer::k3(Kind::hover, context, "std::filesystem") == std::optional<std::string> { "3|hover|std::filesystem::pa" });
        context.place = Context::Place::member;
        expect(!answer::k3(Kind::completion, context, "x").has_value());
        expect(answer::k2(Kind::completion, Context::Place::member, answer::type_key("basic_json", "/i/json.hpp", 10))
               == "2|completion|member|basic_json@/i/json.hpp:10");
    };

    "what is kept of an answer: what is shown, short documentation, nothing of an empty one"_test = [] {
        Json list { { "isIncomplete", false }, { "items", Json::array({ Json { { "label", "push_back" }, { "kind", 2 }, { "score", 1.5 },
                                                                               { "documentation", std::string(2000, 'd') }, { "commitCharacters", Json::array({ "(" }) } } }) } };
        const auto kept = answer::trim(Kind::completion, list);
        expect(fatal(kept.has_value()));
        expect((*kept)["items"][0]["label"] == "push_back" && !(*kept)["items"][0].contains("commitCharacters"));
        expect((*kept)["items"][0]["documentation"].get<std::string>().size() < 600u);
        expect(!answer::trim(Kind::completion, Json { { "items", Json::array() } }).has_value());
        expect(!answer::trim(Kind::hover, Json(nullptr)).has_value());
        expect(!answer::trim(Kind::definition, Json::array()).has_value());
        const auto hover = answer::trim(Kind::hover, Json { { "contents", Json { { "kind", "markdown" }, { "value", "int x" } } }, { "range", Json::object() } });
        expect(fatal(hover.has_value()));
        expect(!hover->contains("range"));
    };

    "the store keeps the most recently used within its budget, and persists"_test = [] {
        answer::Store store { 4096 };
        for (int i { 0 }; i < 40; ++i) store.put(std::format("1|k{}", i), completion_entry({ std::format("name{}", i) }));
        expect(store.size() < 40u && store.bytes() <= 4096u) << store.size() << " " << store.bytes();
        expect(store.find("1|k39") != nullptr);
        expect(store.find("1|k0") == nullptr);

        answer::Store small;
        small.put("3|completion|std", completion_entry({ "vector", "string" }));
        small.put("1|file:///a.cpp|completion||member|x", completion_entry({ "size" }));
        const Json everything = small.to_json();
        const Json names = small.to_json(true);
        expect(names["entries"].size() == 1u && names["entries"][0]["key"] == "3|completion|std");
        answer::Store back;
        expect(back.adopt(everything));
        expect(back.size() == 2u);
        const answer::Entry* adopted { back.find("3|completion|std") };
        expect(fatal(adopted != nullptr));
        expect(adopted->fingerprint == "fp" && labels_of(adopted->result) == std::vector<std::string> { "vector", "string" });
        // Another session's change counts are never this session's.
        answer::Now now { "fp", 0, 0, {} };
        expect(answer::certainty_of(*adopted, answer::Source::k3, now) == answer::Certainty::uncertain);
        expect(!back.adopt(Json { { "format", 999 }, { "entries", Json::array() } }));
    };

    "certainty: the fingerprint, the local text, other files, the provider"_test = [] {
        answer::Entry entry = completion_entry({ "size" });
        entry.others = 3;
        entry.localHash = 7;
        std::map<std::string, std::uint64_t, std::less<>> generations { { "/p/json.hpp", 1 } };
        answer::Now now { "fp", 3, 7, [&](std::string_view path) { const auto it = generations.find(path); return it == generations.end() ? 0 : it->second; } };
        expect(answer::certainty_of(entry, answer::Source::k1, now) == answer::Certainty::certain);
        now.localHash = 8;
        expect(answer::certainty_of(entry, answer::Source::k1, now) == answer::Certainty::uncertain);
        now.localHash = 7;
        now.others = 4;
        expect(answer::certainty_of(entry, answer::Source::k1, now) == answer::Certainty::uncertain);
        now.fingerprint = "other";
        expect(answer::certainty_of(entry, answer::Source::k3, now) == answer::Certainty::uncertain);
        now.fingerprint = "fp";
        entry.provider = "/p/json.hpp";
        entry.providerGeneration = 1;
        expect(answer::certainty_of(entry, answer::Source::k2, now) == answer::Certainty::certain);
        generations["/p/json.hpp"] = 2;
        expect(answer::provider_changed(entry, now));
        expect(answer::certainty_of(entry, answer::Source::k2, now) == answer::Certainty::uncertain);
    };

    "a kept completion is complete only when certain, complete, and asked for a word the one typed now extends"_test = [] {
        const auto [text, at] = cursor("void f() {\n    doc.pu|\n}\n");
        const auto context = answer::context_at(text, at, Kind::completion);
        expect(fatal(context.has_value()));
        answer::Entry entry = completion_entry({ "push_back", "pop_back", "size" });
        entry.complete = true;
        entry.word = "p";
        const auto compose = [&](const answer::Entry& e, answer::Certainty certainty) {
            return answer::compose_completion(e, answer::Source::k1, certainty, *context, text, at, answer::Marks::subtle, std::nullopt, false).result;
        };
        expect(compose(entry, answer::Certainty::certain)["isIncomplete"] == false) << "as the engine gave it: the editor filters it";
        expect(compose(entry, answer::Certainty::uncertain)["isIncomplete"] == true) << "something changed: the engine's answer replaces it";
        answer::Entry narrower = entry;
        narrower.word = "pop";   // filtered for `pop`: `pu` is not within it
        expect(compose(narrower, answer::Certainty::certain)["isIncomplete"] == true);
        answer::Entry incomplete = entry;
        incomplete.complete = false;
        expect(compose(incomplete, answer::Certainty::certain)["isIncomplete"] == true);
        // Kept between sessions as it was.
        answer::Store store;
        store.put("1|k", entry);
        answer::Store again;
        expect(again.adopt(store.to_json()));
        const answer::Entry* read { again.find("1|k") };
        expect(fatal(read != nullptr));
        expect(read->complete && read->word == "p");
    };

    "a kept completion is filtered by the word, placed on it, and incomplete"_test = [] {
        const auto [text, at] = cursor("void f() {\n    doc.pu|\n}\n");
        const auto context = answer::context_at(text, at, Kind::completion);
        expect(fatal(context.has_value()));
        answer::Entry entry = completion_entry({ "push_back", "size", "dump" });
        entry.result["items"][0]["additionalTextEdits"] = Json::array({ Json::object() });
        const auto composed = answer::compose_completion(entry, answer::Source::k2, answer::Certainty::certain, *context, text, at, answer::Marks::subtle,
                                                         std::nullopt, false);
        expect(composed.result["isIncomplete"] == true);
        expect(labels_of(composed.result) == std::vector<std::string> { "push_back" });
        const Json& edit = composed.result["items"][0]["textEdit"]["range"];
        expect(edit["start"]["line"] == 1 && edit["start"]["character"] == 8 && edit["end"]["character"] == 10) << edit.dump();
        expect(!composed.result["items"][0].contains("additionalTextEdits"));
        expect(composed.doubted == 0u);
        expect(!composed.result["items"][0].contains("labelDetails"));   // certain: no mark
    };

    "a class that changed: members added and removed by its outline, marked; only those"_test = [] {
        const auto [text, at] = cursor("void f() {\n    doc.|\n}\n");
        const auto context = answer::context_at(text, at, Kind::completion);
        const answer::Entry entry = completion_entry({ "push_back", "size", "removed" });
        const std::optional<std::vector<std::string>> members { std::vector<std::string> { "push_back", "size", "added" } };
        const auto composed = answer::compose_completion(entry, answer::Source::k2, answer::Certainty::uncertain, *context, text, at, answer::Marks::subtle, members,
                                                         true);
        const auto labels = labels_of(composed.result);
        expect(labels == std::vector<std::string> { "push_back", "size", "added", "removed" }) << labels.size();
        expect(composed.doubted == 2u);
        const Json& items = composed.result["items"];
        expect(!items[0].contains("labelDetails"));
        expect(items[2]["labelDetails"]["description"] == "·" && items[2]["preselect"] == false);
        expect(items[3]["labelDetails"]["description"] == "·" && items[3]["sortText"].get<std::string>().starts_with("~"));
        expect(items[0]["preselect"] == false);   // uncertain: nothing preselected

        // Off: the same answer, no marks.
        const auto quiet = answer::compose_completion(entry, answer::Source::k2, answer::Certainty::uncertain, *context, text, at, answer::Marks::off, members, true);
        expect(!quiet.result["items"][2].contains("labelDetails"));
        // A changed class and no outline: every item is in doubt.
        const auto unread = answer::compose_completion(entry, answer::Source::k2, answer::Certainty::uncertain, *context, text, at, answer::Marks::subtle,
                                                       std::nullopt, true);
        expect(unread.doubted == 3u);
        // Verbose: every item says where it came from.
        const auto verbose = answer::compose_completion(entry, answer::Source::k2, answer::Certainty::certain, *context, text, at, answer::Marks::verbose,
                                                        std::nullopt, false);
        expect(verbose.result["items"][0]["documentation"]["value"].get<std::string>().find("cache:K2") != std::string::npos);
    };

    "hover is placed on the word; signature help on the argument; jumps only when certain"_test = [] {
        const auto [text, at] = cursor("void f() { doc.du|mp(); }");
        const auto context = answer::context_at(text, at, Kind::hover);
        answer::Entry hover;
        hover.kind = Kind::hover;
        hover.result = Json { { "contents", Json { { "kind", "markdown" }, { "value", "dump()" } } } };
        const auto composed = answer::compose_hover(hover, answer::Source::k1, answer::Certainty::certain, *context, text, answer::Marks::subtle);
        expect(composed.result["range"]["start"]["character"] == 15 && composed.result["range"]["end"]["character"] == 19) << composed.result.dump();
        expect(composed.result["contents"]["value"] == "dump()");

        const auto [call, at2] = cursor("void f() { g(1, |); }");
        const auto signatureContext = answer::context_at(call, at2, Kind::signature_help);
        answer::Entry signature;
        signature.kind = Kind::signature_help;
        signature.result = Json { { "signatures", Json::array({ Json { { "label", "g(int, int)" } } }) } };
        expect(answer::compose_signature(signature, *signatureContext).result["activeParameter"] == 1);

        answer::Entry jump;
        jump.kind = Kind::definition;
        jump.result = Json::array({ Json { { "uri", "file:///a.h" } } });
        expect(answer::compose_jump(jump, answer::Certainty::certain, std::nullopt).has_value());
        expect(!answer::compose_jump(jump, answer::Certainty::uncertain, std::nullopt).has_value());
        expect(answer::compose_jump(jump, answer::Certainty::uncertain, Json::array()).has_value());
    };

    "what a person saw differed by, and provenance for agents"_test = [] {
        const Json served { { "items", Json::array({ item("a"), item("b") }) } };
        const Json answered { { "items", Json::array({ item("b"), item("c"), item("d") }) } };
        expect(answer::difference(served, answered) == std::pair<std::size_t, std::size_t> { 1u, 2u });
        answer::Provenance authoritative;
        expect(answer::to_json(authoritative)["authoritative"] == true);
        answer::Provenance kept { answer::Source::k3, answer::Certainty::uncertain, 180000, true, true, {} };
        kept.note = answer::note_for(Kind::completion, kept);
        const Json value = answer::to_json(kept);
        expect(value["authoritative"] == false && value["source"] == "cache:K3" && value["providerChanged"] == true);
        expect(value["note"].get<std::string>().find("3 min ago") != std::string::npos) << value.dump();
    };

    "prefetch asks only in a function's body, never in a declaration's head, a directive or a comment"_test = [] {
        namespace answering = mcppls::orchestrator::answering;
        answering::Layer layer { answering::Settings {}, "", "", [](std::string_view) { return std::optional<std::string> {}; } };
        const std::string text { "#include <a::b>\nnamespace app::detail {\nstruct S : base::Thing {};\n// see doc.dump\n"
                                 "void f() {\n    doc.dump();\n}\n}\n" };
        const auto place = layer.prefetch_place("file:///a.cpp", text, mcppls::base::Position { 1, 0 }, "fp");
        expect(fatal(place.has_value()));
        expect(place->line == 5 && place->character == 8) << place->line << ":" << place->character;   // `doc.` in f()
        const std::string heads { "namespace app::detail {\nstruct S : base::Thing {};\n}\n" };
        expect(!layer.prefetch_place("file:///b.cpp", heads, mcppls::base::Position { 0, 0 }, "fp").has_value());
    };

    "a class's members, read from its body"_test = [] {
        const std::string text {
            "template <class T>\n"
            "class basic_json : public base {\n"
            "public:\n"
            "    using value_type = basic_json;\n"
            "    basic_json() = default;\n"
            "    ~basic_json();\n"
            "    void push_back(T value) { items.push_back(value); }\n"
            "    std::size_t size() const noexcept;\n"
            "    template <class U> U get() const;\n"
            "    static constexpr int version = 3, revision { 1 };\n"
            "    bool operator==(const basic_json&) const;\n"
            "    friend void swap(basic_json&, basic_json&);\n"
            "    struct iterator { int at; };\n"
            "private:\n"
            "    std::vector<T> items;\n"
            "    int flags : 3;\n"
            "};\n" };
        const auto members = outline::class_members(text, 1);
        expect(fatal(members.has_value()));
        const std::vector<std::string> expected { "value_type", "push_back", "size", "get", "version", "revision", "iterator", "items", "flags" };
        expect(*members == expected) << [&] { std::string all; for (auto& m : *members) all += m + ","; return all; }();
        expect(!outline::class_members("class only;\n", 0).has_value());
        expect(!outline::class_members("struct S {\n#if X\n  int a;\n#endif\n};\n", 0).has_value());
        expect(!outline::class_members("struct S {\n  int a;\n", 0).has_value());
    };

    "where a name is declared, not where it is used"_test = [] {
        const std::string text {
            "namespace gpp {\n"
            "export std::string gppTr(std::string_view key);\n"
            "struct Tool { int gppTr; };\n"
            "void use() { auto x = gppTr(\"k\"); obj.gppTr; }\n"
            "}\n"
            "using gppTr2 = int;\n" };
        const auto places = outline::declarations(text, "gppTr");
        expect(places == std::vector<outline::Place> { { 1, 19 }, { 2, 18 } }) << places.size();
        expect(outline::declarations(text, "gppTr2").size() == 1u);
        expect(outline::declarations(text, "use").size() == 1u);
    };

    return report();
}
