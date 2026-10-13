module mcppls.orchestrator.outline;

import std;

namespace mcppls::orchestrator::outline {

namespace {

bool identifier_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }
bool identifier_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

struct Token {
    enum class Kind { identifier, punctuation, literal, directive } kind;
    std::string_view text;
    int line { 0 };
    int column { 0 };
};

// Identifiers, punctuation (`::` and `->` as one), literals and preprocessor directives; comments are dropped.
std::vector<Token> tokenize(std::string_view text) {
    std::vector<Token> tokens;
    int line { 0 };
    std::size_t lineStart { 0 };
    bool lineHasCode { false };
    std::size_t i { 0 };
    const auto push = [&](Token::Kind kind, std::size_t begin, std::size_t end) {
        tokens.push_back(Token { kind, text.substr(begin, end - begin), line, static_cast<int>(begin - lineStart) });
        lineHasCode = true;
    };
    while (i < text.size()) {
        const char c { text[i] };
        if (c == '\n') {
            ++line;
            lineStart = ++i;
            lineHasCode = false;
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            ++i;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            i = std::min(text.find('\n', i), text.size());
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            const std::size_t end { text.find("*/", i + 2) };
            const std::size_t stop { end == std::string_view::npos ? text.size() : end + 2 };
            for (std::size_t j { i }; j < stop; ++j) {
                if (text[j] == '\n') {
                    ++line;
                    lineStart = j + 1;
                }
            }
            i = stop;
        } else if (c == '#' && !lineHasCode) {
            // A directive runs to the end of its line, continued by a backslash.
            std::size_t end { i };
            while (end < text.size() && text[end] != '\n') {
                if (text[end] == '\\' && end + 1 < text.size() && text[end + 1] == '\n') {
                    end += 2;
                    ++line;
                    lineStart = end;
                    continue;
                }
                ++end;
            }
            push(Token::Kind::directive, i, end);
            i = end;
        } else if (c == '"' || c == '\'') {
            std::size_t j { i + 1 };
            while (j < text.size() && text[j] != c && text[j] != '\n') j += text[j] == '\\' ? 2 : 1;
            push(Token::Kind::literal, i, std::min(j + 1, text.size()));
            i = j + 1;
        } else if (identifier_start(c)) {
            std::size_t j { i };
            while (j < text.size() && identifier_char(text[j])) ++j;
            // A raw string literal: R"delimiter( ... )delimiter", with any encoding prefix.
            const std::string_view word { text.substr(i, j - i) };
            if (j < text.size() && text[j] == '"' && (word == "R" || word == "LR" || word == "uR" || word == "UR" || word == "u8R")) {
                const std::size_t open { text.find('(', j) };
                if (open != std::string_view::npos && open - j <= 17) {
                    const std::string closing { std::format("){}\"", text.substr(j + 1, open - j - 1)) };
                    const std::size_t end { text.find(closing, open) };
                    const std::size_t stop { end == std::string_view::npos ? text.size() : end + closing.size() };
                    for (std::size_t k { i }; k < stop; ++k) {
                        if (text[k] == '\n') {
                            ++line;
                            lineStart = k + 1;
                        }
                    }
                    push(Token::Kind::literal, i, stop);
                    i = stop;
                    continue;
                }
            }
            push(Token::Kind::identifier, i, j);
            i = j;
        } else if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            std::size_t j { i };
            while (j < text.size() && (identifier_char(text[j]) || text[j] == '.' || text[j] == '\'')) ++j;
            push(Token::Kind::literal, i, j);
            i = j;
        } else if (i + 1 < text.size() && ((c == ':' && text[i + 1] == ':') || (c == '-' && text[i + 1] == '>'))) {
            push(Token::Kind::punctuation, i, i + 2);
            i += 2;
        } else {
            push(Token::Kind::punctuation, i, i + 1);
            ++i;
        }
    }
    return tokens;
}

bool is(const Token& token, std::string_view text) { return token.text == text; }

