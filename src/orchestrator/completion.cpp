module mcppls.orchestrator.completion;

import std;
import nlohmann.json;
import mcppls.base.text;
import mcppls.lsp.jsonrpc;

namespace mcppls::orchestrator::completion {

namespace {

bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f'; }

std::size_t skip_blanks(std::string_view& text) {
    std::size_t skipped { 0 };
    while (skipped < text.size() && is_blank(text[skipped])) ++skipped;
    text.remove_prefix(skipped);
    return skipped;
}

std::string lowercase(std::string_view text) {
    std::string lowered { text };
    std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered;
}

} // namespace

bool is_import_line_prefix(std::string_view linePrefix) {
    std::string_view rest { linePrefix };
    skip_blanks(rest);
    if (rest.starts_with("export")) {
        std::string_view afterExport { rest.substr(6) };
        // `export` and `import` are two words: `exportimport ` is neither.
        if (skip_blanks(afterExport) > 0) rest = afterExport;
    }
    return rest.size() == 7 && rest.starts_with("import") && is_blank(rest[6]);
}

std::optional<std::string_view> line_prefix(std::string_view text, base::Position position) {
    const auto offset = base::offset_at(text, position);
    if (!offset) return std::nullopt;
    const std::size_t newline { *offset == 0 ? std::string_view::npos : text.rfind('\n', *offset - 1) };
    const std::size_t start { newline == std::string_view::npos ? 0 : newline + 1 };
    return text.substr(start, *offset - start);
}

bool is_space_trigger(const Json& params) {
    const Json* context { lsp::find(params, "context") };
    if (context == nullptr || !context->is_object()) return false;
    return lsp::int_at(*context, "triggerKind").value_or(0) == 2 && context->value("triggerCharacter", std::string {}) == " ";
}

bool vscode_like(const Json& clientParams) {
    const Json* info { lsp::find(clientParams, "clientInfo") };
    if (info == nullptr || !info->is_object()) return false;
    const std::string name { lowercase(info->value("name", std::string {})) };
    if (name.starts_with("visual studio code")) return true;   // and "- Insiders"
    static constexpr std::array<std::string_view, 6> FORKS { "code - oss", "vscodium", "cursor", "windsurf", "trae", "positron" };
    return std::ranges::find(FORKS, name) != FORKS.end();
}

bool space_trigger_wanted(const Json& clientParams) {
    if (const Json* option = lsp::find_path(clientParams, { "initializationOptions", "completion", "triggerOnSpace" }); option != nullptr && option->is_boolean()) {
        return option->get<bool>();
    }
    return vscode_like(clientParams);
}

void add_space_trigger(Json& capabilities) {
    if (!capabilities.is_object()) return;
    Json& provider = capabilities["completionProvider"];
    if (!provider.is_object()) provider = Json::object();
    Json& triggers = provider["triggerCharacters"];
    if (!triggers.is_array()) triggers = Json::array();
    if (std::ranges::find(triggers, Json(" ")) == triggers.end()) triggers.push_back(" ");
}

Json empty_list() { return Json { { "isIncomplete", false }, { "items", Json::array() } }; }

Json merge(const Json& engineResult, const Json& keywordItems) {
    if (!keywordItems.is_array() || keywordItems.empty()) return engineResult;
    Json list = engineResult.is_object() ? engineResult : Json { { "isIncomplete", false }, { "items", engineResult.is_array() ? engineResult : Json::array() } };
    if (!list.contains("items") || !list["items"].is_array()) list["items"] = Json::array();
    if (!list.contains("isIncomplete") || !list["isIncomplete"].is_boolean()) list["isIncomplete"] = false;
    Json& items = list["items"];
    std::set<std::string, std::less<>> labels;
    for (const auto& item : items) {
        if (item.is_object()) labels.emplace(base::trim(item.value("label", std::string {})));
    }
    for (const auto& keyword : keywordItems) {
        if (labels.emplace(base::trim(keyword.value("label", std::string {}))).second) items.push_back(keyword);
    }
    return list;
}

Json keywords_only(const Json& keywordItems) {
    return Json { { "isIncomplete", true }, { "items", keywordItems.is_array() ? keywordItems : Json::array() } };
}

Json document_words(std::string_view text, base::Position position, std::size_t limit) {
    Json items = Json::array();
    const auto offset = base::offset_at(text, position);
    if (!offset) return items;
    std::size_t start { *offset };
    while (start > 0 && base::is_identifier_char(text[start - 1])) --start;
    const std::string_view typed { text.substr(start, *offset - start) };
    if (typed.empty() || std::isdigit(static_cast<unsigned char>(typed.front())) != 0) return items;
    std::size_t before { start };
    while (before > 0 && is_blank(text[before - 1])) --before;
    const std::string_view lead { text.substr(before >= 2 ? before - 2 : 0, before >= 2 ? 2 : before) };
    if (lead.ends_with('.') || lead == "->" || lead == "::") return items;

    const auto folded = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    std::map<std::string_view, std::size_t, std::less<>> nearest;   // word -> its distance from the cursor
    for (std::size_t i { 0 }; i < text.size();) {
        const char c { text[i] };
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            i = std::min(text.find('\n', i), text.size());
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            const std::size_t end { text.find("*/", i + 2) };
            i = end == std::string_view::npos ? text.size() : end + 2;
        } else if (c == '"' || c == '\'') {
            std::size_t j { i + 1 };
            while (j < text.size() && text[j] != c && text[j] != '\n') j += text[j] == '\\' ? 2 : 1;
            i = j + 1;
        } else if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            while (i < text.size() && (base::is_identifier_char(text[i]) || text[i] == '.' || text[i] == '\'')) ++i;   // a number, suffix and all
        } else if (base::is_identifier_char(c)) {
            std::size_t j { i };
            while (j < text.size() && base::is_identifier_char(text[j])) ++j;
            const std::string_view word { text.substr(i, j - i) };
            // A raw string literal: R"delimiter( ... )delimiter", with any encoding prefix.
            // A delimiter is at most 16 characters, none of them a blank, a parenthesis or a backslash: anything else is
            // not a raw string (a half-typed one reads as the identifier and a literal).
            const std::size_t open { j < text.size() && text[j] == '"' ? text.find('(', j) : std::string_view::npos };
            const std::string_view delimiter { open == std::string_view::npos ? std::string_view {} : text.substr(j + 1, open - j - 1) };
            if (open != std::string_view::npos && (word == "R" || word == "LR" || word == "uR" || word == "UR" || word == "u8R") && delimiter.size() <= 16
                && delimiter.find_first_of(" \t\n\\()") == std::string_view::npos) {
                const std::string closing { std::format("){}\"", delimiter) };
                const std::size_t end { text.find(closing, open) };
                i = end == std::string_view::npos ? text.size() : end + closing.size();
                continue;
            }
            if (i != start && word != typed && folded(word.front()) == folded(typed.front())) {
                const std::size_t distance { i < start ? start - i : i - start };
                if (const auto [it, added] = nearest.try_emplace(word, distance); !added) it->second = std::min(it->second, distance);
            }
            i = j;
        } else {
            ++i;
        }
    }
    std::vector<std::pair<std::size_t, std::string_view>> ranked;
    for (const auto& [word, distance] : nearest) ranked.emplace_back(distance, word);
    std::ranges::sort(ranked);
    for (const auto& [distance, word] : ranked | std::views::take(limit)) {
        items.push_back(Json { { "label", std::string { word } }, { "kind", 1 }, { "sortText", std::format("{:06}", items.size()) } });
    }
    return items;
}

