module mcppls.engine.clangd.definition;

import std;
import mcppls.base.path;
import mcppls.base.text;

namespace mcppls::engine::clangd {

namespace {

std::string_view stem_of(std::string_view path) {
    std::string_view name { base::file_name(path) };
    if (const std::size_t dot { name.find('.') }; dot != std::string_view::npos) name = name.substr(0, dot);
    return name;
}

std::size_t shared_directories(std::string_view left, std::string_view right) {
    std::size_t shared { 0 };
    std::size_t i { 0 };
    for (; i < left.size() && i < right.size() && left[i] == right[i]; ++i) {
        if (left[i] == '/') ++shared;
    }
    return shared;
}

// ---- N-8: lexical scanning shared by declared_function_at, function_definitions, units_defining ----

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

// Skips one comment, string/character literal or raw string starting at `i` (returns `i` unchanged when
// nothing starts there), and one preprocessor line when `i` is a `#` with only whitespace before it since
// the last newline. The caller resumes scanning at what this returns.
std::size_t skip_non_code(std::string_view text, std::size_t i) {
    const std::size_t n { text.size() };
    if (i >= n) return i;
    const char c { text[i] };
    const char next { i + 1 < n ? text[i + 1] : '\0' };
    if (c == '/' && next == '/') {
        const auto nl = text.find('\n', i);
        return nl == std::string_view::npos ? n : nl;
    }
    if (c == '/' && next == '*') {
        const auto end = text.find("*/", i + 2);
        return end == std::string_view::npos ? n : end + 2;
    }
    if (c == 'R' && next == '"') {
        std::size_t p { i + 2 };
        const std::size_t delimStart { p };
        while (p < n && p - delimStart < 16 && text[p] != '(') ++p;
        if (p >= n || text[p] != '(') return i + 1;   // not a raw string after all -- just step past 'R'
        const std::string_view delim { text.substr(delimStart, p - delimStart) };
        const std::string closer { std::string { ")" } + std::string { delim } + "\"" };
        const auto end = text.find(closer, p + 1);
        return end == std::string_view::npos ? n : end + closer.size();
    }
    if (c == '"' || c == '\'') {
        std::size_t p { i + 1 };
        for (; p < n && text[p] != c; ++p) {
            if (text[p] == '\\') ++p;
            if (p < n && text[p] == '\n') break;
        }
        return p < n && text[p] == c ? p + 1 : p;
    }
    if (c == '#') {
        std::size_t back { i };
        bool onlyWhitespaceBefore { true };
        while (back > 0 && text[back - 1] != '\n') {
            if (!is_space(text[back - 1])) { onlyWhitespaceBefore = false; break; }
            --back;
        }
        if (onlyWhitespaceBefore) {
            std::size_t p { i };
            while (true) {
                const auto nl = text.find('\n', p);
                if (nl == std::string_view::npos) return n;
                std::size_t last { nl };
                while (last > p && is_space(text[last - 1]) && text[last - 1] != '\\') --last;
                if (last > p && text[last - 1] == '\\') { p = nl + 1; continue; }   // a continued directive
                return nl + 1;
            }
        }
    }
    return i;
}

// Splits a declarator's qualification (`hello::add`, `mcpp::build::phase0_manifest_and_workspace`) into
// its identifiers, in order. Whitespace and `::` between identifiers are the only separators understood.
std::vector<std::string> split_scope_name(std::string_view s) {
    std::vector<std::string> parts;
    std::size_t i { 0 };
    while (true) {
        const std::size_t start { i };
        while (i < s.size() && mcppls::base::is_identifier_char(s[i])) ++i;
        if (i == start) break;
        parts.push_back(std::string { s.substr(start, i - start) });
        while (i < s.size() && is_space(s[i])) ++i;
        if (i + 1 < s.size() && s[i] == ':' && s[i + 1] == ':') { i += 2; while (i < s.size() && is_space(s[i])) ++i; }
        else break;
    }
    return parts;
}

// The names a `{` at `bracePos` opens, from the header text since `from` (the position right after the
// previous top-level `;`, `{` or `}`): what a `namespace`/`class`/`struct` header introduces, or none for
// anything else (a function body, a control-flow block, an initializer, `export { }`, `extern "C" { }`,
// ...). `export`/`inline` prefixes and one `template <...>` clause ahead of `class`/`struct` are skipped.
std::vector<std::string> header_scope_segments(std::string_view text, std::size_t from, std::size_t bracePos) {
    std::string_view header { mcppls::base::trim(text.substr(from, bracePos - from)) };
    while (true) {
        if (header.starts_with("export") && (header.size() == 6 || is_space(header[6]))) header = mcppls::base::trim(header.substr(6));
        else if (header.starts_with("inline") && (header.size() == 6 || is_space(header[6]))) header = mcppls::base::trim(header.substr(6));
        else break;
    }
    if (header.empty()) return {};   // `export { ... }`, `inline { ... }`
    if (header.starts_with("namespace") && (header.size() == 9 || is_space(header[9]) || header[9] == '{')) {
        return split_scope_name(mcppls::base::trim(header.substr(9)));   // empty for an anonymous namespace
    }
    if (header.starts_with("template")) {
        if (const auto lt = header.find('<'); lt != std::string_view::npos) {
            int depth { 0 };
            std::size_t p { lt };
            for (; p < header.size(); ++p) {
                if (header[p] == '<') ++depth;
                else if (header[p] == '>' && --depth == 0) { ++p; break; }
            }
            header = mcppls::base::trim(header.substr(std::min(p, header.size())));
        }
    }
    for (const std::string_view keyword : { std::string_view { "class" }, std::string_view { "struct" } }) {
        if (!header.starts_with(keyword) || header.size() <= keyword.size() || !is_space(header[keyword.size()])) continue;
        const std::string_view rest { mcppls::base::trim(header.substr(keyword.size())) };
        std::size_t end { 0 };
        while (end < rest.size() && mcppls::base::is_identifier_char(rest[end])) ++end;
        return end == 0 ? std::vector<std::string> {} : std::vector<std::string> { std::string { rest.substr(0, end) } };
    }
    return {};
}

// The running state of the brace/segment scan `function_definitions` and `scope_stack_before` share.
struct ScopeState {
    std::vector<std::string> scopeStack;
    std::vector<int> frameCounts;   // how many of scopeStack's entries close with each open frame
    std::size_t segmentStart { 0 };
};

void cross_structural_char(std::string_view text, std::size_t i, char c, ScopeState& state) {
    if (c == ';') { state.segmentStart = i + 1; return; }
    if (c == '{') {
        auto segments = header_scope_segments(text, state.segmentStart, i);
        state.frameCounts.push_back(static_cast<int>(segments.size()));
        for (auto& s : segments) state.scopeStack.push_back(std::move(s));
        state.segmentStart = i + 1;
        return;
    }
    // c == '}'
    if (!state.frameCounts.empty()) {
        int count { state.frameCounts.back() };
        state.frameCounts.pop_back();
        while (count-- > 0 && !state.scopeStack.empty()) state.scopeStack.pop_back();
    }
    state.segmentStart = i + 1;
}

// The namespaces and classes enclosing byte offset `pos` -- the same bookkeeping `function_definitions`
// does over a whole file, run just far enough for a single declaration.
std::vector<std::string> scope_stack_before(std::string_view text, std::size_t pos) {
    ScopeState state;
    const std::size_t end { std::min(pos, text.size()) };
    std::size_t i { 0 };
    while (i < end) {
        if (const std::size_t j { skip_non_code(text, i) }; j != i) { i = std::min(j, end); continue; }
        const char c { text[i] };
        if (c == ';' || c == '{' || c == '}') cross_structural_char(text, i, c, state);
        ++i;
    }
    return std::move(state.scopeStack);
}

// The index one past the `)` matching the `(` at `openParen`, skipping nested parentheses, comments and
// literals; nullopt when the text ends unbalanced.
std::optional<std::size_t> matching_close_paren(std::string_view text, std::size_t openParen) {
    int depth { 0 };
    std::size_t i { openParen };
    const std::size_t n { text.size() };
    while (i < n) {
        if (const std::size_t j { skip_non_code(text, i) }; j != i) { i = j; continue; }
        if (text[i] == '(') ++depth;
        else if (text[i] == ')' && --depth == 0) return i;
        ++i;
    }
    return std::nullopt;
}

bool is_type_keyword(std::string_view word) {
    static const std::set<std::string_view, std::less<>> keywords {
        "void", "bool", "char", "char8_t", "char16_t", "char32_t", "wchar_t",
        "short", "int", "long", "signed", "unsigned", "float", "double", "auto",
    };
    return keywords.contains(word);
}

// One parameter's normalized spelling: whitespace collapsed, and no space around `*`, `&`, `&&`, `<`,
// `>`, `,`, `::` (a `,` can remain here inside a nested `<...>` the caller did not split on).
std::string normalize_spacing(std::string_view s) {
    std::vector<std::string> tokens;
    std::size_t i { 0 };
    while (i < s.size()) {
        if (const std::size_t j { skip_non_code(s, i) }; j != i) { i = j; continue; }
        const char c { s[i] };
        if (is_space(c)) { ++i; continue; }
        if (mcppls::base::is_identifier_char(c)) {
            const std::size_t start { i };
            while (i < s.size() && mcppls::base::is_identifier_char(s[i])) ++i;
            tokens.push_back(std::string { s.substr(start, i - start) });
            continue;
        }
        if (c == ':' && i + 1 < s.size() && s[i + 1] == ':') { tokens.emplace_back("::"); i += 2; continue; }
        if (c == '&' && i + 1 < s.size() && s[i + 1] == '&') { tokens.emplace_back("&&"); i += 2; continue; }
        tokens.push_back(std::string(1, c));   // braces here would pick initializer_list<char>, not the fill constructor
        ++i;
    }
    static const std::set<std::string_view, std::less<>> tightBefore { "*", "&", "&&", ",", "<", ">", "::", ")", "]" };
    static const std::set<std::string_view, std::less<>> tightAfter { "*", "&", "&&", "<", "::", "(", "[" };
    std::string out;
    for (std::size_t k { 0 }; k < tokens.size(); ++k) {
        if (k > 0 && !tightBefore.contains(std::string_view { tokens[k] }) && !tightAfter.contains(std::string_view { tokens[k - 1] })) out += ' ';
        out += tokens[k];
    }
    return out;
}

// One parameter, as written between two top-level commas (or the whole list, for a single parameter):
// its default argument dropped, its name dropped when the parameter has more than one token and the
// last identifier is neither preceded by `::` (the tail of a qualified type, not a name) nor a type
// keyword, and its spacing normalized. `int (*fp)(int)` is left alone: a function-pointer parameter's
// last identifier is already its name, wrapped in the declarator, not a trailing word to drop.
std::string normalize_one_parameter(std::string_view part) {
    {   // Drop a default argument: the first top-level '=' (outside <>, (), [], {}).
        int angle { 0 }, paren { 0 }, bracket { 0 }, brace { 0 };
        std::size_t i { 0 };
        while (i < part.size()) {
            if (const std::size_t j { skip_non_code(part, i) }; j != i) { i = j; continue; }
            const char c { part[i] };
            if (c == '<') ++angle;
            else if (c == '>') { if (angle > 0) --angle; }
            else if (c == '(') ++paren;
            else if (c == ')') { if (paren > 0) --paren; }
            else if (c == '[') ++bracket;
            else if (c == ']') { if (bracket > 0) --bracket; }
            else if (c == '{') ++brace;
            else if (c == '}') { if (brace > 0) --brace; }
            else if (c == '=' && angle == 0 && paren == 0 && bracket == 0 && brace == 0) { part = part.substr(0, i); break; }
            ++i;
        }
    }
    part = mcppls::base::trim(part);
    if (!part.empty() && !part.contains("(*")) {
        const std::size_t idEnd { part.size() };
        std::size_t idStart { idEnd };
        while (idStart > 0 && mcppls::base::is_identifier_char(part[idStart - 1])) --idStart;
        if (idStart < idEnd) {
            std::size_t before { idStart };
            while (before > 0 && is_space(part[before - 1])) --before;
            const bool afterScope { before >= 2 && part[before - 1] == ':' && part[before - 2] == ':' };
            const std::string_view lastWord { part.substr(idStart, idEnd - idStart) };
            if (before > 0 && !afterScope && !is_type_keyword(lastWord)) part = mcppls::base::trim(part.substr(0, idStart));
        }
    }
    return normalize_spacing(part);
}

// Splits a parameter list's raw text at its top-level commas (respecting `<>`, `()`, `[]`, `{}`), then
// normalizes each part. `void` alone, or an empty list, normalizes to no parameters at all.
std::vector<std::string> normalize_parameters(std::string_view rawList) {
    rawList = mcppls::base::trim(rawList);
    if (rawList.empty() || rawList == "void") return {};
    std::vector<std::string> parameters;
    int angle { 0 }, paren { 0 }, bracket { 0 }, brace { 0 };
    std::size_t start { 0 }, i { 0 };
    while (i < rawList.size()) {
        if (const std::size_t j { skip_non_code(rawList, i) }; j != i) { i = j; continue; }
        const char c { rawList[i] };
        if (c == '<') ++angle;
        else if (c == '>') { if (angle > 0) --angle; }
        else if (c == '(') ++paren;
        else if (c == ')') { if (paren > 0) --paren; }
        else if (c == '[') ++bracket;
        else if (c == ']') { if (bracket > 0) --bracket; }
        else if (c == '{') ++brace;
        else if (c == '}') { if (brace > 0) --brace; }
        else if (c == ',' && angle == 0 && paren == 0 && bracket == 0 && brace == 0) {
            parameters.push_back(normalize_one_parameter(rawList.substr(start, i - start)));
            start = i + 1;
        }
        ++i;
    }
    parameters.push_back(normalize_one_parameter(rawList.substr(start)));
    return parameters;
}

} // namespace

DeclarationKind declaration_kind(std::string_view text, std::size_t nameEnd) {
    int parentheses { 0 };
    int brackets { 0 };
    const std::size_t end { std::min(text.size(), nameEnd + 16384) };
    for (std::size_t i { nameEnd }; i < end; ++i) {
        const char c { text[i] };
        const char next { i + 1 < end ? text[i + 1] : '\0' };
        if (c == '/' && next == '/') {
            i = text.find('\n', i);
            if (i == std::string_view::npos) return DeclarationKind::unknown;
            continue;
        }
        if (c == '/' && next == '*') {
            i = text.find("*/", i + 2);
            if (i == std::string_view::npos) return DeclarationKind::unknown;
            ++i;
            continue;
        }
        if (c == '"' || c == '\'') {
            for (++i; i < end && text[i] != c; ++i) {
                if (text[i] == '\\') ++i;
                if (i < end && text[i] == '\n') break;
            }
            continue;
        }
        if (c == '(') ++parentheses;
        else if (c == '[') ++brackets;
        else if (c == ')' || c == ']') {
            int& depth { c == ')' ? parentheses : brackets };
            if (depth == 0) return DeclarationKind::unknown;
            --depth;
        } else if (parentheses > 0 || brackets > 0) {
            continue;
        } else if (c == ';') {
            return DeclarationKind::declaration;
        } else if (c == '{' || c == '=') {
            return DeclarationKind::definition;   // a body, a class body, an initializer, = default, = delete
        } else if (c == ':') {
            if (next == ':') {
                ++i;
                continue;
            }
            return DeclarationKind::definition;   // a constructor's initializers, a base clause
        } else if (c == '}') {
            return DeclarationKind::unknown;
        }
    }
    return DeclarationKind::unknown;
}

std::vector<std::string> units_to_search(std::string_view interfacePath, std::span<const UnitOfModule> units, std::size_t limit) {
    std::vector<const UnitOfModule*> ordered;
    for (const auto& unit : units) ordered.push_back(&unit);
    const std::string_view stem { stem_of(interfacePath) };
    const std::string directory { base::parent_path(interfacePath) };
    auto rank = [&](const UnitOfModule* unit) {
        return std::tuple { unit->partition ? 1 : 0, stem_of(unit->path) == stem ? 0 : 1,
                            -static_cast<long>(shared_directories(base::parent_path(unit->path), directory)), std::string_view { unit->path } };
    };
    std::ranges::sort(ordered, [&](const UnitOfModule* a, const UnitOfModule* b) { return rank(a) < rank(b); });
    std::vector<std::string> chosen;
    for (const auto* unit : ordered) {
        if (chosen.size() >= limit) break;
        chosen.push_back(unit->path);
    }
    return chosen;
}

std::optional<DeclaredFunction> declared_function_at(std::string_view text, std::size_t nameOffset) {
    if (nameOffset >= text.size() || !base::is_identifier_start(text[nameOffset])) return std::nullopt;
    std::size_t nameEnd { nameOffset };
    while (nameEnd < text.size() && base::is_identifier_char(text[nameEnd])) ++nameEnd;
    if (declaration_kind(text, nameEnd) != DeclarationKind::declaration) return std::nullopt;
    std::size_t k { nameEnd };
    while (true) {
        if (const std::size_t j { skip_non_code(text, k) }; j != k) { k = j; continue; }
        if (k < text.size() && is_space(text[k])) { ++k; continue; }
        break;
    }
    if (k >= text.size() || text[k] != '(') return std::nullopt;   // the name is not a declarator at all
    const auto close = matching_close_paren(text, k);
    DeclaredFunction declared;
    declared.name = std::string { text.substr(nameOffset, nameEnd - nameOffset) };
    declared.scopes = scope_stack_before(text, nameOffset);
    declared.parameters = close ? normalize_parameters(text.substr(k + 1, *close - k - 1)) : std::vector<std::string> {};
    return declared;
}

std::vector<FunctionDefinition> function_definitions(std::string_view text, std::string_view name) {
    std::vector<FunctionDefinition> found;
    if (name.empty()) return found;
    ScopeState state;
    const std::size_t n { text.size() };
    std::size_t i { 0 };
    while (i < n) {
        if (const std::size_t j { skip_non_code(text, i) }; j != i) { i = j; continue; }
        const char c { text[i] };
        if (c == ';' || c == '{' || c == '}') { cross_structural_char(text, i, c, state); ++i; continue; }
        const std::size_t wordStart { i };
        if (c == '~') {
            if (i + 1 >= n || !base::is_identifier_start(text[i + 1])) { ++i; continue; }
            ++i;   // a destructor's name is "~Class" -- the identifier scan below extends the token over it
        } else if (!base::is_identifier_start(c)) {
            ++i;
            continue;
        }
        while (i < n && base::is_identifier_char(text[i])) ++i;
        const std::string_view word { text.substr(wordStart, i - wordStart) };
        if (word != name) continue;
        // Followed, after whitespace and comments, directly by '(': a call or a declarator, never a plain
        // use of the name (a type, a variable, a member access) -- `declaration_kind` cannot tell those apart.
        std::size_t k { i };
        while (true) {
            if (const std::size_t j { skip_non_code(text, k) }; j != k) { k = j; continue; }
            if (k < n && is_space(text[k])) { ++k; continue; }
            break;
        }
        if (k >= n || text[k] != '(') continue;
        if (declaration_kind(text, i) != DeclarationKind::definition) continue;
        // The declarator's own qualification: identifiers joined by "::" immediately before the name.
        std::vector<std::string> qualifiers;
        std::size_t back { wordStart };
        while (true) {
            std::size_t p { back };
            while (p > 0 && is_space(text[p - 1])) --p;
            if (p < 2 || text[p - 1] != ':' || text[p - 2] != ':') break;
            p -= 2;
            while (p > 0 && is_space(text[p - 1])) --p;
            const std::size_t qEnd { p };
            while (p > 0 && base::is_identifier_char(text[p - 1])) --p;
            if (p == qEnd) break;   // "::" with nothing before it -- give up rather than guess
            qualifiers.push_back(std::string { text.substr(p, qEnd - p) });
            back = p;
        }
        std::ranges::reverse(qualifiers);
        std::string qualifiedName;
        for (const auto& scope : state.scopeStack) { qualifiedName += scope; qualifiedName += "::"; }
        for (const auto& qualifier : qualifiers) { qualifiedName += qualifier; qualifiedName += "::"; }
        qualifiedName += word;
        const auto close = matching_close_paren(text, k);
        FunctionDefinition definition;
        definition.qualifiedName = std::move(qualifiedName);
        definition.name = std::string { word };
        definition.nameOffset = wordStart;
        definition.nameEnd = i;
        definition.nameRange = { base::position_at(text, wordStart), base::position_at(text, i) };
        definition.parameters = close ? normalize_parameters(text.substr(k + 1, *close - k - 1)) : std::vector<std::string> {};
        found.push_back(std::move(definition));
    }
    return found;
}

bool same_function(const DeclaredFunction& declaration, const FunctionDefinition& definition) {
    if (declaration.name != definition.name) return false;
    if (declaration.parameters != definition.parameters) return false;
    std::vector<std::string> defScopes { split_scope_name(definition.qualifiedName) };
    if (!defScopes.empty()) defScopes.pop_back();   // the name itself; the equality above already checked it
    const std::size_t shared { std::min(declaration.scopes.size(), defScopes.size()) };
    for (std::size_t k { 0 }; k < shared; ++k) {
        if (declaration.scopes[declaration.scopes.size() - 1 - k] != defScopes[defScopes.size() - 1 - k]) return false;
    }
    return true;
}

std::vector<std::string> units_defining(std::string_view name, std::span<const UnitOfModule> units,
                                        const std::function<std::optional<std::string>(const std::string&)>& read,
                                        std::size_t limit) {
    std::vector<const UnitOfModule*> defining;
    std::vector<const UnitOfModule*> rest;
    for (const auto& unit : units) {
        bool defines { false };
        if (const auto text = read(unit.path)) defines = !function_definitions(*text, name).empty();
        (defines ? defining : rest).push_back(&unit);
    }
    std::ranges::sort(defining, [](const UnitOfModule* a, const UnitOfModule* b) { return a->path < b->path; });
    std::vector<std::string> chosen;
    for (const auto* unit : defining) {
        if (chosen.size() >= limit) return chosen;
        chosen.push_back(unit->path);
    }
    for (const auto* unit : rest) {
        if (chosen.size() >= limit) break;
        chosen.push_back(unit->path);
    }
    return chosen;
}

} // namespace mcppls::engine::clangd
