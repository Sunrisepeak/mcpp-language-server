module mcppls.orchestrator.answer;

import std;
import nlohmann.json;
import mcppls.base.text;

namespace mcppls::orchestrator::answer {

namespace {

bool identifier_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }
bool blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

std::uint64_t mix(std::uint64_t seed, std::string_view text) {
    const std::uint64_t value { std::hash<std::string_view> {}(text) };
    return seed ^ (value + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2));
}

std::string lower(std::string_view text) {
    std::string out { text };
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// The text with comments, string and character literals blanked out (same length, newlines kept): what the scanners
// below read, so that a brace or a `.` in a comment or a literal is not taken for code.
std::string code_only(std::string_view text, std::size_t upTo) {
    std::string out { text.substr(0, std::min(upTo, text.size())) };
    for (std::size_t i { 0 }; i < out.size();) {
        if (out[i] == '/' && i + 1 < out.size() && out[i + 1] == '/') {
            while (i < out.size() && out[i] != '\n') out[i++] = ' ';
        } else if (out[i] == '/' && i + 1 < out.size() && out[i + 1] == '*') {
            while (i < out.size() && !(out[i] == '*' && i + 1 < out.size() && out[i + 1] == '/')) {
                if (out[i] != '\n') out[i] = ' ';
                ++i;
            }
            for (std::size_t k { 0 }; k < 2 && i < out.size(); ++k) out[i++] = ' ';
        } else if (out[i] == '"' || (out[i] == '\'' && !(i > 0 && identifier_char(out[i - 1]) && std::isdigit(static_cast<unsigned char>(out[i - 1])) != 0))) {
            const char quote { out[i] };
            out[i++] = ' ';
            while (i < out.size() && out[i] != quote && out[i] != '\n') {
                if (out[i] == '\\' && i + 1 < out.size()) out[i++] = ' ';
                out[i++] = ' ';
            }
            if (i < out.size() && out[i] == quote) out[i++] = ' ';
        } else {
            ++i;
        }
    }
    return out;
}

// From `end` back over blanks; the index of the last code character before them, or npos.
std::size_t back_over_blanks(std::string_view code, std::size_t end) {
    while (end > 0 && blank(code[end - 1])) --end;
    return end == 0 ? std::string_view::npos : end - 1;
}

// The expression that ends just before `end`: identifiers joined by `::`, `.`, `->`, with balanced `()`, `[]` and
// `<>` groups -- `a.b()`, `std::views`, `m_items[0]`. Blanks dropped.
std::string chain_before(std::string_view code, std::size_t end) {
    std::size_t i { end };
    std::string out;
    while (i > 0) {
        std::size_t last { back_over_blanks(code, i) };
        if (last == std::string_view::npos) break;
        const char c { code[last] };
        if (identifier_char(c)) {
            std::size_t start { last };
            while (start > 0 && identifier_char(code[start - 1])) --start;
            out.insert(0, code.substr(start, last + 1 - start));
            i = start;
        } else if (c == ')' || c == ']' || c == '>') {
            const char open { c == ')' ? '(' : c == ']' ? '[' : '<' };
            int depth { 0 };
            std::size_t j { last + 1 };
            while (j > 0) {
                --j;
                if (code[j] == c) ++depth;
                else if (code[j] == open && --depth == 0) break;
            }
            if (depth != 0) break;
            std::string group;
            for (const char g : code.substr(j, last + 1 - j)) {
                if (!blank(g)) group.push_back(g);
            }
            out.insert(0, group);
            i = j;
            continue;
        } else {
            break;
        }
        // Joined by `::`, `.` or `->`? Then the chain goes on.
        std::size_t joint { back_over_blanks(code, i) };
        if (joint == std::string_view::npos) break;
        if (code[joint] == '.') {
            out.insert(0, ".");
            i = joint;
        } else if (joint > 0 && code[joint] == ':' && code[joint - 1] == ':') {
            out.insert(0, "::");
            i = joint - 1;
        } else if (joint > 0 && code[joint] == '>' && code[joint - 1] == '-') {
            out.insert(0, "->");
            i = joint - 1;
        } else {
            break;
        }
    }
    return out;
}

bool keyword_head(std::string_view name) {
    static constexpr std::array<std::string_view, 12> BLOCKS { "if", "for", "while", "switch", "do", "else", "try", "catch", "return", "case", "default", "requires" };
    return std::ranges::find(BLOCKS, name) != BLOCKS.end();
}

// The innermost declaration enclosing `offset` -- a function, a class, a namespace -- as "name@line", and where its
// body starts; blocks (if, for, lambdas, initializers) are looked through.
std::pair<std::string, std::size_t> enclosing_of(std::string_view code, std::size_t offset) {
    struct Open {
        std::string name;   // empty: a block
        std::size_t at;
    };
    std::vector<Open> open;
    std::size_t statement { 0 };
    for (std::size_t i { 0 }; i < offset && i < code.size(); ++i) {
        const char c { code[i] };
        if (c == ';') {
            statement = i + 1;
        } else if (c == '}') {
            if (!open.empty()) open.pop_back();
            statement = i + 1;
        } else if (c == '{') {
            const std::string_view head { base::trim(code.substr(statement, i - statement)) };
            std::string name;
            const std::size_t paren { head.find('(') };
            const auto word_before = [&](std::size_t end) {
                while (end > 0 && blank(head[end - 1])) --end;
                std::size_t start { end };
                while (start > 0 && identifier_char(head[start - 1])) --start;
                return std::string { head.substr(start, end - start) };
            };
            if (paren != std::string_view::npos && head.find("](") == std::string_view::npos && head.find("] (") == std::string_view::npos) {
                name = word_before(paren);   // `R f(...) const {`, `C::C(...) : m() {`
                if (keyword_head(name)) name.clear();
            } else {
                for (std::string_view key : { std::string_view { "namespace" }, std::string_view { "class" }, std::string_view { "struct" },
                                              std::string_view { "union" }, std::string_view { "enum" } }) {
                    const std::size_t at { head.find(key) };
                    if (at == std::string_view::npos || (at > 0 && identifier_char(head[at - 1]))) continue;
                    std::size_t start { at + key.size() };
                    while (start < head.size() && blank(head[start])) ++start;
                    std::size_t end { start };
                    while (end < head.size() && (identifier_char(head[end]) || head[end] == ':')) ++end;
                    name = std::format("{} {}", key, head.substr(start, end - start));
                    break;
                }
            }
            open.push_back(Open { std::move(name), i });
            statement = i + 1;
        }
    }
    for (auto it = open.rbegin(); it != open.rend(); ++it) {
        if (it->name.empty()) continue;
        const std::size_t line { static_cast<std::size_t>(std::count(code.begin(), code.begin() + static_cast<std::ptrdiff_t>(it->at), '\n')) };
        return { std::format("{}@{}", it->name, line), it->at };
    }
    return { std::string {}, 0 };
}

const Json* items_of(const Json& result) {
    if (result.is_array()) return &result;
    if (result.is_object()) {
        const auto it = result.find("items");
        if (it != result.end() && it->is_array()) return &*it;
    }
    return nullptr;
}

std::string label_of(const Json& item) {
    std::string name { item.value("filterText", std::string {}) };
    if (name.empty()) name = item.value("label", std::string {});
    // `push_back(…)`, `• views`: the name a person types.
    std::size_t start { 0 };
    while (start < name.size() && !identifier_char(name[start]) && name[start] != '~') ++start;
    std::size_t end { start };
    while (end < name.size() && (identifier_char(name[end]) || name[end] == '~')) ++end;
    return name.substr(start, end - start);
}

// The client's own filter, roughly: the word typed is a case-insensitive subsequence of the item's name, starting at
// its first character.
bool matches(std::string_view name, std::string_view typed) {
    if (typed.empty()) return true;
    if (name.empty() || std::tolower(static_cast<unsigned char>(name.front())) != std::tolower(static_cast<unsigned char>(typed.front()))) return false;
    std::size_t at { 0 };
    for (const char c : typed) {
        const auto found = std::find_if(name.begin() + static_cast<std::ptrdiff_t>(at), name.end(), [&](char n) {
            return std::tolower(static_cast<unsigned char>(n)) == std::tolower(static_cast<unsigned char>(c));
        });
        if (found == name.end()) return false;
        at = static_cast<std::size_t>(found - name.begin()) + 1;
    }
    return true;
}

Json position_json(base::Position position) { return Json { { "line", position.line }, { "character", position.character } }; }

void mark_doubt(Json& item, Marks marks, std::string_view source) {
    if (marks == Marks::off) return;
    Json& details = item["labelDetails"];
    if (!details.is_object()) details = Json::object();
    const std::string description { details.value("description", std::string {}) };
    details["description"] = description.empty() ? std::string { "·" } : std::format("{} ·", description);
    const std::string note { marks == Marks::verbose ? std::format("_mcppls: {} answer, being checked_", source) : std::string { "_from an earlier answer, being checked_" } };
    Json& documentation = item["documentation"];
    if (documentation.is_string()) {
        documentation = Json { { "kind", "markdown" }, { "value", std::format("{}\n\n{}", documentation.get<std::string>(), note) } };
    } else if (documentation.is_object()) {
        documentation["kind"] = "markdown";
        documentation["value"] = std::format("{}\n\n{}", documentation.value("value", std::string {}), note);
    } else {
        documentation = Json { { "kind", "markdown" }, { "value", note } };
    }
}

std::string clipped(std::string text, std::size_t limit) {
    if (text.size() > limit) {
        text.resize(limit);
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) text.pop_back();   // not mid-character
        if (!text.empty()) text.pop_back();
        text += "…";
    }
    return text;
}