constexpr std::array<std::string_view, 48> NOT_NAMES {
    "public", "private", "protected", "virtual", "static", "inline", "constexpr", "consteval", "constinit", "const",
    "volatile", "mutable", "explicit", "friend", "typename", "template", "override", "final", "noexcept", "operator",
    "return", "using", "typedef", "decltype", "auto", "void", "bool", "char", "int", "long", "short", "unsigned",
    "signed", "float", "double", "wchar_t", "char8_t", "char16_t", "char32_t", "extern", "register", "thread_local",
    "requires", "default", "delete", "sizeof", "alignas", "struct",
};

bool a_name(const Token& token) {
    return token.kind == Token::Kind::identifier && std::ranges::find(NOT_NAMES, token.text) == NOT_NAMES.end();
}

// The index just past the bracket that closes the one at `open`, or npos when it does not close.
std::size_t past_closing(const std::vector<Token>& tokens, std::size_t open) {
    const std::string_view opening { tokens[open].text };
    const std::string_view closing { opening == "(" ? ")" : opening == "[" ? "]" : opening == "<" ? ">" : "}" };
    int depth { 0 };
    for (std::size_t i { open }; i < tokens.size(); ++i) {
        if (tokens[i].kind != Token::Kind::punctuation) continue;
        if (tokens[i].text == opening) ++depth;
        else if (tokens[i].text == closing && --depth == 0) return i + 1;
        else if (opening == "<" && (tokens[i].text == ";" || tokens[i].text == "{" || tokens[i].text == "}")) return std::string_view::npos;
    }
    return std::string_view::npos;
}

// The names one member declaration (tokens [begin, end), at the class body's own depth) declares.
void declared_names(const std::vector<Token>& tokens, std::size_t begin, std::size_t end, std::string_view className,
                    std::vector<std::string>& names) {
    // Leading `template <...>`, access specifiers, attributes.
    while (begin < end) {
        if (is(tokens[begin], "template") && begin + 1 < end && is(tokens[begin + 1], "<")) {
            const std::size_t past { past_closing(tokens, begin + 1) };
            if (past == std::string_view::npos || past > end) return;
            begin = past;
        } else if ((is(tokens[begin], "public") || is(tokens[begin], "private") || is(tokens[begin], "protected")) && begin + 1 < end
                   && is(tokens[begin + 1], ":")) {
            begin += 2;
        } else if (is(tokens[begin], "[") && begin + 1 < end && is(tokens[begin + 1], "[")) {
            const std::size_t past { past_closing(tokens, begin) };
            if (past == std::string_view::npos || past > end) return;
            begin = past;
        } else {
            break;
        }
    }
    if (begin >= end || is(tokens[begin], "friend") || is(tokens[begin], "static_assert")) return;
    const auto add = [&](std::string_view name) {
        if (name.empty() || name == className) return;
        if (std::ranges::find(names, name) == names.end()) names.emplace_back(name);
    };
    if (is(tokens[begin], "using")) {
        // `using name = type;` or `using Base::name;`
        if (begin + 2 < end && a_name(tokens[begin + 1]) && is(tokens[begin + 2], "=")) add(tokens[begin + 1].text);
        else if (a_name(tokens[end - 1])) add(tokens[end - 1].text);
        return;
    }
    if (is(tokens[begin], "typedef")) {
        for (std::size_t i { end }; i > begin + 1; --i) {
            if (a_name(tokens[i - 1])) {
                add(tokens[i - 1].text);
                return;
            }
        }
        return;
    }
    if ((is(tokens[begin], "class") || is(tokens[begin], "struct") || is(tokens[begin], "union") || is(tokens[begin], "enum"))) {
        std::size_t i { begin + 1 };
        if (i < end && (is(tokens[i], "class") || is(tokens[i], "struct"))) ++i;   // enum class
        if (i < end && a_name(tokens[i])) add(tokens[i].text);
        return;
    }
    // A function: the name before its first parenthesis at this depth; a destructor or an operator is not one to offer.
    int angle { 0 };
    for (std::size_t i { begin }; i < end; ++i) {
        if (is(tokens[i], "<")) ++angle;
        else if (is(tokens[i], ">") && angle > 0) --angle;
        else if (angle == 0 && is(tokens[i], "(")) {
            if (i == begin || tokens[i - 1].kind != Token::Kind::identifier) return;
            if (i >= 2 && (is(tokens[i - 2], "~") || is(tokens[i - 2], "operator"))) return;
            if (!a_name(tokens[i - 1]) || i - begin < 2) {
                if (i - begin == 1 && tokens[i - 1].text != className) return;   // `NAME(...)` alone: a macro, or the constructor
            }
            add(tokens[i - 1].text);
            return;
        }
    }
    // Data members: each name before `=`, `{`, `[`, `,`, `:` or the end, after a type.
    angle = 0;
    std::size_t typeTokens { 0 };
    for (std::size_t i { begin }; i < end; ++i) {
        if (is(tokens[i], "<")) ++angle;
        else if (is(tokens[i], ">") && angle > 0) --angle;
        if (angle > 0) continue;
        const bool last { i + 1 == end };
        const bool beforeDelimiter { !last && (is(tokens[i + 1], "=") || is(tokens[i + 1], "{") || is(tokens[i + 1], "[") || is(tokens[i + 1], ",")
                                               || is(tokens[i + 1], ":")) };
        if (a_name(tokens[i]) && (last || beforeDelimiter) && typeTokens > 0) {
            add(tokens[i].text);
            // Past an initializer to the next declarator.
            std::size_t j { i + 1 };
            int depth { 0 };
            while (j < end && !(depth == 0 && is(tokens[j], ","))) {
                if (is(tokens[j], "(") || is(tokens[j], "{") || is(tokens[j], "[")) ++depth;
                else if (is(tokens[j], ")") || is(tokens[j], "}") || is(tokens[j], "]")) --depth;
                ++j;
            }
            i = j;
            continue;
        }
        ++typeTokens;
    }
}

} // namespace

