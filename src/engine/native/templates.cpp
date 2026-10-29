module mcppls.engine.native.templates;

import std;
import nlohmann.json;
import mcppls.base.text;
import mcppls.project.scan;
import mcppls.engine.native.keywords;
import mcppls.orchestrator.outline;

namespace mcppls::index {

using Json = nlohmann::json;

namespace {

constexpr int COMPLETION_KIND_SNIPPET { 15 };
constexpr int INSERT_TEXT_PLAIN { 1 };
constexpr int INSERT_TEXT_SNIPPET { 2 };
constexpr std::string_view BLANKS { " \t\v\f\r" };
constexpr std::string_view DETAIL { "code template" };

struct Template {
    std::string_view label;
    std::string_view type;      // what may be typed before the name
    std::string_view name;      // what it declares
    std::string_view snippet;   // `$0` where the cursor is left
};

// Each declares `main`: a file has one, and a named module none ([basic.start.main]).
constexpr std::array<Template, 2> TEMPLATES { {
    { "auto main() -> int { }", "auto", "main", "auto main() -> int {\n\t$0\n}" },
    { "int main() { }", "int", "main", "int main() {\n\t$0\n}" },
} };

// The declaration typed so far on a line: the word being typed, alone or after a type and blanks.
struct Typed {
    std::string_view type;
    std::string_view word;
    std::size_t wordAt { 0 };   // where the word begins in what was typed
};

bool is_word(std::string_view text) { return std::ranges::all_of(text, base::is_identifier_char); }

std::optional<Typed> read_typed(std::string_view head) {
    const std::size_t blank { head.find_first_of(BLANKS) };
    if (blank == std::string_view::npos) return is_word(head) ? std::optional { Typed { {}, head, 0 } } : std::nullopt;
    const std::size_t word { head.find_first_not_of(BLANKS, blank) };
    if (word == std::string_view::npos) return std::nullopt;
    const Typed typed { head.substr(0, blank), head.substr(word), word };
    if (!is_word(typed.type) || !is_word(typed.word)) return std::nullopt;
    return typed;
}

// At least one letter of the name, after the template's own type or none.
bool fits(const Template& entry, const Typed& typed) {
    return !typed.word.empty() && entry.name.starts_with(typed.word) && (typed.type.empty() || typed.type == entry.type);
}

// The file declares `name` on a line other than `line`, where it is being typed.
bool declared_elsewhere(std::string_view text, std::string_view name, int line) {
    return std::ranges::any_of(orchestrator::outline::declarations(text, name),
                               [&](const orchestrator::outline::Place& place) { return place.line != line; });
}

} // namespace

Json template_completion(std::string_view text, base::Position position, const project::ScanResult* scan, TemplateOptions options) {
    Json items = Json::array();
    const auto offset = base::offset_at(text, position);
    if (!offset) return items;
    const std::size_t newline { *offset == 0 ? std::string_view::npos : text.rfind('\n', *offset - 1) };
    const std::size_t lineStart { newline == std::string_view::npos ? 0 : newline + 1 };
    // Nothing after the cursor on its line: what was typed is all of the declaration so far.
    const std::size_t lineEnd { std::min(text.find('\n', *offset), text.size()) };
    if (text.substr(*offset, lineEnd - *offset).find_first_not_of(BLANKS) != std::string_view::npos) return items;
    const std::size_t headStart { std::min(text.find_first_not_of(BLANKS, lineStart), *offset) };
    const std::string_view head { text.substr(headStart, *offset - headStart) };
    const auto typed = read_typed(head);
    if (!typed || std::ranges::none_of(TEMPLATES, [&](const Template& entry) { return fits(entry, *typed); })) return items;
    // A named module unit: after its declaration `main` would be attached to the module, and before
    // it only the global module fragment's preprocessing directives can appear.
    if (scan != nullptr && scan->declaration) return items;
    // File scope, and the declaration begins on this line: the code before the line ends one (`;`, `}`), or there is none.
    const Before before { read_before(text.substr(0, lineStart)) };
    if (before.inComment || before.continued || before.depth != 0) return items;
    if (before.last != 0 && before.last != ';' && before.last != '}') return items;

    const Json range { { "start", Json { { "line", position.line }, { "character", base::position_at(text, headStart).character } } },
                       { "end", Json { { "line", position.line }, { "character", position.character } } } };
    std::string_view checked;
    bool declared { false };
    for (const auto& entry : TEMPLATES) {
        if (!fits(entry, *typed)) continue;
        if (entry.name != checked) {
            checked = entry.name;
            declared = declared_elsewhere(text, entry.name, position.line);
        }
        if (declared) continue;
        // What it replaces, with the name in full: a client filters by the text from the edit's start. `~`
        // sorts after clangd's sortText, a score in hex digits and the name.
        const std::string filter { std::string { head.substr(0, typed->wordAt) } + std::string { entry.name } };
        // Plain, it is the template as written, on one line: a body with nothing in it to leave the cursor in.
        const std::string newText { options.snippets ? entry.snippet : entry.label };
        items.push_back(Json { { "label", std::string { entry.label } }, { "kind", COMPLETION_KIND_SNIPPET }, { "detail", std::string { DETAIL } },
                               { "filterText", filter }, { "sortText", "~" + std::string { entry.label } },
                               { "insertTextFormat", options.snippets ? INSERT_TEXT_SNIPPET : INSERT_TEXT_PLAIN },
                               { "textEdit", Json { { "range", range }, { "newText", newText } } } });
    }
    return items;
}

} // namespace mcppls::index