Json clipped_documentation(const Json& documentation, std::size_t limit) {
    if (documentation.is_string()) return clipped(documentation.get<std::string>(), limit);
    if (documentation.is_object() && documentation.contains("value") && documentation["value"].is_string()) {
        Json out = documentation;
        out["value"] = clipped(documentation["value"].get<std::string>(), limit);
        return out;
    }
    return Json();
}

} // namespace

// ---- what is asked ------------------------------------------------------------------------------------------------

std::optional<Kind> kind_of(std::string_view method) {
    if (method == "textDocument/completion") return Kind::completion;
    if (method == "textDocument/signatureHelp") return Kind::signature_help;
    if (method == "textDocument/hover") return Kind::hover;
    if (method == "textDocument/definition") return Kind::definition;
    if (method == "textDocument/declaration") return Kind::declaration;
    if (method == "textDocument/typeDefinition") return Kind::type_definition;
    if (method == "textDocument/implementation") return Kind::implementation;
    return std::nullopt;
}

std::string_view name_of(Kind kind) {
    switch (kind) {
    case Kind::completion: return "completion";
    case Kind::signature_help: return "signature";
    case Kind::hover: return "hover";
    case Kind::definition: return "definition";
    case Kind::declaration: return "declaration";
    case Kind::type_definition: return "typeDefinition";
    case Kind::implementation: return "implementation";
    }
    return "?";
}