bool is_empty(const Json& result) {
    if (result.is_null()) return true;
    if (result.is_array()) return result.empty();
    if (!result.is_object()) return false;
    const Json* items { lsp::find(result, "items") };
    return items == nullptr || !items->is_array() || items->empty();
}

bool in_import_directive(std::string_view linePrefix) {
    std::string_view rest { linePrefix };
    skip_blanks(rest);
    if (rest.starts_with("export")) {
        std::string_view afterExport { rest.substr(6) };
        if (skip_blanks(afterExport) > 0) rest = afterExport;
    }
    return rest.starts_with("import") && (rest.size() == 6 || is_blank(rest[6]) || rest[6] == '<' || rest[6] == '"' || rest[6] == ':');
}

Json without_engine(const Json& wordItems) {
    return Json { { "isIncomplete", true }, { "items", wordItems.is_array() ? wordItems : Json::array() } };
}

std::optional<WordKey> word_key(std::string_view text, base::Position position) {
    const auto offset = base::offset_at(text, position);
    if (!offset) return std::nullopt;
    std::size_t start { *offset };
    while (start > 0 && base::is_identifier_char(text[start - 1])) --start;
    const std::size_t lineStart { start == 0 ? 0 : text.rfind('\n', start - 1) + 1 };   // npos + 1 is 0
    return WordKey { position.line, std::string { text.substr(lineStart, start - lineStart) }, std::string { text.substr(start, *offset - start) } };
}