std::optional<std::vector<std::string>> class_members(std::string_view text, int line, int column) {
    const std::vector<Token> tokens { tokenize(text) };
    // The class's name on the line, after a class-key.
    std::size_t head { std::string_view::npos };
    for (std::size_t i { 1 }; i < tokens.size(); ++i) {
        if (tokens[i].line != line || tokens[i].column < column || tokens[i].kind != Token::Kind::identifier) continue;
        if (is(tokens[i - 1], "class") || is(tokens[i - 1], "struct") || is(tokens[i - 1], "union")) {
            head = i;
            break;
        }
    }
    if (head == std::string_view::npos) return std::nullopt;
    const std::string_view className { tokens[head].text };
    // Its body: the first `{` before any `;` (a declaration only has none).
    std::size_t open { head + 1 };
    while (open < tokens.size() && !is(tokens[open], "{")) {
        if (is(tokens[open], ";")) return std::nullopt;
        if (is(tokens[open], "<")) {
            const std::size_t past { past_closing(tokens, open) };
            if (past == std::string_view::npos) return std::nullopt;
            open = past;
            continue;
        }
        ++open;
    }
    if (open >= tokens.size()) return std::nullopt;
    const std::size_t close { past_closing(tokens, open) };
    if (close == std::string_view::npos) return std::nullopt;
    std::vector<std::string> names;
    std::size_t start { open + 1 };
    int depth { 0 };
    for (std::size_t i { open + 1 }; i + 1 < close; ++i) {
        const Token& token { tokens[i] };
        if (token.kind == Token::Kind::directive) {
            // A conditional, or a macro defined in the body, makes what it declares uncertain.
            const std::string_view directive { token.text.substr(1) };
            const std::size_t word { directive.find_first_not_of(" \t") };
            if (word == std::string_view::npos || !directive.substr(word).starts_with("pragma")) return std::nullopt;
            start = i + 1;
            continue;
        }
        if (depth == 0 && is(token, "{")) {
            // A function's body, a nested type's, or a brace initializer.
            const std::size_t past { past_closing(tokens, i) };
            if (past == std::string_view::npos || past >= close) return std::nullopt;
            const auto head = std::span { tokens }.subspan(start, i - start);
            const bool function { std::ranges::any_of(head, [](const Token& t) { return is(t, "("); }) };
            const bool type { std::ranges::any_of(head, [](const Token& t) { return is(t, "class") || is(t, "struct") || is(t, "union") || is(t, "enum"); }) };
            if (function || type) {
                declared_names(tokens, start, i, className, names);
                start = past;
                if (start + 1 < close && is(tokens[start], ";")) ++start;
                i = start - 1;
            } else {
                i = past - 1;   // `int x { 1 };`: the declaration ends at its `;`
            }
            continue;
        }
        if (is(token, "{") || is(token, "(") || is(token, "[")) {
            ++depth;
        } else if (is(token, "}") || is(token, ")") || is(token, "]")) {
            --depth;
        } else if (depth == 0 && is(token, ";")) {
            declared_names(tokens, start, i, className, names);
            start = i + 1;
        } else if (depth == 0 && token.kind == Token::Kind::punctuation && is(token, ":") && i > start
                   && (is(tokens[i - 1], "public") || is(tokens[i - 1], "private") || is(tokens[i - 1], "protected"))) {
            start = i + 1;
        }
    }
    if (depth != 0) return std::nullopt;
    return names;
}