bool is_jump(Kind kind) {
    return kind == Kind::definition || kind == Kind::declaration || kind == Kind::type_definition || kind == Kind::implementation;
}

std::string_view name_of(Context::Place place) {
    switch (place) {
    case Context::Place::member: return "member";
    case Context::Place::arrow: return "arrow";
    case Context::Place::qualified: return "qualified";
    case Context::Place::unqualified: return "unqualified";
    case Context::Place::call: return "call";
    case Context::Place::other: return "other";
    }
    return "?";
}

std::optional<Context> context_at(std::string_view text, base::Position position, Kind kind) {
    const auto offset = base::offset_at(text, position);
    if (!offset) return std::nullopt;
    // The word: typed up to the cursor for a completion, the whole identifier under it otherwise.
    std::size_t start { *offset };
    while (start > 0 && identifier_char(text[start - 1])) --start;
    std::size_t end { *offset };
    if (kind != Kind::completion && kind != Kind::signature_help) {
        while (end < text.size() && identifier_char(text[end])) ++end;
        if (end == start) return std::nullopt;   // hover and jumps are about an identifier
    }
    const std::size_t scanTo { std::min(text.size(), end + 1) };
    const std::string code { code_only(text, scanTo) };
    // Inside a comment or a literal: nothing to answer from what is kept.
    for (std::size_t i { start }; i < std::min(end, code.size()); ++i) {
        if (code[i] == ' ' && !blank(text[i])) return std::nullopt;
    }
    if (*offset > 0 && *offset <= code.size() && code[*offset - 1] == ' ' && !blank(text[*offset - 1])) return std::nullopt;
    Context context;
    context.word = std::string { text.substr(start, end - start) };
    context.wordStart = start;
    context.wordEnd = end;
    const std::string_view view { code };
    if (kind == Kind::signature_help) {
        // The call: the innermost `(` not closed before the cursor, and what names the function before it.
        int depth { 0 };
        std::size_t i { std::min(*offset, view.size()) };
        while (i > 0) {
            --i;
            const char c { view[i] };
            if (c == ')' || c == ']' || c == '}') ++depth;
            else if ((c == '(' || c == '[' || c == '{') && depth > 0) --depth;
            else if (c == '(' && depth == 0) break;
            else if ((c == ';' || c == '{') && depth == 0) return std::nullopt;
        }
        if (i == 0 && (view.empty() || view[0] != '(')) return std::nullopt;
        // Which argument the cursor is in: the commas at the call's own depth.
        int nested { 0 };
        for (std::size_t j { i + 1 }; j < std::min(*offset, view.size()); ++j) {
            const char c { view[j] };
            if (c == '(' || c == '[' || c == '{' || c == '<') ++nested;
            else if ((c == ')' || c == ']' || c == '}' || c == '>') && nested > 0) --nested;
            else if (c == ',' && nested == 0) ++context.argument;
        }
        context.place = Context::Place::call;
        context.receiver = chain_before(view, i);
        if (context.receiver.empty()) return std::nullopt;
    } else {
        const std::size_t before { back_over_blanks(view, start) };
        if (before != std::string_view::npos && view[before] == '.') {
            context.place = Context::Place::member;
            context.receiver = chain_before(view, before);
        } else if (before != std::string_view::npos && before > 0 && view[before] == '>' && view[before - 1] == '-') {
            context.place = Context::Place::arrow;
            context.receiver = chain_before(view, before - 1);
        } else if (before != std::string_view::npos && before > 0 && view[before] == ':' && view[before - 1] == ':') {
            context.place = Context::Place::qualified;
            context.receiver = chain_before(view, before - 1);
            if (context.receiver.empty()) context.receiver = "::";   // the global namespace
        } else {
            context.place = Context::Place::unqualified;
        }
        if ((context.place == Context::Place::member || context.place == Context::Place::arrow) && context.receiver.empty()) return std::nullopt;
    }
    const auto [enclosing, bodyAt] = enclosing_of(view, start);
    context.enclosing = enclosing;
    context.localHash = mix(0, view.substr(bodyAt, start - std::min(bodyAt, start)));
    return context;
}

