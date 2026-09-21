module mcppls.engine.native.exports;

import std;
import mcppls.base.text;

namespace mcppls::index {

namespace {

enum class TokenKind { identifier, punctuation, literal, other };

struct Token {
    TokenKind kind { TokenKind::other };
    std::string_view text;
    std::size_t offset { 0 };
    bool spaceBefore { false };     // whitespace, a comment or a directive precedes this token
    int conditionalDepth { 0 };     // #if / #ifdef / #ifndef nesting at this token
};

// A comment's span in the source, "//..." to end of line or "/*...*/" whole.
struct Comment {
    std::size_t begin { 0 };
    std::size_t end { 0 };
};

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\f' || c == '\v' || c == '\r'; }

// Tokenizes the whole file up front (rather than streaming, like
// project::scan's lexer in src/project/scan.cpp): the declaration parser
// below needs lookahead and backtrack-free recursive descent, which a token
// vector makes simple. Comments and preprocessor lines are never emitted as
// tokens; comments are instead recorded separately for documentation lookup,
// and each token is stamped with the #if/#ifdef/#ifndef depth at that point.
class Lexer {
private:
    std::string_view text_;
    std::size_t at_ { 0 };
    bool lineStart_ { true };
    bool spacePending_ { false };
    int conditionalDepth_ { 0 };
    std::vector<Comment>& comments_;

public:
    Lexer(std::string_view text, std::vector<Comment>& comments) : text_ { text }, comments_ { comments } {}

    std::vector<Token> tokenize() {
        std::vector<Token> tokens;
        while (auto token = next_()) tokens.push_back(*token);
        return tokens;
    }

private:
    std::optional<Token> next_() {
        while (at_ < text_.size()) {
            const char c { text_[at_] };
            if (c == '\n') { lineStart_ = true; spacePending_ = true; ++at_; continue; }
            if (is_space(c)) { spacePending_ = true; ++at_; continue; }
            if (c == '\\' && at_ + 1 < text_.size() && (text_[at_ + 1] == '\n' || text_[at_ + 1] == '\r')) {
                spacePending_ = true;
                at_ += 2;
                continue;
            }
            if (c == '/' && at_ + 1 < text_.size() && text_[at_ + 1] == '/') {
                const std::size_t begin { at_ };
                skip_line_comment_();
                comments_.push_back(Comment { begin, at_ });
                spacePending_ = true;
                continue;
            }
            if (c == '/' && at_ + 1 < text_.size() && text_[at_ + 1] == '*') {
                const std::size_t begin { at_ };
                const std::size_t end { text_.find("*/", at_ + 2) };
                at_ = end == std::string_view::npos ? text_.size() : end + 2;
                comments_.push_back(Comment { begin, at_ });
                spacePending_ = true;
                continue;
            }
            if (c == '#' && lineStart_) { directive_(); spacePending_ = true; continue; }

            const std::size_t begin { at_ };
            const bool spaceBefore { spacePending_ };
            const int depth { conditionalDepth_ };
            lineStart_ = false;
            spacePending_ = false;

            if (c == 'R' && at_ + 1 < text_.size() && text_[at_ + 1] == '"') {
                raw_string_(at_ + 1);
                return Token { TokenKind::literal, text_.substr(begin, at_ - begin), begin, spaceBefore, depth };
            }
            if (c == '"' || c == '\'') {
                quoted_(c);
                return Token { TokenKind::literal, text_.substr(begin, at_ - begin), begin, spaceBefore, depth };
            }
            if (base::is_identifier_start(c)) {
                while (at_ < text_.size() && base::is_identifier_char(text_[at_])) ++at_;
                // An identifier directly followed by a quote is an encoding prefix (u8"x").
                if (at_ < text_.size() && (text_[at_] == '"' || text_[at_] == '\'')) {
                    const std::string_view prefix { text_.substr(begin, at_ - begin) };
                    if (prefix == "u8" || prefix == "u" || prefix == "U" || prefix == "L") {
                        quoted_(text_[at_]);
                        return Token { TokenKind::literal, text_.substr(begin, at_ - begin), begin, spaceBefore, depth };
                    }
                    if (prefix == "u8R" || prefix == "uR" || prefix == "UR" || prefix == "LR") {
                        raw_string_(at_);
                        return Token { TokenKind::literal, text_.substr(begin, at_ - begin), begin, spaceBefore, depth };
                    }
                }
                return Token { TokenKind::identifier, text_.substr(begin, at_ - begin), begin, spaceBefore, depth };
            }
            if (c >= '0' && c <= '9') {
                // pp-number, including digit separators (1'000) and exponents.
                ++at_;
                while (at_ < text_.size()) {
                    const char d { text_[at_] };
                    if (base::is_identifier_char(d) || d == '.') {
                        ++at_;
                    } else if (d == '\'' && at_ + 1 < text_.size() && base::is_identifier_char(text_[at_ + 1])) {
                        at_ += 2;
                    } else if ((d == '+' || d == '-') && (text_[at_ - 1] == 'e' || text_[at_ - 1] == 'E' || text_[at_ - 1] == 'p' || text_[at_ - 1] == 'P')) {
                        ++at_;
                    } else {
                        break;
                    }
                }
                return Token { TokenKind::literal, text_.substr(begin, at_ - begin), begin, spaceBefore, depth };
            }
            ++at_;
            return Token { TokenKind::punctuation, text_.substr(begin, 1), begin, spaceBefore, depth };
        }
        return std::nullopt;
    }

