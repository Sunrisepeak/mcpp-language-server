module mcppls.engine.native.keywords;

import std;
import nlohmann.json;
import mcppls.base.text;
import mcppls.project.scan;

namespace mcppls::index {

using Json = nlohmann::json;

namespace {

constexpr int COMPLETION_KIND_KEYWORD { 14 };

bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r'; }
bool is_word_char(char c) { return base::is_identifier_char(c); }

struct Keyword {
    std::string_view label;
    std::string_view insert;
    std::string_view detail;
    bool opensModules;
};

constexpr std::array<Keyword, 6> KEYWORDS { {
    { "import", "import ", "import a module", true },
    { "export import", "export import ", "import a module and export it", true },
    { "module;", "module;", "start the global module fragment", false },
    { "export module", "export module ", "declare a module interface unit", false },
    { "module", "module ", "declare a module implementation unit", false },
    { "module :private;", "module :private;", "start the private module fragment", false },
} };

// What the text before a line says about it: whether it is at global scope, and whether anything
// but comments and blanks comes before it. Braces are counted outside comments, string and
// character literals, and preprocessor lines (a macro's braces are not the file's).
struct Before {
    int depth { 0 };
    bool inComment { false };   // the line starts inside a block comment
    bool anything { false };    // a token or a preprocessor line comes before the line
};

Before read_before(std::string_view text) {
    Before before;
    bool lineStart { true };
    for (std::size_t i { 0 }; i < text.size(); ++i) {
        const char c { text[i] };
        if (c == '\n') {
            lineStart = true;
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            const std::size_t end { text.find('\n', i) };
            if (end == std::string_view::npos) break;
            i = end - 1;
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            const std::size_t end { text.find("*/", i + 2) };
            if (end == std::string_view::npos) {
                before.inComment = true;
                break;
            }
            i = end + 1;
            continue;
        }
        if (is_blank(c)) continue;
        before.anything = true;
        if (c == '#' && lineStart) {
            // A preprocessor line, continuation lines included.
            std::size_t end { i };
            while (true) {
                end = text.find('\n', end);
                if (end == std::string_view::npos || end == 0) break;
                std::size_t back { end - 1 };
                if (text[back] == '\r' && back > 0) --back;
                if (text[back] != '\\') break;
                ++end;
            }
            if (end == std::string_view::npos) break;
            i = end - 1;
            continue;
        }
        lineStart = false;
        if (c == 'R' && i + 1 < text.size() && text[i + 1] == '"' && (i == 0 || !is_word_char(text[i - 1]) || text[i - 1] == '8'
                                                                    || text[i - 1] == 'u' || text[i - 1] == 'U' || text[i - 1] == 'L')) {
            // A raw string literal: R"delimiter( ... )delimiter"
            const std::size_t open { text.find('(', i + 2) };
            if (open == std::string_view::npos) break;
            const std::string closing { ")" + std::string { text.substr(i + 2, open - i - 2) } + "\"" };
            const std::size_t end { text.find(closing, open + 1) };
            if (end == std::string_view::npos) break;
            i = end + closing.size() - 1;
            continue;
        }
        if (c == '"' || c == '\'') {
            // A digit separator (1'000) is no character literal.
            if (c == '\'' && i > 0 && std::isxdigit(static_cast<unsigned char>(text[i - 1]))) continue;
            std::size_t j { i + 1 };
            while (j < text.size() && text[j] != c && text[j] != '\n') j += text[j] == '\\' ? 2 : 1;
            i = std::min(j, text.size() - 1);
            continue;
        }
        if (c == '{') ++before.depth;
        else if (c == '}' && before.depth > 0) --before.depth;
    }
    return before;
}

bool has_private_fragment(std::string_view text) {
    return text.find("module :private") != std::string_view::npos || text.find("module:private") != std::string_view::npos;
}

} // namespace

Json keyword_completion(std::string_view text, base::Position position, const project::ScanResult* scan, KeywordOptions options) {
    const auto offset = base::offset_at(text, position);
    if (!offset) return nullptr;
    const std::size_t newline { *offset == 0 ? std::string_view::npos : text.rfind('\n', *offset - 1) };
    const std::size_t lineStart { newline == std::string_view::npos ? 0 : newline + 1 };
    // The cursor is at the end of what was typed, not inside a word.
    if (*offset < text.size() && is_word_char(text[*offset])) return nullptr;
    std::size_t headStart { lineStart };
    while (headStart < *offset && is_blank(text[headStart])) ++headStart;
    const std::string_view head { text.substr(headStart, *offset - headStart) };
    // A partial word, or `export`, blanks and a partial word; `export` alone is a partial word too.
    std::string typed { head };
    if (!std::ranges::all_of(head, is_word_char)) {
        if (!head.starts_with("export") || head.size() == 6 || !is_blank(head[6])) return nullptr;
        std::string_view word { head.substr(6) };
        while (!word.empty() && is_blank(word.front())) word.remove_prefix(1);
        if (!std::ranges::all_of(word, is_word_char)) return nullptr;
        typed = "export " + std::string { word };
    }
    if (std::ranges::none_of(KEYWORDS, [&](const Keyword& keyword) { return keyword.label.starts_with(typed); })) return nullptr;
    const Before before { read_before(text.substr(0, lineStart)) };
    if (before.inComment || before.depth != 0) return nullptr;

    const auto& declaration = scan != nullptr ? scan->declaration : std::optional<project::ModuleDeclaration> {};
    const bool declaredBefore { declaration && declaration->nameRange.start.line < position.line };
    const bool declaredElsewhere { declaration && declaration->nameRange.start.line != position.line };
    const bool interfaceUnit { declaredBefore && declaration->isExported };
    const auto applies = [&](const Keyword& keyword) {
        if (keyword.label == "import") return true;
        if (keyword.label == "export import") return interfaceUnit;
        if (keyword.label == "module;") return !before.anything;
        if (keyword.label == "export module" || keyword.label == "module") return !declaredElsewhere;
        if (keyword.label == "module :private;") return interfaceUnit && declaration->partition.empty() && !has_private_fragment(text);
        return false;
    };
    const Json range { { "start", Json { { "line", position.line }, { "character", base::position_at(text, headStart).character } } },
                       { "end", Json { { "line", position.line }, { "character", position.character } } } };
    Json items = Json::array();
    for (const auto& keyword : KEYWORDS) {
        if (!keyword.label.starts_with(typed) || !applies(keyword)) continue;
        Json item { { "label", std::string { keyword.label } }, { "kind", COMPLETION_KIND_KEYWORD }, { "detail", std::string { keyword.detail } },
                    { "filterText", std::string { keyword.label } }, { "insertTextFormat", 1 },
                    { "textEdit", Json { { "range", range }, { "newText", std::string { keyword.insert } } } } };
        if (keyword.opensModules && options.suggestModulesAfterImport) {
            item["command"] = Json { { "title", "Suggest modules" }, { "command", "editor.action.triggerSuggest" } };
        }
        items.push_back(std::move(item));
    }
    return items;
}

} // namespace mcppls::index