std::string expand_aliases(std::string_view text, std::string_view qualifier) {
    const std::size_t split { qualifier.find("::") };
    const std::string_view first { qualifier.substr(0, split) };
    if (first.empty()) return std::string { qualifier };
    // `namespace first = target;`
    const std::string code { code_only(text, text.size()) };
    std::size_t at { 0 };
    while ((at = code.find("namespace", at)) != std::string::npos) {
        std::size_t i { at + 9 };
        const bool word { (at == 0 || !identifier_char(code[at - 1])) && i < code.size() && blank(code[i]) };
        at = i;
        if (!word) continue;
        while (i < code.size() && blank(code[i])) ++i;
        std::size_t nameEnd { i };
        while (nameEnd < code.size() && identifier_char(code[nameEnd])) ++nameEnd;
        if (std::string_view { code }.substr(i, nameEnd - i) != first) continue;
        std::size_t eq { nameEnd };
        while (eq < code.size() && blank(code[eq])) ++eq;
        if (eq >= code.size() || code[eq] != '=') continue;
        const std::size_t semicolon { code.find(';', eq) };
        if (semicolon == std::string::npos) continue;
        std::string target;
        for (const char c : std::string_view { code }.substr(eq + 1, semicolon - eq - 1)) {
            if (!blank(c)) target.push_back(c);
        }
        if (target.starts_with("::")) target.erase(0, 2);
        if (target.empty()) continue;
        return split == std::string_view::npos ? target : target + std::string { qualifier.substr(split) };
    }
    return std::string { qualifier };
}

