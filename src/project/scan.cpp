module mcppls.project.scan;

import std;
import mcppls.base.text;
import mcppls.base.path;
import mcppls.spec.database;

namespace mcppls::project {

namespace {

enum class TokenKind { identifier, punctuation, string, header_name, other };

struct Token {
    TokenKind kind { TokenKind::other };
    std::string_view text;
    std::size_t offset { 0 };
    bool startsLine { false };     // first token on its logical line
};

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\f' || c == '\v' || c == '\r'; }

// Where a byte of `text` is, as an editor shows the text: a byte order mark at its start takes no
// column (fix plan F2), so a file read from disk and the same file open in an editor get the same ranges.
base::Position position_in(std::string_view text, std::size_t offset) {
    const std::size_t mark { base::byte_order_mark_size(text) };
    return base::position_at(text.substr(mark), offset < mark ? 0 : offset - mark);
}

// Offsets are the text's own; a byte order mark before `export module` is skipped like white space
// (fix plan F2), or the declaration would not begin its line and the file would be no module.
class Lexer {
private:
    std::string_view text_;
    std::size_t at_ { 0 };
    bool lineStart_ { true };
    int conditionalDepth_ { 0 };

public:
    explicit Lexer(std::string_view text) : text_ { text }, at_ { base::byte_order_mark_size(text) } {}

    int conditional_depth() const { return conditionalDepth_; }