    void skip_line_comment_() {
        while (at_ < text_.size() && text_[at_] != '\n') {
            if (text_[at_] == '\\' && at_ + 1 < text_.size() && text_[at_ + 1] == '\n') ++at_;
            ++at_;
        }
    }

    void quoted_(char quote) {
        ++at_;
        while (at_ < text_.size() && text_[at_] != quote && text_[at_] != '\n') {
            if (text_[at_] == '\\' && at_ + 1 < text_.size()) ++at_;
            ++at_;
        }
        if (at_ < text_.size() && text_[at_] == quote) ++at_;
    }

    // `quoteAt` indexes the opening quote of R"delim( ... )delim".
    void raw_string_(std::size_t quoteAt) {
        const std::size_t open { text_.find('(', quoteAt + 1) };
        if (open == std::string_view::npos || open - quoteAt - 1 > 16) {
            at_ = quoteAt;
            quoted_('"');
            return;
        }
        const std::string terminator { std::format("){}\"", text_.substr(quoteAt + 1, open - quoteAt - 1)) };
        const std::size_t close { text_.find(terminator, open + 1) };
        at_ = close == std::string_view::npos ? text_.size() : close + terminator.size();
    }

    void directive_() {
        std::size_t cursor { at_ + 1 };
        while (cursor < text_.size() && is_space(text_[cursor])) ++cursor;
        std::size_t nameEnd { cursor };
        while (nameEnd < text_.size() && base::is_identifier_char(text_[nameEnd])) ++nameEnd;
        const std::string_view name { text_.substr(cursor, nameEnd - cursor) };
        if (name == "if" || name == "ifdef" || name == "ifndef") ++conditionalDepth_;
        if (name == "endif" && conditionalDepth_ > 0) --conditionalDepth_;
        // The directive runs to the end of its logical line; comments inside it end with it.
        while (at_ < text_.size() && text_[at_] != '\n') {
            if (text_[at_] == '\\' && at_ + 1 < text_.size() && (text_[at_ + 1] == '\n' || text_[at_ + 1] == '\r')) {
                at_ += text_[at_ + 1] == '\r' && at_ + 2 < text_.size() && text_[at_ + 2] == '\n' ? 3 : 2;
                continue;
            }
            if (text_[at_] == '/' && at_ + 1 < text_.size() && text_[at_ + 1] == '*') {
                const std::size_t end { text_.find("*/", at_ + 2) };
                at_ = end == std::string_view::npos ? text_.size() : end + 2;
                continue;
            }
            ++at_;
        }
        lineStart_ = true;
    }
};

bool is_line_comment_text(std::string_view text) { return text.size() >= 2 && text[0] == '/' && text[1] == '/'; }
bool is_block_comment_text(std::string_view text) { return text.size() >= 2 && text[0] == '/' && text[1] == '*'; }

std::string_view ltrim_line(std::string_view text) {
    while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
    return text;
}

std::string_view rtrim_line(std::string_view text) {
    while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
    return text;
}

// Strips a "//" or "///" marker (any number of leading slashes) and one
// following space, then trailing whitespace.
std::string strip_line_marker(std::string_view rawComment) {
    std::size_t i { 0 };
    while (i < rawComment.size() && rawComment[i] == '/') ++i;
    std::string_view rest { rawComment.substr(i) };
    if (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    return std::string { rtrim_line(rest) };
}

std::size_t common_indent(const std::vector<std::string>& lines) {
    std::size_t indent { std::string::npos };
    for (const auto& line : lines) {
        const std::size_t firstNonSpace { line.find_first_not_of(" \t") };
        if (firstNonSpace == std::string::npos) continue;   // a blank line does not constrain the indent
        indent = std::min(indent, firstNonSpace);
    }
    return indent == std::string::npos ? 0 : indent;
}

std::string strip_common_indent(std::vector<std::string> lines) {
    const std::size_t indent { common_indent(lines) };
    if (indent > 0) {
        for (auto& line : lines) {
            if (line.size() >= indent) line.erase(0, indent);
        }
    }
    return base::join(lines, "\n");
}

// A `/* ... */` block: the physical line right after "/*" is trimmed on both
// sides (it commonly carries the first sentence, e.g. "/* Doubles a value.");
// each following line has a leading "*" continuation marker (plus one space)
// stripped when present; a wholly blank first or last line is then dropped
// (the common "/*\n text \n*/" shape), and the remaining lines share their
// common indentation stripped.
std::string documentation_from_block(std::string_view commentText) {
    const std::string_view content { commentText.substr(2, commentText.size() - 4) };
    std::vector<std::string_view> rawLines { base::split_lines(content) };
    if (rawLines.empty()) return {};
    std::vector<std::string> lines;
    lines.reserve(rawLines.size());
    for (std::size_t i { 0 }; i < rawLines.size(); ++i) {
        if (i == 0) {
            lines.push_back(std::string { base::trim(rawLines[i]) });
            continue;
        }
        const std::string_view left { ltrim_line(rawLines[i]) };
        if (!left.empty() && left.front() == '*') {
            std::string_view rest { left.substr(1) };
            if (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
            lines.push_back(std::string { rtrim_line(rest) });
        } else {
            lines.push_back(std::string { rtrim_line(rawLines[i]) });
        }
    }
    if (lines.size() > 1 && lines.front().empty()) lines.erase(lines.begin());
    if (lines.size() > 1 && lines.back().empty()) lines.pop_back();
    return strip_common_indent(std::move(lines));
}

// Joins tokens [fromIndex, toIndex) verbatim, collapsing every original gap
// (space, tab, newline, comment) to a single space and inserting none where
// the source had none, so "template <typename T> T twice(T value)" keeps its
// original shape while multi-line source collapses to one line.
std::string collapse_span(const std::vector<Token>& tokens, std::size_t fromIndex, std::size_t toIndex) {
    std::string text;
    for (std::size_t i { fromIndex }; i < toIndex; ++i) {
        if (!text.empty() && tokens[i].spaceBefore) text += ' ';
        text += tokens[i].text;
    }
    return text;
}

bool is_specifier_keyword(std::string_view text) {
    static constexpr std::array<std::string_view, 11> KEYWORDS {
        "inline", "constexpr", "consteval", "constinit", "static", "friend",
        "virtual", "explicit", "mutable", "thread_local", "extern",
    };
    return std::ranges::any_of(KEYWORDS, [&](std::string_view keyword) { return keyword == text; });
}

std::string join_qualification(const std::string& outer, const std::vector<std::string>& components) {
    std::string result { outer };
    for (const auto& component : components) {
        if (!result.empty()) result += "::";
        result += component;
    }
    return result;
}

// Recursive-descent over the token vector: walks a module interface looking
// only for `export`; everything else (bodies, non-exported declarations) is
// structurally consumed to keep the cursor in sync but never inspected.
class Parser {
private:
    std::string_view source_;
    const std::vector<Token>& tokens_;
    const std::vector<Comment>& comments_;
    std::size_t cursor_ { 0 };
    std::vector<ExportedDeclaration> results_;

    struct NamePath {
        std::vector<std::string> components;
        std::size_t begin { 0 };
        std::size_t end { 0 };
        bool ok { false };
    };

    struct ModuleNameRef {
        std::string text;   // "m", "m.sub", ":p" or "m:p" as written, reconstructed canonically
        std::size_t begin { 0 };
        std::size_t end { 0 };
        bool ok { false };
    };

public:
    Parser(std::string_view source, const std::vector<Token>& tokens, const std::vector<Comment>& comments)
        : source_ { source }, tokens_ { tokens }, comments_ { comments } {}

    std::vector<ExportedDeclaration> run() {
        scan_scope_(false, "");
        return std::move(results_);
    }

private:
    const Token* peek_(std::size_t ahead = 0) const {
        return cursor_ + ahead < tokens_.size() ? &tokens_[cursor_ + ahead] : nullptr;
    }

    void advance_() {
        if (cursor_ < tokens_.size()) ++cursor_;
    }

    bool at_(std::string_view text) const {
        const auto* token { peek_() };
        return token != nullptr && token->text == text;
    }

    bool at_identifier_(std::string_view text) const {
        const auto* token { peek_() };
        return token != nullptr && token->kind == TokenKind::identifier && token->text == text;
    }

    static base::Range token_range_(std::string_view source, const Token& token) {
        return base::Range { base::position_at(source, token.offset), base::position_at(source, token.offset + token.text.size()) };
    }

    // A sequence of items at one brace-depth-0 scope: the whole file, a
    // namespace body, or an `export { ... }` group. Stops (without consuming)
    // at end of input or the scope's closing "}".
    void scan_scope_(bool implicitExported, const std::string& qualification) {
        while (true) {
            const auto* token { peek_() };
            if (token == nullptr) return;
            if (token->kind == TokenKind::punctuation && token->text == "}") return;

            bool exported { implicitExported };
            const std::size_t anchorOffset { token->offset };
            const int conditionalDepth { token->conditionalDepth };

            if (token->kind == TokenKind::identifier && token->text == "export") {
                exported = true;
                advance_();
                const auto* afterExport { peek_() };
                if (afterExport == nullptr) return;
                if (afterExport->kind == TokenKind::punctuation && afterExport->text == "{") {
                    advance_();   // consume the group's "{"
                    scan_scope_(true, qualification);
                    if (at_("}")) advance_();
                    continue;
                }
            }

            const auto* current { peek_() };
            if (current == nullptr) return;

            if (current->kind == TokenKind::identifier && current->text == "namespace") {
                handle_namespace_(exported, anchorOffset, conditionalDepth, qualification);
            } else if (current->kind == TokenKind::identifier && current->text == "import") {
                handle_import_(exported, anchorOffset, conditionalDepth);
            } else if (current->kind == TokenKind::identifier && current->text == "module") {
                skip_module_declaration_();
            } else {
                ExportedDeclaration declaration { parse_one_declaration_(qualification, anchorOffset, conditionalDepth) };
                if (exported) results_.push_back(std::move(declaration));
            }
        }
    }

    // `namespace a::b { ... }`: emits a namespace_ entry for itself when
    // exported (explicitly, or implicitly inside an already-exported scope),
    // and recurses with the qualification extended and implicit-export
    // inherited, so members need no "export" of their own.
    void handle_namespace_(bool exported, std::size_t anchorOffset, int conditionalDepth, const std::string& qualification) {
        const std::size_t declStartIndex { cursor_ };
        advance_();   // "namespace"
        NamePath path { parse_namespace_path_() };
        if (!path.ok) { recover_to_boundary_(); return; }
        skip_attributes_();
        if (!at_("{")) { recover_to_boundary_(); return; }   // a namespace-alias or malformed input
        const std::size_t declEndIndex { cursor_ };          // at "{"
        const std::string name { path.components.back() };
        const std::string qualifiedName { join_qualification(qualification, path.components) };
        if (exported) {
            ExportedDeclaration entry;
            entry.kind = DeclarationKind::namespace_;
            entry.name = name;
            entry.qualifiedName = qualifiedName;
            entry.declaration = collapse_span(tokens_, declStartIndex, declEndIndex);
            entry.documentation = extract_documentation_(anchorOffset);
            entry.nameRange = base::Range { base::position_at(source_, path.begin), base::position_at(source_, path.end) };
            entry.conditional = conditionalDepth > 0;
            results_.push_back(std::move(entry));
        }
        advance_();   // consume "{"
        scan_scope_(exported, qualifiedName);
        if (at_("}")) advance_();
    }

    // `[export] import <module-name-ref>;`. A re-export only when exported;
    // a plain (non-exported) import is not a declaration and is just skipped.
    void handle_import_(bool exported, std::size_t anchorOffset, int conditionalDepth) {
        const std::size_t declStartIndex { cursor_ };
        advance_();   // "import"
        ModuleNameRef ref { parse_module_name_ref_() };
        if (!ref.ok) { recover_to_boundary_(); return; }
        const std::size_t declEndIndex { cursor_ };
        skip_attributes_();
        if (at_(";")) advance_();
        if (!exported) return;
        ExportedDeclaration entry;
        entry.kind = DeclarationKind::reexport;
        entry.name = ref.text;
        entry.qualifiedName = ref.text;   // re-exports are only ever written at global module scope
        entry.declaration = collapse_span(tokens_, declStartIndex, declEndIndex);
        entry.documentation = extract_documentation_(anchorOffset);
        entry.nameRange = base::Range { base::position_at(source_, ref.begin), base::position_at(source_, ref.end) };
        entry.conditional = conditionalDepth > 0;
        results_.push_back(std::move(entry));
    }

    void skip_module_declaration_() {
        advance_();   // "module"
        skip_to_top_level_semicolon_();
        if (at_(";")) advance_();
    }

    // One declaration: prefix specifiers/attributes/`template <...>` first
    // (kept in the text but not yet deciding the kind), then a dispatch on
    // what follows.
    ExportedDeclaration parse_one_declaration_(const std::string& qualification, std::size_t anchorOffset, int conditionalDepth) {
        const std::size_t declStartIndex { cursor_ };
        while (true) {
            const auto* token { peek_() };
            if (token == nullptr) break;
            if (token->kind == TokenKind::identifier && token->text == "template") {
                advance_();
                skip_template_parameters_();
                continue;
            }
            if (token->kind == TokenKind::punctuation && token->text == "[") { skip_attributes_(); continue; }
            if (token->kind == TokenKind::identifier && is_specifier_keyword(token->text)) { advance_(); continue; }
            break;
        }

        const auto* keyword { peek_() };
        if (keyword != nullptr && keyword->kind == TokenKind::identifier
            && (keyword->text == "class" || keyword->text == "struct" || keyword->text == "union")) {
            const DeclarationKind kind { keyword->text == "class" ? DeclarationKind::class_type
                                        : keyword->text == "struct" ? DeclarationKind::struct_type
                                                                     : DeclarationKind::union_type };
            advance_();
            return finish_class_like_(qualification, anchorOffset, conditionalDepth, declStartIndex, kind);
        }
        if (keyword != nullptr && keyword->kind == TokenKind::identifier && keyword->text == "enum") {
            advance_();
            const auto* next { peek_() };
            if (next != nullptr && next->kind == TokenKind::identifier && (next->text == "class" || next->text == "struct")) advance_();
            return finish_class_like_(qualification, anchorOffset, conditionalDepth, declStartIndex, DeclarationKind::enum_type);
        }
        if (keyword != nullptr && keyword->kind == TokenKind::identifier && keyword->text == "concept") {
            advance_();
            return finish_concept_(qualification, anchorOffset, conditionalDepth, declStartIndex);
        }
        if (keyword != nullptr && keyword->kind == TokenKind::identifier && keyword->text == "using") {
            advance_();
            return finish_using_(qualification, anchorOffset, conditionalDepth, declStartIndex);
        }
        return finish_generic_(qualification, anchorOffset, conditionalDepth, declStartIndex);
    }

    // class/struct/union/enum[ class|struct]: name, then a base-clause or
    // enum-base kept verbatim up to (excluding) the body or terminator; the
    // body, if any, is skipped wholesale rather than scanned for exports.
    ExportedDeclaration finish_class_like_(const std::string& qualification, std::size_t anchorOffset, int conditionalDepth,
                                            std::size_t declStartIndex, DeclarationKind kind) {
        skip_attributes_();
        std::string name;
        base::Range nameRange;
        const auto* nameToken { peek_() };
        if (nameToken != nullptr && nameToken->kind == TokenKind::identifier) {
            name = std::string { nameToken->text };
            nameRange = token_range_(source_, *nameToken);
            advance_();
        }
        int depth { 0 };
        while (true) {
            const auto* token { peek_() };
            if (token == nullptr) break;
            if (depth == 0 && token->text == "{") break;
            if (depth == 0 && token->text == ";") break;
            if (token->text == "(" || token->text == "[") ++depth;
            if (token->text == ")" || token->text == "]") --depth;
            advance_();
        }
        const std::size_t declEndIndex { cursor_ };
        const auto* terminator { peek_() };
        if (terminator != nullptr && terminator->text == "{") {
            skip_balanced_braces_();
            if (at_(";")) advance_();
        } else if (terminator != nullptr && terminator->text == ";") {
            advance_();
        }
        ExportedDeclaration entry;
        entry.kind = kind;
        entry.name = name;
        entry.qualifiedName = qualification.empty() ? name : qualification + "::" + name;
        entry.declaration = collapse_span(tokens_, declStartIndex, declEndIndex);
        entry.documentation = extract_documentation_(anchorOffset);
        entry.nameRange = nameRange;
        entry.conditional = conditionalDepth > 0;
        return entry;
    }

    // `concept Name = constraint-expression;`: text stops at (excludes) "=".
    ExportedDeclaration finish_concept_(const std::string& qualification, std::size_t anchorOffset, int conditionalDepth, std::size_t declStartIndex) {
        std::string name;
        base::Range nameRange;
        const auto* nameToken { peek_() };
        if (nameToken != nullptr && nameToken->kind == TokenKind::identifier) {
            name = std::string { nameToken->text };
            nameRange = token_range_(source_, *nameToken);
            advance_();
        }
        const std::size_t declEndIndex { cursor_ };
        if (at_("=")) advance_();
        skip_to_top_level_semicolon_();
        if (at_(";")) advance_();
        ExportedDeclaration entry;
        entry.kind = DeclarationKind::concept_;
        entry.name = name;
        entry.qualifiedName = qualification.empty() ? name : qualification + "::" + name;
        entry.declaration = collapse_span(tokens_, declStartIndex, declEndIndex);
        entry.documentation = extract_documentation_(anchorOffset);
        entry.nameRange = nameRange;
        entry.conditional = conditionalDepth > 0;
        return entry;
    }

    // `using Name = type-id;` (an alias: text stops at "="). Any other form
    // (`using std::string;`, `using namespace std;`) is not one of the
    // rules' shapes; kept as a best-effort "other" entry rather than dropped.
    ExportedDeclaration finish_using_(const std::string& qualification, std::size_t anchorOffset, int conditionalDepth, std::size_t declStartIndex) {
        std::string name;
        base::Range nameRange;
        const auto* nameToken { peek_() };
        if (nameToken != nullptr && nameToken->kind == TokenKind::identifier) {
            name = std::string { nameToken->text };
            nameRange = token_range_(source_, *nameToken);
            advance_();
        }
        ExportedDeclaration entry;
        if (at_("=")) {
            const std::size_t declEndIndex { cursor_ };
            advance_();   // "="
            skip_to_top_level_semicolon_();
            if (at_(";")) advance_();
            entry.kind = DeclarationKind::alias;
            entry.declaration = collapse_span(tokens_, declStartIndex, declEndIndex);
        } else {
            skip_to_top_level_semicolon_();
            const std::size_t declEndIndex { cursor_ };
            if (at_(";")) advance_();
            entry.kind = DeclarationKind::other;
            entry.declaration = collapse_span(tokens_, declStartIndex, declEndIndex);
        }
        entry.name = name;
        entry.qualifiedName = qualification.empty() ? name : qualification + "::" + name;
        entry.documentation = extract_documentation_(anchorOffset);
        entry.nameRange = nameRange;
        entry.conditional = conditionalDepth > 0;
        return entry;
    }

    // Functions/function templates and variables. The declarator name is the
    // last top-level identifier seen before the declaration's kind is fixed
    // (a top-level "(" fixes it as a function; nothing further updates the
    // name, so a trailing return type or "= delete" cannot overwrite it). A
    // top-level "{" or "=" then means a body/brace-init or an initializer for
    // a variable, but "= delete"/"= default" for a function stays in the text.
    ExportedDeclaration finish_generic_(const std::string& qualification, std::size_t anchorOffset, int conditionalDepth, std::size_t declStartIndex) {
        DeclarationKind kind { DeclarationKind::other };
        std::string name;
        base::Range nameRange;
        int depth { 0 };
        std::size_t declEndIndex { cursor_ };

        while (true) {
            const auto* token { peek_() };
            if (token == nullptr) { declEndIndex = cursor_; break; }

            if (depth == 0 && kind == DeclarationKind::other && token->kind == TokenKind::identifier) {
                name = std::string { token->text };
                nameRange = token_range_(source_, *token);
            }
            if (depth == 0 && token->kind == TokenKind::punctuation && token->text == "(") {
                kind = DeclarationKind::function;
                ++depth;
                advance_();
                continue;
            }
            if (depth == 0 && token->kind == TokenKind::punctuation && token->text == "{") {
                declEndIndex = cursor_;
                skip_balanced_braces_();
                if (kind != DeclarationKind::function) {
                    kind = DeclarationKind::variable;
                    if (at_(";")) advance_();
                }
                break;
            }
            if (depth == 0 && token->kind == TokenKind::punctuation && token->text == "=") {
                if (kind == DeclarationKind::function) {
                    // "= delete" / "= default": part of the declaration, not an initializer.
                    advance_();
                    continue;
                }
                declEndIndex = cursor_;
                advance_();   // "="
                skip_to_top_level_semicolon_();
                if (at_(";")) advance_();
                kind = DeclarationKind::variable;
                break;
            }
            if (depth == 0 && token->kind == TokenKind::punctuation && token->text == ";") {
                declEndIndex = cursor_;
                advance_();
                if (kind == DeclarationKind::other) kind = DeclarationKind::variable;
                break;
            }
            if (depth == 0 && token->kind == TokenKind::punctuation && token->text == ",") {
                // Multiple declarators in one declaration (`int a, b;`) are a
                // known gap: only the first is reported; the rest are dropped
                // rather than misreported as a second entry.
                declEndIndex = cursor_;
                skip_to_top_level_semicolon_();
                if (at_(";")) advance_();
                if (kind == DeclarationKind::other) kind = DeclarationKind::variable;
                break;
            }
            if (token->text == "(" || token->text == "[") ++depth;
            if (token->text == ")" || token->text == "]") --depth;
            advance_();
        }

        ExportedDeclaration entry;
        entry.kind = kind;
        entry.name = name;
        entry.qualifiedName = qualification.empty() ? name : qualification + "::" + name;
        entry.declaration = collapse_span(tokens_, declStartIndex, declEndIndex);
        entry.documentation = extract_documentation_(anchorOffset);
        entry.nameRange = nameRange;
        entry.conditional = conditionalDepth > 0;
        return entry;
    }

    void skip_template_parameters_() {
        if (!at_("<")) return;
        int depth { 0 };
        do {
            const auto* token { peek_() };
            if (token == nullptr) return;
            if (token->text == "<") ++depth;
            if (token->text == ">") --depth;
            advance_();
        } while (depth > 0);
    }

    // One or more `[[ ... ]]` (or, generically, `[ ... ]`) attribute groups.
    void skip_attributes_() {
        while (at_("[")) {
            int depth { 0 };
            do {
                if (at_end_of_attribute_scan_()) return;
                if (at_("[")) ++depth;
                if (at_("]")) --depth;
                advance_();
            } while (depth > 0);
        }
    }

    bool at_end_of_attribute_scan_() const { return peek_() == nullptr; }

    // `cursor_` is at the opening "{"; consumes the whole balanced block.
    void skip_balanced_braces_() {
        int depth { 0 };
        do {
            if (peek_() == nullptr) return;
            if (at_("{")) ++depth;
            if (at_("}")) --depth;
            advance_();
        } while (depth > 0);
    }

    // Advances (honoring nested (), [], {} depth) up to but not including a
    // top-level ";"; leaves the cursor there, or at end of input.
    void skip_to_top_level_semicolon_() {
        int depth { 0 };
        while (true) {
            const auto* token { peek_() };
            if (token == nullptr) return;
            if (depth == 0 && token->text == ";") return;
            if (token->text == "(" || token->text == "[" || token->text == "{") ++depth;
            if (token->text == ")" || token->text == "]" || token->text == "}") --depth;
            advance_();
        }
    }

    // Defensive fallback for a construct this scanner does not model (a
    // namespace-alias, malformed input): advance past one top-level ";" or a
    // balanced "{ ... }" (with an optional trailing ";") so scanning resumes
    // instead of getting stuck.
    void recover_to_boundary_() {
        int depth { 0 };
        while (true) {
            const auto* token { peek_() };
            if (token == nullptr) return;
            if (depth == 0 && token->text == ";") { advance_(); return; }
            if (depth == 0 && token->text == "{") {
                skip_balanced_braces_();
                if (at_(";")) advance_();
                return;
            }
            if (token->text == "(" || token->text == "[") ++depth;
            if (token->text == ")" || token->text == "]") --depth;
            advance_();
        }
    }

    // namespace-name: identifier ('::' identifier)*
    NamePath parse_namespace_path_() {
        NamePath result;
        const auto* first { peek_() };
        if (first == nullptr || first->kind != TokenKind::identifier) return result;
        result.begin = first->offset;
        result.end = first->offset + first->text.size();
        result.components.push_back(std::string { first->text });
        advance_();
        while (at_(":") && peek_(1) != nullptr && peek_(1)->kind == TokenKind::punctuation && peek_(1)->text == ":") {
            advance_();
            advance_();
            const auto* next { peek_() };
            if (next == nullptr || next->kind != TokenKind::identifier) return result;   // ok stays false
            result.components.push_back(std::string { next->text });
            result.end = next->offset + next->text.size();
            advance_();
        }
        result.ok = true;
        return result;
    }

    // module-name-ref: [':'] identifier ('.' identifier)* [':' identifier ('.' identifier)*]
    // Mirrors project::scan's grammar (src/project/scan.cpp) but reconstructs
    // the canonical "m" / "m.sub" / ":p" / "m:p" text directly.
    ModuleNameRef parse_module_name_ref_() {
        ModuleNameRef result;
        const auto* first { peek_() };
        if (first == nullptr) return result;
        result.begin = first->offset;
        std::string text;
        if (first->kind == TokenKind::punctuation && first->text == ":") {
            text += ':';
            advance_();
        }
        auto read_dotted = [&]() -> bool {
            const auto* start { peek_() };
            if (start == nullptr || start->kind != TokenKind::identifier) return false;
            text += start->text;
            result.end = start->offset + start->text.size();
            advance_();
            while (at_(".")) {
                advance_();
                const auto* part { peek_() };
                if (part == nullptr || part->kind != TokenKind::identifier) return false;
                text += '.';
                text += part->text;
                result.end = part->offset + part->text.size();
                advance_();
            }
            return true;
        };
        if (!read_dotted()) return result;
        if (at_(":")) {
            advance_();
            text += ':';
            if (!read_dotted()) return result;
        }
        result.text = std::move(text);
        result.ok = true;
        return result;
    }

    std::string extract_documentation_(std::size_t anchorOffset) {
        if (comments_.empty()) return {};
        std::size_t index { comments_.size() };
        while (index > 0 && comments_[index - 1].end > anchorOffset) --index;
        if (index == 0) return {};
        --index;
        if (!immediately_above_(comments_[index].end, anchorOffset)) return {};
        const Comment& comment { comments_[index] };
        const std::string_view text { source_.substr(comment.begin, comment.end - comment.begin) };
        if (is_block_comment_text(text)) return documentation_from_block(text);
        if (is_line_comment_text(text)) return documentation_from_line_chain_(index);
        return {};
    }

    // A run of consecutive "//"/"///" lines immediately above one another
    // (each gap containing only whitespace and exactly one newline), read
    // top to bottom, markers stripped, then their common indentation.
    std::string documentation_from_line_chain_(std::size_t lastIndex) {
        std::vector<std::string> lines;
        std::size_t index { lastIndex };
        while (true) {
            const Comment& comment { comments_[index] };
            lines.push_back(strip_line_marker(source_.substr(comment.begin, comment.end - comment.begin)));
            if (index == 0) break;
            const Comment& previous { comments_[index - 1] };
            const std::string_view previousText { source_.substr(previous.begin, previous.end - previous.begin) };
            if (!is_line_comment_text(previousText)) break;
            if (!immediately_above_(previous.end, comment.begin)) break;
            --index;
        }
        std::ranges::reverse(lines);
        return strip_common_indent(std::move(lines));
    }

    // Whether [from, to) holds only whitespace with exactly one newline: the
    // spans that bound it sit on directly adjacent lines, with none blank
    // between them and nothing else (code, another comment) in the gap.
    bool immediately_above_(std::size_t from, std::size_t to) const {
        if (from > to) return false;
        int newlines { 0 };
        for (std::size_t i { from }; i < to; ++i) {
            const char c { source_[i] };
            if (c == '\n') { ++newlines; continue; }
            if (c != ' ' && c != '\t' && c != '\r' && c != '\f' && c != '\v') return false;
        }
        return newlines == 1;
    }
};

} // namespace

std::string_view to_string(DeclarationKind kind) {
    switch (kind) {
    case DeclarationKind::function: return "function";
    case DeclarationKind::class_type: return "class";
    case DeclarationKind::struct_type: return "struct";
    case DeclarationKind::union_type: return "union";
    case DeclarationKind::enum_type: return "enum";
    case DeclarationKind::concept_: return "concept";
    case DeclarationKind::alias: return "alias";
    case DeclarationKind::variable: return "variable";
    case DeclarationKind::namespace_: return "namespace";
    case DeclarationKind::reexport: return "reexport";
    case DeclarationKind::other: return "other";
    }
    return "other";
}

std::vector<ExportedDeclaration> exported_declarations(std::string_view source) {
    std::vector<Comment> comments;
    Lexer lexer { source, comments };
    const std::vector<Token> tokens { lexer.tokenize() };
    Parser parser { source, tokens, comments };
    return parser.run();
}

} // namespace mcppls::index