std::optional<std::string> declared_type(std::string_view text, std::size_t before, std::string_view name) {
    if (name.empty()) return std::nullopt;
    const std::string code { code_only(text, before) };
    std::size_t at { code.size() };
    while (at > 0) {
        at = code.rfind(name, at - 1);
        if (at == std::string::npos) return std::nullopt;
        const std::size_t end { at + name.size() };
        if ((at > 0 && identifier_char(code[at - 1])) || (end < code.size() && identifier_char(code[end]))) {
            if (at == 0) return std::nullopt;
            continue;
        }
        std::size_t next { end };
        while (next < code.size() && blank(code[next])) ++next;
        const char after { next < code.size() ? code[next] : ';' };
        if (after != '=' && after != ';' && after != '{' && after != ',' && after != ')' && after != '[') {
            if (at == 0) return std::nullopt;
            continue;
        }
        // The type: back to the start of the declaration.
        std::size_t begin { at };
        int angle { 0 };
        while (begin > 0) {
            const char c { code[begin - 1] };
            if (c == '>') ++angle;
            else if (c == '<' && angle > 0) --angle;
            else if (angle == 0 && (c == ';' || c == '{' || c == '}' || c == '(' || c == ',' || c == ')' || c == '=' || c == '.')) break;
            --begin;
        }
        std::string type;
        for (const char c : std::string_view { code }.substr(begin, at - begin)) {
            if (blank(c)) {
                if (!type.empty() && type.back() != ' ') type.push_back(' ');
            } else {
                type.push_back(c);
            }
        }
        // Specifiers and declarators are not the type.
        for (std::string_view drop : { "const ", "constexpr ", "static ", "inline ", "mutable ", "volatile ", "thread_local ", "extern " }) {
            for (std::size_t found; (found = type.find(drop)) != std::string::npos;) {
                if (found == 0 || type[found - 1] == ' ') type.erase(found, drop.size());
                else break;
            }
        }
        while (!type.empty() && (type.back() == ' ' || type.back() == '&' || type.back() == '*')) type.pop_back();
        while (!type.empty() && type.front() == ' ') type.erase(0, 1);
        if (type == "auto" || type.starts_with("auto ")) return std::nullopt;   // declared, and nothing to read the type from
        if (type.empty() || type.starts_with("decltype") || type.find_first_of("=.()[]{};") != std::string::npos || type == "return" || type == "case"
            || type == "else" || type == "delete" || type == "throw" || type == "co_return" || type == "co_yield") {
            if (at == 0) return std::nullopt;
            continue;   // a use: an earlier occurrence may be the declaration
        }
        return type;
    }
    return std::nullopt;
}

// ---- keys -----------------------------------------------------------------------------------------------------------

std::string k1(std::string_view uri, Kind kind, const Context& context) {
    std::string key { std::format("1|{}|{}|{}|{}|{}", uri, name_of(kind), context.enclosing, name_of(context.place), context.receiver) };
    if (kind != Kind::completion && kind != Kind::signature_help) key += "|" + context.word;
    return key;
}

std::optional<std::string> k3(Kind kind, const Context& context, std::string_view expandedQualifier) {
    if (context.place != Context::Place::qualified && !(kind == Kind::signature_help && context.receiver.find("::") != std::string::npos)) return std::nullopt;
    std::string key { std::format("3|{}|{}", name_of(kind), expandedQualifier) };
    if (kind != Kind::completion && kind != Kind::signature_help) key += "::" + context.word;
    return key;
}

std::string k2(Kind kind, Context::Place place, std::string_view typeKey) { return std::format("2|{}|{}|{}", name_of(kind), name_of(place), typeKey); }

std::string type_key(std::string_view name, std::string_view path, int line) { return std::format("{}@{}:{}", name, path, line); }

// ---- what is kept ---------------------------------------------------------------------------------------------------

std::string_view name_of(Source source) {
    switch (source) {
    case Source::engine: return "engine";
    case Source::k1: return "cache:K1";
    case Source::k2: return "cache:K2";
    case Source::k3: return "cache:K3";
    case Source::outline: return "outline";
    case Source::native: return "native";
    }
    return "?";
}

std::string_view name_of(Certainty certainty) {
    switch (certainty) {
    case Certainty::authoritative: return "authoritative";
    case Certainty::certain: return "certain";
    case Certainty::uncertain: return "uncertain";
    }
    return "?";
}