    // The next token outside comments, literals' interiors and preprocessor directives.
    std::optional<Token> next(bool wantHeaderName) {
        while (at_ < text_.size()) {
            const char c { text_[at_] };
            if (c == '\n') {
                lineStart_ = true;
                ++at_;
                continue;
            }
            if (is_space(c)) {
                ++at_;
                continue;
            }
            if (c == '\\' && at_ + 1 < text_.size() && (text_[at_ + 1] == '\n' || text_[at_ + 1] == '\r')) {
                at_ += 2;
                continue;
            }
            if (c == '/' && at_ + 1 < text_.size() && text_[at_ + 1] == '/') {
                skip_line_comment_();
                continue;
            }
            if (c == '/' && at_ + 1 < text_.size() && text_[at_ + 1] == '*') {
                const std::size_t end { text_.find("*/", at_ + 2) };
                at_ = end == std::string_view::npos ? text_.size() : end + 2;
                continue;
            }
            if (c == '#' && lineStart_) {
                directive_();
                continue;
            }
            const bool startsLine { lineStart_ };
            lineStart_ = false;
            const std::size_t begin { at_ };

            if (wantHeaderName && c == '<') {
                const std::size_t close { text_.find_first_of(">\n", at_ + 1) };
                if (close != std::string_view::npos && text_[close] == '>') {
                    at_ = close + 1;
                    return Token { TokenKind::header_name, text_.substr(begin, at_ - begin), begin, startsLine };
                }
            }
            if (c == 'R' && at_ + 1 < text_.size() && text_[at_ + 1] == '"') {
                raw_string_(at_ + 1);
                return Token { TokenKind::string, text_.substr(begin, at_ - begin), begin, startsLine };
            }
            if (c == '"') {
                quoted_('"');
                return Token { TokenKind::string, text_.substr(begin, at_ - begin), begin, startsLine };
            }
            if (c == '\'') {
                quoted_('\'');
                return Token { TokenKind::other, text_.substr(begin, at_ - begin), begin, startsLine };
            }
            if (base::is_identifier_start(c)) {
                while (at_ < text_.size() && base::is_identifier_char(text_[at_])) ++at_;
                // An identifier directly followed by a quote is an encoding prefix (u8"x").
                if (at_ < text_.size() && (text_[at_] == '"' || text_[at_] == '\'')) {
                    const std::string_view prefix { text_.substr(begin, at_ - begin) };
                    if (prefix == "u8" || prefix == "u" || prefix == "U" || prefix == "L") {
                        quoted_(text_[at_]);
                        return Token { TokenKind::string, text_.substr(begin, at_ - begin), begin, startsLine };
                    }
                    if (prefix == "u8R" || prefix == "uR" || prefix == "UR" || prefix == "LR") {
                        raw_string_(at_);
                        return Token { TokenKind::string, text_.substr(begin, at_ - begin), begin, startsLine };
                    }
                }
                return Token { TokenKind::identifier, text_.substr(begin, at_ - begin), begin, startsLine };
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
                return Token { TokenKind::other, text_.substr(begin, at_ - begin), begin, startsLine };
            }
            ++at_;
            return Token { TokenKind::punctuation, text_.substr(begin, 1), begin, startsLine };
        }
        return std::nullopt;
    }

private:
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

struct NameParse {
    std::string module;
    std::string partition;
    std::size_t begin { 0 };
    std::size_t end { 0 };
    bool ok { false };
};

// module-name: identifier ( '.' identifier )* ; partition: ':' module-name
NameParse parse_name(Lexer& lexer, std::optional<Token>& token, bool allowLeadingPartition) {
    NameParse result;
    auto read_dotted = [&](std::string& out) {
        if (!token || token->kind != TokenKind::identifier) return false;
        out.append(token->text);
        result.end = token->offset + token->text.size();
        token = lexer.next(false);
        while (token && token->kind == TokenKind::punctuation && token->text == ".") {
            token = lexer.next(false);
            if (!token || token->kind != TokenKind::identifier) return false;
            out += '.';
            out.append(token->text);
            result.end = token->offset + token->text.size();
            token = lexer.next(false);
        }
        return true;
    };
    if (!token) return result;
    result.begin = token->offset;
    if (token->kind == TokenKind::punctuation && token->text == ":") {
        if (!allowLeadingPartition) return result;
        token = lexer.next(false);
        result.ok = read_dotted(result.partition);
        return result;
    }
    if (!read_dotted(result.module)) return result;
    if (token && token->kind == TokenKind::punctuation && token->text == ":") {
        token = lexer.next(false);
        if (!read_dotted(result.partition)) return result;
    }
    result.ok = true;
    return result;
}

void skip_attributes(Lexer& lexer, std::optional<Token>& token) {
    while (token && token->kind == TokenKind::punctuation && token->text == "[") {
        int depth { 0 };
        do {
            if (token->text == "[") ++depth;
            if (token->text == "]") --depth;
            token = lexer.next(false);
        } while (token && depth > 0);
    }
}

// module-name, read leniently: whatever complete identifiers were read before the name broke off
// (a trailing dot, a token that is not an identifier, the end of the file) -- and never across a
// physical line, even if the raw token stream would otherwise happily continue past a newline (as
// scan_source's own parse_name does; that is fine there, since an incomplete name there is simply
// not recorded, but here it would make a token that spans two lines, which LSP does not allow).
// Advances `token` to wherever reading stopped, so the caller's own scan can go on from there.
std::optional<std::pair<std::size_t, std::size_t>> read_dotted_lenient(std::string_view text, Lexer& lexer, std::optional<Token>& token) {
    if (!token || token->kind != TokenKind::identifier) return std::nullopt;
    const std::size_t begin { token->offset };
    const std::size_t lineEnd { [&] {
        const auto newline = text.find('\n', begin);
        return newline == std::string_view::npos ? text.size() : newline;
    }() };
    std::size_t end { token->offset + token->text.size() };
    token = lexer.next(false);
    while (token && token->kind == TokenKind::punctuation && token->text == "." && token->offset <= lineEnd) {
        std::optional<Token> afterDot { lexer.next(false) };
        if (!afterDot || afterDot->kind != TokenKind::identifier || afterDot->offset > lineEnd) {
            token = afterDot;
            break;
        }
        end = afterDot->offset + afterDot->text.size();
        token = lexer.next(false);
    }
    return std::make_pair(begin, end);
}

} // namespace

std::vector<SyntaxToken> scan_syntax_tokens(std::string_view text) {
    std::vector<SyntaxToken> tokens;
    Lexer lexer { text };
    int braceDepth { 0 };
    std::optional<Token> token { lexer.next(false) };
    const auto push = [&](SyntaxTokenKind kind, std::size_t begin, std::size_t end, bool isDeclaration = false) {
        if (end <= begin) return;
        tokens.push_back(SyntaxToken { kind, base::Range { position_in(text, begin), position_in(text, end) }, isDeclaration });
    };
    while (token) {
        if (token->kind == TokenKind::punctuation) {
            if (token->text == "{") ++braceDepth;
            if (token->text == "}" && braceDepth > 0) --braceDepth;
            token = lexer.next(false);
            continue;
        }
        if (token->kind != TokenKind::identifier || braceDepth != 0) {
            token = lexer.next(false);
            continue;
        }
        bool exported { false };
        std::size_t exportBegin { 0 };
        std::size_t exportEnd { 0 };
        if (token->text == "export" && token->startsLine) {
            exportBegin = token->offset;
            exportEnd = token->offset + token->text.size();
            token = lexer.next(false);
            if (!token || token->kind != TokenKind::identifier || (token->text != "module" && token->text != "import")) continue;
            exported = true;
        } else if (!token->startsLine || (token->text != "module" && token->text != "import")) {
            token = lexer.next(false);
            continue;
        }
        const bool isImport { token->text == "import" };
        if (exported) push(SyntaxTokenKind::keyword, exportBegin, exportEnd);
        push(SyntaxTokenKind::keyword, token->offset, token->offset + token->text.size());
        token = lexer.next(isImport);
        if (!token) break;

        if (isImport && (token->kind == TokenKind::header_name || token->kind == TokenKind::string)) {
            // `import <header>;` / `import "header";`: the header text is a string/header-name
            // literal, not a module-type token; other layers already color it.
            token = lexer.next(false);
            continue;
        }
        if (isImport && token->kind == TokenKind::punctuation && token->text == ":") {
            // `import :partition;`: a partition of the current translation unit's own module, no
            // module name of its own.
            token = lexer.next(false);
            if (const auto partition = read_dotted_lenient(text, lexer, token)) push(SyntaxTokenKind::partitionName, partition->first, partition->second);
            continue;
        }
        const auto name = read_dotted_lenient(text, lexer, token);
        if (name) push(SyntaxTokenKind::moduleName, name->first, name->second, !isImport);
        // A colon only introduces a partition once a module name was actually read: `module
        // :private;`'s colon is the private-module-fragment syntax, not `module`'s own partition.
        if (name && token && token->kind == TokenKind::punctuation && token->text == ":") {
            token = lexer.next(false);
            if (const auto partition = read_dotted_lenient(text, lexer, token)) push(SyntaxTokenKind::partitionName, partition->first, partition->second, !isImport);
        }
    }
    return tokens;
}

ScanResult scan_source(std::string_view text) {
    ScanResult result;
    Lexer lexer { text };
    int braceDepth { 0 };
    std::optional<Token> token { lexer.next(false) };
    while (token) {
        if (token->kind == TokenKind::punctuation) {
            if (token->text == "{") ++braceDepth;
            if (token->text == "}" && braceDepth > 0) --braceDepth;
            token = lexer.next(false);
            continue;
        }
        if (token->kind != TokenKind::identifier || braceDepth != 0) {
            token = lexer.next(false);
            continue;
        }
        bool exported { false };
        if (token->text == "export" && token->startsLine) {
            exported = true;
            token = lexer.next(false);
            if (!token || token->kind != TokenKind::identifier || (token->text != "module" && token->text != "import")) continue;
        } else if (!token->startsLine || (token->text != "module" && token->text != "import")) {
            token = lexer.next(false);
            continue;
        }
        const bool isImport { token->text == "import" };
        const bool conditional { lexer.conditional_depth() > 0 };
        token = lexer.next(isImport);
        if (!token) break;

        if (isImport && (token->kind == TokenKind::header_name || token->kind == TokenKind::string)) {
            ImportDeclaration import;
            import.isExported = exported;
            import.isHeaderUnit = true;
            import.header = std::string { token->text };
            import.conditional = conditional;
            import.nameRange = base::Range { position_in(text, token->offset), position_in(text, token->offset + token->text.size()) };
            token = lexer.next(false);
            skip_attributes(lexer, token);
            if (token && token->text == ";") {
                result.imports.push_back(std::move(import));
                if (conditional) result.uncertain = true;
                token = lexer.next(false);
            }
            continue;
        }
        // A contextual keyword is a declaration only when a name or partition follows.
        const bool nameFollows { token->kind == TokenKind::identifier || (isImport && token->kind == TokenKind::punctuation && token->text == ":") };
        if (!nameFollows) continue;
        NameParse name { parse_name(lexer, token, isImport) };
        if (!name.ok) continue;
        skip_attributes(lexer, token);
        if (!token || token->kind != TokenKind::punctuation || token->text != ";") continue;
        token = lexer.next(false);

        const base::Range range { position_in(text, name.begin), position_in(text, name.end) };
        if (conditional) result.uncertain = true;
        if (isImport) {
            result.imports.push_back(ImportDeclaration { name.module, name.partition, exported, false, {}, conditional, range });
        } else if (!result.declaration) {
            result.declaration = ModuleDeclaration { name.module, name.partition, exported, conditional, range };
        } else {
            result.uncertain = true;
        }
    }
    return result;
}

spec::Role role_of(const ScanResult& result) {
    if (!result.declaration) return result.uncertain ? spec::Role::unknown : spec::Role::non_module;
    const auto& declaration = *result.declaration;
    if (declaration.conditional) return spec::Role::unknown;
    if (declaration.isExported) {
        return declaration.partition.empty() ? spec::Role::module_interface : spec::Role::module_partition_interface;
    }
    return declaration.partition.empty() ? spec::Role::module_implementation : spec::Role::module_partition_implementation;
}

std::string provided_name(const ScanResult& result) {
    if (!result.declaration) return {};
    const spec::Role role { role_of(result) };
    if (role == spec::Role::module_implementation || role == spec::Role::non_module) return {};
    const auto& declaration = *result.declaration;
    return declaration.partition.empty() ? declaration.module : declaration.module + ":" + declaration.partition;
}

std::string imported_name(const ScanResult& result, const ImportDeclaration& import) {
    if (import.isHeaderUnit) return import.header;
    if (!import.module.empty()) return import.partition.empty() ? import.module : import.module + ":" + import.partition;
    if (!result.declaration) return ":" + import.partition;
    return result.declaration->module + ":" + import.partition;
}

bool is_module_name(std::string_view name) {
    const auto dotted = [](std::string_view part) {
        if (part.empty()) return false;
        bool atStart { true };
        for (const char c : part) {
            if (c == '.') {
                if (atStart) return false;
                atStart = true;
                continue;
            }
            const bool utf8 { static_cast<unsigned char>(c) >= 0x80 };
            if (!utf8 && !base::is_identifier_char(c)) return false;
            if (atStart && c >= '0' && c <= '9') return false;
            atStart = false;
        }
        return !atStart;
    };
    const std::size_t colon { name.find(':') };
    if (colon == std::string_view::npos) return dotted(name);
    return dotted(name.substr(0, colon)) && dotted(name.substr(colon + 1));
}

std::vector<std::string> required_names(const ScanResult& result) {
    std::vector<std::string> names;
    auto add = [&](std::string name) {
        if (std::ranges::find(names, name) == names.end()) names.push_back(std::move(name));
    };
    if (result.declaration && !result.declaration->isExported && result.declaration->partition.empty()) {
        add(result.declaration->module);
    }
    for (const auto& import : result.imports) {
        if (import.isHeaderUnit) continue;
        add(imported_name(result, import));
    }
    return names;
}

bool is_cxx_source_name(std::string_view path) {
    static constexpr std::array<std::string_view, 12> EXTENSIONS {
        ".cpp", ".cc", ".cxx", ".c++", ".cppm", ".ccm", ".cxxm", ".c++m", ".ixx", ".mpp", ".mxx", ".cp",
    };
    const std::string_view extension { base::extension(path) };
    return std::ranges::any_of(EXTENSIONS, [&](std::string_view candidate) { return base::iequals_ascii(candidate, extension); });
}

} // namespace mcppls::project