std::vector<Place> declarations(std::string_view text, std::string_view name) {
    const std::vector<Token> tokens { tokenize(text) };
    std::vector<Place> places;
    // What each open brace opened: a scope that declares (a namespace, a class, an `extern "C"` block) or a block.
    std::vector<bool> declaring;
    std::size_t statement { 0 };   // where the current declaration began
    const auto in_declaring_scope = [&] { return std::ranges::all_of(declaring, [](bool d) { return d; }); };
    for (std::size_t i { 0 }; i < tokens.size(); ++i) {
        const Token& token { tokens[i] };
        if (token.kind == Token::Kind::directive) {
            statement = i + 1;
            continue;
        }
        if (is(token, "{")) {
            // A namespace, an `extern "C"` block, a class's body or an `export { }` block declares; anything else -- a
            // function's body, an initializer, a lambda -- is a block.
            const auto head = std::span { tokens }.subspan(statement, i - statement);
            const bool parenthesized { std::ranges::any_of(head, [](const Token& t) { return is(t, "("); }) };
            const bool scope { std::ranges::any_of(head, [](const Token& t) { return is(t, "namespace") || is(t, "extern"); })
                               || (!parenthesized && std::ranges::any_of(head, [](const Token& t) {
                                      return is(t, "class") || is(t, "struct") || is(t, "union") || is(t, "enum");
                                  }))
                               || (head.size() == 1 && is(head.front(), "export")) };
            declaring.push_back(scope);
            statement = i + 1;
            continue;
        }
        if (is(token, "}")) {
            if (!declaring.empty()) declaring.pop_back();
            statement = i + 1;
            continue;
        }
        if (is(token, ";")) {
            statement = i + 1;
            continue;
        }
        if (token.kind != Token::Kind::identifier || token.text != name || !in_declaring_scope()) continue;
        // Not a use: no member access or call argument before it; a declarator after it.
        if (i > 0 && (is(tokens[i - 1], ".") || is(tokens[i - 1], "->") || is(tokens[i - 1], "::"))) continue;
        const bool keyword { i > 0
                             && (is(tokens[i - 1], "class") || is(tokens[i - 1], "struct") || is(tokens[i - 1], "union") || is(tokens[i - 1], "enum")
                                 || is(tokens[i - 1], "namespace") || is(tokens[i - 1], "using") || is(tokens[i - 1], "concept")) };
        const bool typed { i > statement && i > 0
                           && (tokens[i - 1].kind == Token::Kind::identifier || is(tokens[i - 1], ">") || is(tokens[i - 1], "*") || is(tokens[i - 1], "&")) };
        const bool declarator { i + 1 < tokens.size()
                                && (is(tokens[i + 1], "(") || is(tokens[i + 1], ";") || is(tokens[i + 1], "=") || is(tokens[i + 1], "{")
                                    || is(tokens[i + 1], "[") || is(tokens[i + 1], ",") || is(tokens[i + 1], ":")) };
        if ((keyword && i + 1 < tokens.size()) || (typed && declarator)) places.push_back(Place { token.line, token.column });
    }
    return places;
}

} // namespace mcppls::orchestrator::outline