std::optional<Json> trim(Kind kind, const Json& result) {
    if (result.is_null()) return std::nullopt;
    switch (kind) {
    case Kind::completion: {
        const Json* items { items_of(result) };
        if (items == nullptr || items->empty()) return std::nullopt;
        Json kept = Json::array();
        for (const Json& item : *items) {
            if (!item.is_object()) continue;
            Json one = Json::object();
            for (const char* field : { "label", "kind", "detail", "labelDetails", "sortText", "filterText", "insertText", "insertTextFormat",
                                       "textEdit", "additionalTextEdits", "data", "tags", "deprecated", "score" }) {
                if (item.contains(field)) one[field] = item[field];
            }
            if (item.contains("documentation")) {
                if (Json documentation = clipped_documentation(item["documentation"], 512); !documentation.is_null()) one["documentation"] = std::move(documentation);
            }
            kept.push_back(std::move(one));
        }
        if (kept.empty()) return std::nullopt;
        return Json { { "items", std::move(kept) } };
    }
    case Kind::signature_help: {
        if (!result.is_object() || !result.contains("signatures") || !result["signatures"].is_array() || result["signatures"].empty()) return std::nullopt;
        Json kept = Json::object();
        kept["signatures"] = result["signatures"];
        for (Json& signature : kept["signatures"]) {
            if (signature.is_object() && signature.contains("documentation")) signature["documentation"] = clipped_documentation(signature["documentation"], 2048);
            if (signature.is_object()) signature.erase("activeParameter");
        }
        if (result.contains("activeSignature")) kept["activeSignature"] = result["activeSignature"];
        return kept;
    }
    case Kind::hover: {
        if (!result.is_object() || !result.contains("contents")) return std::nullopt;
        Json contents = result["contents"];
        if (contents.is_object() && contents.contains("value") && contents["value"].is_string()) {
            if (contents["value"].get<std::string>().empty()) return std::nullopt;
            contents["value"] = clipped(contents["value"].get<std::string>(), 16384);
        }
        return Json { { "contents", std::move(contents) } };
    }
    default:
        if (result.is_array() && result.empty()) return std::nullopt;
        return result;
    }
}

Store::Store(std::size_t maxBytes) : maxBytes_ { maxBytes } {}

const Entry* Store::find(const std::string& key) {
    const auto it = entries_.find(key);
    if (it == entries_.end()) return nullptr;
    order_.splice(order_.end(), order_, it->second.use);
    ++it->second.entry.hits;
    return &it->second.entry;
}

void Store::put(const std::string& key, Entry entry) {
    const std::size_t bytes { key.size() + entry.result.dump().size() + entry.fingerprint.size() + entry.provider.size() + entry.asked.size() + 96 };
    if (const auto it = entries_.find(key); it != entries_.end()) {
        bytes_ -= it->second.bytes;
        order_.erase(it->second.use);
        entries_.erase(it);
    }
    order_.push_back(key);
    entries_.emplace(key, Slot { std::move(entry), bytes, std::prev(order_.end()) });
    bytes_ += bytes;
    ++changes_;
    evict_();
}

void Store::erase(const std::string& key) {
    const auto it = entries_.find(key);
    if (it == entries_.end()) return;
    bytes_ -= it->second.bytes;
    order_.erase(it->second.use);
    entries_.erase(it);
    ++changes_;
}

void Store::evict_() {
    while (bytes_ > maxBytes_ && !order_.empty()) erase(order_.front());
}

Json Store::to_json(bool onlyNames) const {
    Json entries = Json::array();
    for (const std::string& key : order_) {
        if (onlyNames && !key.starts_with("3|")) continue;
        const Entry& entry { entries_.at(key).entry };
        entries.push_back(Json { { "key", key },
                                 { "kind", std::string { name_of(entry.kind) } },
                                 { "result", entry.result },
                                 { "fingerprint", entry.fingerprint },
                                 { "localHash", std::format("{:x}", entry.localHash) },
                                 { "provider", entry.provider },
                                 { "providerLine", entry.providerLine },
                                 { "providerGeneration", std::format("{:x}", entry.providerGeneration) },
                                 { "asked", entry.asked },
                                 { "keptAt", entry.keptAt },
                                 { "hits", entry.hits } });
    }
    return Json { { "format", FORMAT }, { "entries", std::move(entries) } };
}

bool Store::adopt(const Json& persisted) {
    if (!persisted.is_object() || persisted.value("format", 0) != FORMAT) return false;
    const auto entries = persisted.find("entries");
    if (entries == persisted.end() || !entries->is_array()) return false;
    const auto hex = [](const Json& value) -> std::uint64_t {
        if (!value.is_string()) return 0;
        std::uint64_t out { 0 };
        const std::string text { value.get<std::string>() };
        std::from_chars(text.data(), text.data() + text.size(), out, 16);
        return out;
    };
    for (const Json& one : *entries) {
        if (!one.is_object() || !one.contains("key") || !one["key"].is_string()) continue;
        const std::string key { one["key"].get<std::string>() };
        if (entries_.contains(key)) continue;   // this session's own is newer
        Entry entry;
        bool known { false };
        for (const Kind kind : { Kind::completion, Kind::signature_help, Kind::hover, Kind::definition, Kind::declaration, Kind::type_definition,
                                 Kind::implementation }) {
            if (one.value("kind", std::string {}) == name_of(kind)) {
                entry.kind = kind;
                known = true;
            }
        }
        if (!known) continue;
        entry.result = one.value("result", Json());
        entry.fingerprint = one.value("fingerprint", std::string {});
        entry.localHash = hex(one.value("localHash", Json()));
        entry.provider = one.value("provider", std::string {});
        entry.providerLine = one.value("providerLine", -1);
        entry.providerGeneration = hex(one.value("providerGeneration", Json()));
        entry.asked = one.value("asked", std::string {});
        entry.keptAt = one.value("keptAt", std::int64_t { 0 });
        entry.hits = one.value("hits", std::uint32_t { 0 });
        entry.others = ~std::uint64_t { 0 };   // the counts of another session: never this one's
        put(key, std::move(entry));
    }
    return true;
}