bool typed_on(const WordKey& earlier, const WordKey& later) {
    return earlier.line == later.line && earlier.before == later.before && later.typed.starts_with(earlier.typed);
}

namespace {

// A range on `position`'s line that starts at or before it, ended at it; false when it cannot be.
bool end_range_at(Json& range, base::Position position) {
    if (!range.is_object() || !range.contains("start") || !range.contains("end")) return false;
    const Json& start { range["start"] };
    if (start.value("line", -1) != position.line || range["end"].value("line", -1) != position.line) return false;
    if (start.value("character", -1) > position.character) return false;
    range["end"] = Json { { "line", position.line }, { "character", position.character } };
    return true;
}

} // namespace

Json retarget(const Json& result, base::Position position) {
    const bool list { result.is_object() };
    Json out { { "isIncomplete", list && result.value("isIncomplete", false) }, { "items", Json::array() } };
    const Json* items { list ? lsp::find(result, "items") : &result };
    if (items == nullptr || !items->is_array()) return out;
    for (Json item : *items) {
        if (item.contains("textEdit") && item["textEdit"].is_object()) {
            Json& edit { item["textEdit"] };
            // TextEdit has a range; InsertReplaceEdit an insert and a replace range, both starting at the word.
            const bool kept { edit.contains("range") ? end_range_at(edit["range"], position)
                                                     : end_range_at(edit["insert"], position) && end_range_at(edit["replace"], position) };
            if (!kept) continue;
        }
        out["items"].push_back(std::move(item));
    }
    return out;
}

void EnginePace::answered(std::chrono::steady_clock::time_point at, std::chrono::milliseconds latency,
                          std::chrono::milliseconds cap) {
    if (latency > cap - MARGIN) return;   // beyond any budget this file could earn: the engine is busy, not steady
    recent_.push_back(Answer { at, latency });
    if (recent_.size() > KEEP) recent_.pop_front();
}

std::chrono::milliseconds EnginePace::budget(std::chrono::steady_clock::time_point now, std::chrono::milliseconds base,
                                             std::chrono::milliseconds cap) const {
    const auto window { std::chrono::duration_cast<std::chrono::steady_clock::duration>(WINDOW) };
    std::size_t fresh { 0 };
    std::chrono::milliseconds slowest { 0 };
    std::chrono::milliseconds fastest { std::chrono::milliseconds::max() };
    for (const auto& answer : recent_) {
        if (now - answer.at > window) continue;
        ++fresh;
        slowest = std::max(slowest, answer.latency);
        fastest = std::min(fastest, answer.latency);
    }
    if (fresh < 2 || fastest <= base) return base;
    return std::min(cap, slowest + MARGIN);
}

} // namespace mcppls::orchestrator::completion