// ---- certainty ------------------------------------------------------------------------------------------------------

bool provider_changed(const Entry& entry, const Now& now) {
    return !entry.provider.empty() && now.generation && now.generation(entry.provider) != entry.providerGeneration;
}

Certainty certainty_of(const Entry& entry, Source source, const Now& now) {
    if (entry.fingerprint != now.fingerprint) return Certainty::uncertain;
    if (provider_changed(entry, now)) return Certainty::uncertain;
    switch (source) {
    case Source::k1:
        if (entry.localHash != now.localHash || entry.others != now.others) return Certainty::uncertain;
        return Certainty::certain;
    case Source::k2: return entry.provider.empty() ? Certainty::uncertain : Certainty::certain;
    case Source::k3: return entry.others == now.others ? Certainty::certain : Certainty::uncertain;
    default: return Certainty::uncertain;
    }
}

// ---- composing ------------------------------------------------------------------------------------------------------

std::optional<Marks> marks_of(std::string_view setting) {
    if (setting == "subtle") return Marks::subtle;
    if (setting == "off") return Marks::off;
    if (setting == "verbose") return Marks::verbose;
    return std::nullopt;
}

Composed compose_completion(const Entry& entry, Source source, Certainty certainty, const Context& context, std::string_view text,
                            base::Position cursor, Marks marks, const std::optional<std::vector<std::string>>& members, bool providerChanged) {
    Composed composed;
    Json items = Json::array();
    const Json* kept { items_of(entry.result) };
    const base::Position start { base::position_at(text, context.wordStart) };
    std::size_t wordEnd { context.wordStart };
    while (wordEnd < text.size() && identifier_char(text[wordEnd])) ++wordEnd;
    const base::Position replaceEnd { base::position_at(text, wordEnd) };
    std::vector<std::string> seen;
    std::vector<Json> gone;   // items the outline says the class no longer has: last
    const bool memberPlace { context.place == Context::Place::member || context.place == Context::Place::arrow };
    const bool check { memberPlace && members.has_value() };
    if (kept != nullptr) {
        for (Json item : *kept) {
            const std::string name { label_of(item) };
            if (!matches(name, context.word)) continue;
            seen.push_back(name);
            // Placed on the word as it is now.
            if (item.contains("textEdit") && item["textEdit"].is_object()) {
                Json& edit = item["textEdit"];
                if (edit.contains("insert")) {
                    edit["insert"] = Json { { "start", position_json(start) }, { "end", position_json(cursor) } };
                    edit["replace"] = Json { { "start", position_json(start) }, { "end", position_json(replaceEnd) } };
                } else {
                    edit["range"] = Json { { "start", position_json(start) }, { "end", position_json(cursor) } };
                }
            }
            // An edit elsewhere in the file it was asked in (an #include it would add) is for that file as it was.
            item.erase("additionalTextEdits");
            if (certainty != Certainty::certain) {
                item["preselect"] = false;
                item.erase("commitCharacters");
            }
            const bool goneFromClass { check && !name.empty() && std::ranges::find(*members, name) == members->end() };
            if (goneFromClass) {
                item["sortText"] = "~" + item.value("sortText", name);
                mark_doubt(item, marks, name_of(source));
                ++composed.doubted;
                gone.push_back(std::move(item));
                continue;
            }
            if (marks == Marks::verbose) mark_doubt(item, marks, name_of(source));
            else if (providerChanged && !check) {
                mark_doubt(item, marks, name_of(source));
                ++composed.doubted;
            }
            items.push_back(std::move(item));
        }
    }
    if (check) {
        // Members the class has now and the kept answer lacks: added since it was kept.
        for (const std::string& name : *members) {
            if (!matches(name, context.word) || std::ranges::find(seen, name) != seen.end()) continue;
            Json item { { "label", name }, { "kind", 5 }, { "sortText", "0" + name }, { "filterText", name }, { "preselect", false },
                        { "textEdit", Json { { "range", Json { { "start", position_json(start) }, { "end", position_json(cursor) } } }, { "newText", name } } } };
            mark_doubt(item, marks, name_of(Source::outline));
            ++composed.doubted;
            items.push_back(std::move(item));
        }
    }
    for (Json& item : gone) items.push_back(std::move(item));
    composed.result = Json { { "isIncomplete", true }, { "items", std::move(items) } };
    return composed;
}

Composed compose_hover(const Entry& entry, Source source, Certainty certainty, const Context& context, std::string_view text, Marks marks) {
    Composed composed;
    Json hover = entry.result;
    hover["range"] = Json { { "start", position_json(base::position_at(text, context.wordStart)) },
                            { "end", position_json(base::position_at(text, context.wordEnd)) } };
    if (marks == Marks::verbose && hover.contains("contents") && hover["contents"].is_object()) {
        Json& contents = hover["contents"];
        contents["value"] = std::format("{}\n\n---\n_mcppls: {} answer ({})_", contents.value("value", std::string {}), name_of(source), name_of(certainty));
    }
    composed.result = std::move(hover);
    return composed;
}

Composed compose_signature(const Entry& entry, const Context& context) {
    Composed composed;
    composed.result = entry.result;
    composed.result["activeParameter"] = context.argument;
    return composed;
}

std::optional<Json> compose_jump(const Entry& entry, Certainty certainty, const std::optional<Json>& relocated) {
    if (certainty == Certainty::certain) return entry.result;
    if (relocated) return relocated;
    return std::nullopt;
}

std::pair<std::size_t, std::size_t> difference(const Json& served, const Json& answer) {
    std::set<std::string> was;
    std::set<std::string> is;
    if (const Json* items = items_of(served)) {
        for (const Json& item : *items) was.insert(label_of(item));
    }
    if (const Json* items = items_of(answer)) {
        for (const Json& item : *items) is.insert(label_of(item));
    }
    std::size_t gone { 0 };
    std::size_t added { 0 };
    for (const auto& name : was) gone += is.contains(name) ? 0 : 1;
    for (const auto& name : is) added += was.contains(name) ? 0 : 1;
    return { gone, added };
}

// ---- provenance -----------------------------------------------------------------------------------------------------

Json to_json(const Provenance& provenance) {
    Json value { { "authoritative", provenance.certainty == Certainty::authoritative },
                 { "source", std::string { name_of(provenance.source) } },
                 { "certainty", std::string { name_of(provenance.certainty) } } };
    if (provenance.certainty != Certainty::authoritative) {
        value["ageMs"] = provenance.ageMs;
        value["providerChanged"] = provenance.providerChanged;
        value["enginePending"] = provenance.enginePending;
    }
    if (!provenance.note.empty()) value["note"] = provenance.note;
    return value;
}

std::string note_for(Kind kind, const Provenance& provenance) {
    if (provenance.certainty == Certainty::authoritative) return {};
    const std::int64_t minutes { provenance.ageMs / 60000 };
    const std::string age { minutes >= 1 ? std::format("{} min ago", minutes) : std::format("{} s ago", provenance.ageMs / 1000) };
    const bool plural { kind == Kind::completion || kind == Kind::signature_help };
    const std::string what { kind == Kind::completion ? "The completions" : kind == Kind::hover ? "The hover" : kind == Kind::signature_help ? "The signatures" : "The location" };
    const std::string_view were { plural ? "were" : "was" };
    const std::string_view come { plural ? "come" : "comes" };
    if (provenance.source == Source::outline) return std::format("{} {} placed by reading the file's text, not by the compiler; the compiler's answer was not in yet.", what, were);
    if (provenance.providerChanged) return std::format("{} {} from an answer kept {}; the file that defines what {} about changed since.", what, come, age, plural ? "they are" : "it is");
    if (provenance.certainty == Certainty::uncertain) return std::format("{} {} from an answer kept {}; files changed since, so it may be out of date.", what, come, age);
    return std::format("{} {} from an answer kept {}; nothing it depends on changed since.", what, come, age);
}

} // namespace mcppls::orchestrator::answer
