module mcppls.bundle.redact;

import std;
import nlohmann.json;

namespace mcppls::bundle {

namespace {

using Json = nlohmann::json;

bool is_alpha(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }
bool is_alnum(unsigned char c) { return is_alpha(c) || is_digit(c); }
// A letter of any script (a UTF-8 byte of a multi-byte character) or a digit: what a word is made of.
bool is_word(unsigned char c) { return is_alnum(c) || c >= 0x80; }
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
char upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }

std::string lowered(std::string_view text) {
    std::string out { text };
    for (char& c : out) c = lower(c);
    return out;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// The byte a %XX escape at `i` stands for.
std::optional<char> escaped_at(std::string_view text, std::size_t i) {
    if (i + 2 >= text.size() || text[i] != '%') return std::nullopt;
    const int high { hex_value(text[i + 1]) };
    const int low { hex_value(text[i + 2]) };
    if (high < 0 || low < 0) return std::nullopt;
    return static_cast<char>(high * 16 + low);
}

// ---- paths in any spelling ---------------------------------------------------------------------
//
// One path, written the ways it reaches a log or a report: '/' or '\' separators, a backslash
// doubled (JSON) or doubled again (JSON inside JSON), "\/" (a JSON writer escaping '/'), any byte
// percent-encoded (a file:// URI: "c%3A/Users/John%20Doe"), the drive letter in either case, a
// Windows drive seen from WSL (/mnt/c/...), and a Windows profile under its 8.3 name (RUNNER~1).
// Names compare case-insensitively: Windows and macOS file systems do, and nothing is lost on Linux
// by also catching "/HOME/SPEAK".

enum class PieceKind { drive, separator, literal, short_name };

struct Piece {
    PieceKind kind { PieceKind::literal };
    std::string text;   // drive: the letter; literal: the name; short_name: the 8.3 prefix
};

struct PathPattern {
    std::vector<Piece> pieces;
    std::string rule;
    std::string replacement;
};

// The number of bytes a separator takes at `i`, 0 when there is none.
std::size_t match_separator(std::string_view text, std::size_t i) {
    if (i >= text.size()) return 0;
    if (text[i] == '/') return 1;
    if (text[i] == '\\') {
        std::size_t j { i };
        while (j < text.size() && text[j] == '\\') ++j;
        if (j < text.size() && text[j] == '/') return j + 1 - i;   // "\/", a JSON writer escaping '/'
        return j - i;
    }
    if (const auto escaped = escaped_at(text, i); escaped && (*escaped == '/' || *escaped == '\\')) return 3;
    return 0;
}

// The number of bytes `literal` takes at `i`, each of its bytes as itself or percent-encoded; 0 when it is not there.
std::size_t match_literal(std::string_view text, std::size_t i, std::string_view literal) {
    std::size_t j { i };
    for (const char expected : literal) {
        if (j < text.size() && lower(text[j]) == lower(expected)) {
            ++j;
            continue;
        }
        if (const auto escaped = escaped_at(text, j); escaped && lower(*escaped) == lower(expected)) {
            j += 3;
            continue;
        }
        return 0;
    }
    return j - i;
}

std::size_t match_short_name(std::string_view text, std::size_t i, std::string_view prefix) {
    const std::size_t head { match_literal(text, i, prefix) };
    if (head == 0) return 0;
    std::size_t j { i + head };
    if (j >= text.size() || text[j] != '~') return 0;
    ++j;
    const std::size_t digits { j };
    while (j < text.size() && is_digit(static_cast<unsigned char>(text[j]))) ++j;
    return j == digits ? 0 : j - i;
}

std::size_t match_drive(std::string_view text, std::size_t i, char letter) {
    if (i >= text.size() || lower(text[i]) != letter) return 0;
    if (i + 1 < text.size() && text[i + 1] == ':') return 2;
    if (const auto escaped = escaped_at(text, i + 1); escaped && *escaped == ':') return 4;
    return 0;
}

// Whether a name goes on at `j`: "/home/speak" is not in "/home/speaker" nor in "/home/speak.old",
// but it is in "/home/speak." at the end of a sentence and in "/home/speak/src".
bool name_continues(std::string_view text, std::size_t j) {
    if (j >= text.size()) return false;
    const auto c = static_cast<unsigned char>(text[j]);
    if (is_word(c) || c == '_') return true;
    if (c == '.' || c == '-' || c == '+' || c == '~') {
        if (j + 1 >= text.size()) return false;
        const auto next = static_cast<unsigned char>(text[j + 1]);
        return is_word(next) || next == '_';
    }
    if (const auto escaped = escaped_at(text, j)) return is_alnum(static_cast<unsigned char>(*escaped));
    return false;
}

// The end of `pattern` matched at `i`, when it is there as a whole path of its own.
// Whether a path may start at `i`: not the tail of a longer name ("/data/home/speak" does not
// contain the home "/home/speak"), but it may be glued to a compiler option of one or two letters
// ("-I/home/speak/include", "-LC:/Users/x/lib", MSVC's "/IC:\Users\x"). `drive`: the path starts
// with a drive letter, which a '/' option may come before.
bool path_starts_at(std::string_view text, std::size_t i, bool drive) {
    if (i == 0) return true;
    const auto nameChar = [](unsigned char c) { return is_word(c) || c == '_' || c == '.' || c == '-'; };
    if (!nameChar(static_cast<unsigned char>(text[i - 1]))) return true;
    std::size_t run { i };
    while (run > 0 && is_alpha(static_cast<unsigned char>(text[run - 1]))) --run;
    const std::size_t length { i - run };
    if (length == 0 || length > 2 || run == 0) return false;
    return text[run - 1] == '-' || (drive && text[run - 1] == '/');
}

std::optional<std::size_t> match_path(std::string_view text, std::size_t i, const PathPattern& pattern) {
    if (pattern.pieces.empty()) return std::nullopt;
    if (!path_starts_at(text, i, pattern.pieces.front().kind == PieceKind::drive)) return std::nullopt;
    std::size_t j { i };
    for (const auto& piece : pattern.pieces) {
        std::size_t taken { 0 };
        switch (piece.kind) {
        case PieceKind::drive: taken = match_drive(text, j, piece.text.front()); break;
        case PieceKind::separator: taken = match_separator(text, j); break;
        case PieceKind::literal: taken = match_literal(text, j, piece.text); break;
        case PieceKind::short_name: taken = match_short_name(text, j, piece.text); break;
        }
        if (taken == 0) return std::nullopt;
        j += taken;
    }
    if (name_continues(text, j)) return std::nullopt;
    return j;
}

// Whether a pattern could start at `i`, cheaply, before match_path looks.
bool could_start(std::string_view text, std::size_t i, const PathPattern& pattern) {
    const Piece& first { pattern.pieces.front() };
    const char c { text[i] };
    if (first.kind == PieceKind::drive) return lower(c) == first.text.front();
    return c == '/' || c == '\\' || c == '%';
}

// The segments of a '/'-separated path, empty ones dropped.
std::vector<std::string> segments_of(std::string_view path) {
    std::vector<std::string> segments;
    std::string current;
    for (const char c : path) {
        if (c == '/' || c == '\\') {
            if (!current.empty()) segments.push_back(std::move(current));
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) segments.push_back(std::move(current));
    return segments;
}

bool is_windows_path(std::string_view path) {
    return path.size() >= 2 && is_alpha(static_cast<unsigned char>(path[0])) && path[1] == ':';
}

PathPattern pattern_of(std::string_view path, std::string rule, std::string replacement) {
    PathPattern pattern { {}, std::move(rule), std::move(replacement) };
    std::vector<std::string> segments;
    if (is_windows_path(path)) {
        pattern.pieces.push_back(Piece { PieceKind::drive, std::string(1, lower(path[0])) });
        segments = segments_of(path.substr(2));
    } else {
        segments = segments_of(path);
    }
    for (auto& segment : segments) {
        pattern.pieces.push_back(Piece { PieceKind::separator, {} });
        pattern.pieces.push_back(Piece { PieceKind::literal, std::move(segment) });
    }
    return pattern;
}

// The 8.3 name Windows gives a directory whose own name is not one (longer than eight bytes, or
// with a space or a second dot): its first six valid characters, upper case, then ~N.
std::optional<std::string> short_name_prefix(std::string_view name) {
    std::string valid;
    for (const char c : name) {
        if (c == ' ' || c == '.' || std::string_view { "\"*+,/:;<=>?[\\]|" }.contains(c)) continue;
        valid += upper(c);
    }
    const bool needsOne { name.size() > 8 || name.contains(' ') || std::ranges::count(name, '.') > 1 || valid.size() != name.size() };
    if (!needsOne || valid.size() < 2) return std::nullopt;
    return valid.substr(0, 6);
}

// Every spelling of a path worth looking for: itself, and for a Windows path the WSL view of it
// and, for its last component, the 8.3 name.
std::vector<PathPattern> path_patterns(std::string_view path, std::string_view rule, std::string_view replacement) {
    std::vector<PathPattern> patterns;
    patterns.push_back(pattern_of(path, std::string { rule }, std::string { replacement }));
    if (is_windows_path(path)) {
        const std::string wsl { std::format("/mnt/{}{}", lower(path[0]), path.substr(2)) };
        patterns.push_back(pattern_of(wsl, std::string { rule }, std::string { replacement }));
        auto shortened = patterns.front();
        auto& last = shortened.pieces.back();
        if (last.kind == PieceKind::literal) {
            if (auto prefix = short_name_prefix(last.text)) {
                last = Piece { PieceKind::short_name, std::move(*prefix) };
                patterns.push_back(std::move(shortened));
            }
        }
    }
    return patterns;
}

// Replaces every match of `pattern`; returns how many there were.
std::size_t replace_path(std::string& text, const PathPattern& pattern) {
    if (pattern.pieces.empty() || text.empty()) return 0;
    std::string out;
    std::size_t count { 0 };
    std::size_t copied { 0 };
    for (std::size_t i { 0 }; i < text.size();) {
        if (could_start(text, i, pattern)) {
            if (const auto end = match_path(text, i, pattern)) {
                if (out.empty()) out.reserve(text.size());
                out.append(text, copied, i - copied);
                out += pattern.replacement;
                copied = i = *end;
                ++count;
                continue;
            }
        }
        ++i;
    }
    if (count == 0) return 0;
    out.append(text, copied);
    text = std::move(out);
    return count;
}

std::optional<std::size_t> find_path(std::string_view text, const PathPattern& pattern) {
    if (pattern.pieces.empty()) return std::nullopt;
    for (std::size_t i { 0 }; i < text.size(); ++i) {
        if (could_start(text, i, pattern) && match_path(text, i, pattern)) return i;
    }
    return std::nullopt;
}

// ---- words -------------------------------------------------------------------------------------

// Case-insensitive search for `needle` from `from`.
std::size_t ifind(std::string_view haystack, std::string_view needle, std::size_t from = 0) {
    if (needle.empty() || haystack.size() < needle.size()) return std::string_view::npos;
    const char first { lower(needle.front()) };
    for (std::size_t i { from }; i + needle.size() <= haystack.size(); ++i) {
        if (lower(haystack[i]) != first) continue;
        bool same { true };
        for (std::size_t k { 1 }; k < needle.size() && same; ++k) same = lower(haystack[i + k]) == lower(needle[k]);
        if (same) return i;
    }
    return std::string_view::npos;
}

// A whole word: neither neighbour a letter or digit. '_', '-' and '.' separate words here, so
// "speak_dev" and "speak.log" name the user "speak", "speaker" does not.
bool whole_word_at(std::string_view text, std::size_t at, std::size_t length) {
    if (at > 0 && is_word(static_cast<unsigned char>(text[at - 1]))) return false;
    const std::size_t end { at + length };
    return end >= text.size() || !is_word(static_cast<unsigned char>(text[end]));
}

// A name as a word, but not as a JSON object's key: a key is this program's own vocabulary ("plan",
// "server"), and a user of that name leaks nothing through it.
bool name_word_at(std::string_view text, std::size_t at, std::size_t length) {
    if (!whole_word_at(text, at, length)) return false;
    const std::size_t end { at + length };
    if (at == 0 || text[at - 1] != '"' || end >= text.size() || text[end] != '"') return true;
    std::size_t next { end + 1 };
    while (next < text.size() && (text[next] == ' ' || text[next] == '\t')) ++next;
    return next >= text.size() || text[next] != ':';
}

// A directory name: right after a separator, and not going on as a longer name.
bool directory_name_at(std::string_view text, std::size_t at, std::size_t length) {
    if (at == 0 || (text[at - 1] != '/' && text[at - 1] != '\\')) return false;
    return !name_continues(text, at + length);
}

template <class Accept>
std::size_t replace_words(std::string& text, std::string_view word, std::string_view replacement, Accept accept) {
    if (word.empty()) return 0;
    std::string out;
    std::size_t count { 0 };
    std::size_t copied { 0 };
    for (std::size_t at { ifind(text, word) }; at != std::string::npos; at = ifind(text, word, at + 1)) {
        if (at < copied || !accept(std::string_view { text }, at, word.size())) continue;
        out.append(text, copied, at - copied);
        out += replacement;
        copied = at + word.size();
        ++count;
    }
    if (count == 0) return 0;
    out.append(text, copied);
    text = std::move(out);
    return count;
}

// ---- secrets -----------------------------------------------------------------------------------

// The words of an identifier: "apiKey", "API_KEY", "x-api-key" and "ApiKey" are all {api, key}.
std::vector<std::string> words_of(std::string_view name) {
    std::vector<std::string> words;
    std::string current;
    const auto flush = [&] {
        if (!current.empty()) words.push_back(lowered(current));
        current.clear();
    };
    for (std::size_t i { 0 }; i < name.size(); ++i) {
        const auto c = static_cast<unsigned char>(name[i]);
        if (!is_alnum(c)) {
            flush();
            continue;
        }
        if (!current.empty()) {
            const auto previous = static_cast<unsigned char>(name[i - 1]);
            const bool lowerToUpper { (previous >= 'a' && previous <= 'z') && (c >= 'A' && c <= 'Z') };
            // "APIKey": the K starts a word because a lower-case letter follows it.
            const bool upperRunEnds { (previous >= 'A' && previous <= 'Z') && (c >= 'A' && c <= 'Z') && i + 1 < name.size()
                                      && name[i + 1] >= 'a' && name[i + 1] <= 'z' };
            if (lowerToUpper || upperRunEnds) flush();
        }
        current += static_cast<char>(c);
    }
    flush();
    return words;
}

constexpr std::array<std::string_view, 18> SECRET_WORDS {
    "token", "secret", "secrets", "password", "passwords", "passwd", "pwd", "passphrase", "credential", "credentials",
    "authorization", "auth", "cookie", "apikey", "privatekey", "accesskey", "secretkey", "bearer",
};

constexpr std::array<std::pair<std::string_view, std::string_view>, 4> SECRET_PAIRS {
    std::pair { "api", "key" }, std::pair { "private", "key" }, std::pair { "access", "key" }, std::pair { "session", "key" },
};

// Values that are not secrets whatever their key says.
bool harmless_value(std::string_view value) {
    const std::string lowerValue { lowered(value) };
    return value.empty() || value.starts_with('<') || lowerValue == "null" || lowerValue == "true" || lowerValue == "false"
           || lowerValue == "none" || lowerValue == "***";
}

// Whether the `"` at `at` is escaped by the backslashes before it.
bool escaped_quote(std::string_view text, std::size_t at) {
    std::size_t backslashes { 0 };
    while (at > backslashes && text[at - 1 - backslashes] == '\\') ++backslashes;
    return backslashes % 2 == 1;
}

struct Span {
    std::size_t begin { 0 };
    std::size_t end { 0 };
};

// Spans are collected over the original text, then replaced in one pass.
std::string replace_spans(std::string_view text, std::vector<Span> spans, std::string_view replacement) {
    std::ranges::sort(spans, {}, &Span::begin);
    std::string out;
    out.reserve(text.size());
    std::size_t copied { 0 };
    for (const auto& span : spans) {
        if (span.begin < copied) continue;   // overlaps one already replaced
        out.append(text, copied, span.begin - copied);
        out += replacement;
        copied = span.end;
    }
    out.append(text, copied);
    return out;
}

// The end of a value that follows `=` or `: `: at white space, a quote that is not escaped, `&`,
// or the end of the line.
std::size_t value_end(std::string_view text, std::size_t begin, bool toEndOfLine = false) {
    std::size_t j { begin };
    while (j < text.size()) {
        const char c { text[j] };
        if (c == '\n' || c == '\r') break;
        if (!toEndOfLine && (c == ' ' || c == '\t' || c == '\'' || c == '&')) break;
        if (c == '"' && !escaped_quote(text, j)) break;
        ++j;
    }
    return j;
}

bool token_char(unsigned char c) { return is_alnum(c) || c == '_' || c == '-'; }

constexpr std::array<std::string_view, 13> TOKEN_PREFIXES {
    "ghp_", "gho_", "ghu_", "ghs_", "ghr_", "github_pat_", "glpat-", "xoxb-", "xoxp-", "xoxa-", "xoxr-", "sk-", "AKIA",
};

// Tokens by their well-known prefix: GitHub, GitLab, Slack, OpenAI and Anthropic keys, AWS access keys.
void find_prefixed_tokens(std::string_view text, std::vector<Span>& spans) {
    for (const auto prefix : TOKEN_PREFIXES) {
        const bool aws { prefix == "AKIA" };
        for (std::size_t at { text.find(prefix) }; at != std::string_view::npos; at = text.find(prefix, at + 1)) {
            if (at > 0 && (is_alnum(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '_')) continue;
            std::size_t end { at + prefix.size() };
            while (end < text.size() && token_char(static_cast<unsigned char>(text[end]))) ++end;
            const std::size_t body { end - at - prefix.size() };
            if (aws ? body != 16 : body < 16) continue;
            spans.push_back(Span { at, end });
        }
    }
}

// "scheme://user:password@host": the credentials.
void find_url_credentials(std::string_view text, std::vector<Span>& spans) {
    for (std::size_t at { text.find("://") }; at != std::string_view::npos; at = text.find("://", at + 3)) {
        const std::size_t begin { at + 3 };
        std::size_t j { begin };
        std::optional<std::size_t> colon;
        std::optional<std::size_t> atSign;
        while (j < text.size()) {
            const char c { text[j] };
            if (c == '/' || c == ' ' || c == '"' || c == '\'' || c == '\n' || c == '\\' || c == '?' || c == '#') break;
            if (c == ':' && !colon) colon = j;
            if (c == '@') atSign = j;
            ++j;
        }
        if (atSign && colon && *colon < *atSign && *atSign > begin) spans.push_back(Span { begin, *atSign });
    }
}

// "Bearer <token>", "Basic <credentials>".
void find_authorization_schemes(std::string_view text, std::vector<Span>& spans) {
    for (const std::string_view scheme : { "Bearer ", "bearer ", "Basic ", "basic " }) {
        for (std::size_t at { text.find(scheme) }; at != std::string_view::npos; at = text.find(scheme, at + 1)) {
            if (at > 0 && is_alnum(static_cast<unsigned char>(text[at - 1]))) continue;
            const std::size_t begin { at + scheme.size() };
            std::size_t end { begin };
            while (end < text.size() && (token_char(static_cast<unsigned char>(text[end])) || std::string_view { "._~+/=" }.contains(text[end]))) ++end;
            if (end - begin >= 8 && !harmless_value(text.substr(begin, end - begin))) spans.push_back(Span { begin, end });
        }
    }
}

// "key": "value", where the key names a secret: the value.
void find_json_secrets(std::string_view text, std::vector<Span>& spans) {
    for (std::size_t open { text.find('"') }; open != std::string_view::npos;) {
        std::size_t close { open + 1 };
        while (close < text.size() && close - open <= 64 && text[close] != '"' && text[close] != '\n') {
            close += text[close] == '\\' ? 2 : 1;
        }
        if (close >= text.size() || text[close] != '"' || close - open > 64) {
            open = text.find('"', open + 1);
            continue;
        }
        std::size_t j { close + 1 };
        while (j < text.size() && (text[j] == ' ' || text[j] == '\t')) ++j;
        if (j >= text.size() || text[j] != ':') {
            open = text.find('"', close);
            continue;
        }
        ++j;
        while (j < text.size() && (text[j] == ' ' || text[j] == '\t')) ++j;
        if (j < text.size() && text[j] == '"' && secret_name(text.substr(open + 1, close - open - 1))) {
            std::size_t end { j + 1 };
            while (end < text.size() && !(text[end] == '"' && !escaped_quote(text, end)) && text[end] != '\n') ++end;
            if (end < text.size() && text[end] == '"' && !harmless_value(text.substr(j + 1, end - j - 1))) spans.push_back(Span { j + 1, end });
            open = end < text.size() ? text.find('"', end + 1) : std::string_view::npos;
            continue;
        }
        open = text.find('"', close + 1);
    }
}

// NAME=value (an environment variable, a --flag=value, a -DNAME=value define) whose name names a secret.
void find_assigned_secrets(std::string_view text, std::vector<Span>& spans) {
    for (std::size_t equals { text.find('=') }; equals != std::string_view::npos; equals = text.find('=', equals + 1)) {
        std::size_t start { equals };
        while (start > 0 && (is_alnum(static_cast<unsigned char>(text[start - 1])) || text[start - 1] == '_' || text[start - 1] == '-'
                             || text[start - 1] == '.')) {
            --start;
        }
        while (start < equals && text[start] == '-') ++start;   // --flag
        if (start == equals) continue;
        std::string_view name { text.substr(start, equals - start) };
        bool secret { secret_name(name) };
        // -DAPI_KEY=...: the define's name follows the D.
        if (!secret && start >= 1 && text[start - 1] == '-' && name.size() > 1 && name.front() == 'D') secret = secret_name(name.substr(1));
        if (!secret) continue;
        const std::size_t end { value_end(text, equals + 1) };
        if (end > equals + 1 && !harmless_value(text.substr(equals + 1, end - equals - 1))) spans.push_back(Span { equals + 1, end });
    }
}

// "Authorization: ...", "password: ..." in plain text: the rest of the line. Only names that are
// never anything else; a bare "token:" is too common in a log to mean a secret.
void find_header_secrets(std::string_view text, std::vector<Span>& spans) {
    static constexpr std::array<std::string_view, 10> HEADERS { "authorization:", "proxy-authorization:", "password:", "passwd:", "x-api-key:",
                                                                "api-key:", "api_key:", "private-token:", "client_secret:", "secret:" };
    for (const auto header : HEADERS) {
        for (std::size_t at { ifind(text, header) }; at != std::string_view::npos; at = ifind(text, header, at + 1)) {
            if (at > 0 && (is_alnum(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '_' || text[at - 1] == '-' || text[at - 1] == '"')) continue;
            std::size_t begin { at + header.size() };
            while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
            const std::size_t end { value_end(text, begin, true) };
            if (end > begin && !harmless_value(text.substr(begin, end - begin))) spans.push_back(Span { begin, end });
        }
    }
}

bool email_local_char(unsigned char c) { return is_alnum(c) || std::string_view { "._%+-" }.contains(static_cast<char>(c)); }

// local@domain.tld, the domain with at least two labels and an alphabetic top-level label.
void find_emails(std::string_view text, std::vector<Span>& spans) {
    for (std::size_t at { text.find('@') }; at != std::string_view::npos; at = text.find('@', at + 1)) {
        std::size_t begin { at };
        while (begin > 0 && at - begin < 64 && email_local_char(static_cast<unsigned char>(text[begin - 1]))) --begin;
        if (begin == at || text[begin] == '.' || text[at - 1] == '.') continue;
        if (lowered(text.substr(begin, at - begin)) == "git") continue;   // git@github.com:owner/repo is an address, not a person
        std::size_t end { at + 1 };
        std::size_t labels { 0 };
        std::size_t lastLabel { end };
        bool alphabeticTop { false };
        while (end < text.size()) {
            const std::size_t labelBegin { end };
            while (end < text.size() && (is_alnum(static_cast<unsigned char>(text[end])) || text[end] == '-')) ++end;
            if (end == labelBegin) break;
            ++labels;
            lastLabel = labelBegin;
            alphabeticTop = end - labelBegin >= 2 && std::ranges::all_of(text.substr(labelBegin, end - labelBegin), [](char c) { return is_alpha(static_cast<unsigned char>(c)); });
            if (end + 1 < text.size() && text[end] == '.' && is_alnum(static_cast<unsigned char>(text[end + 1]))) {
                ++end;
                continue;
            }
            break;
        }
        (void)lastLabel;
        if (labels >= 2 && alphabeticTop) spans.push_back(Span { begin, end });
    }
}

constexpr std::array<std::string_view, 43> COMMON_NAMES {
    "admin", "administrator", "user", "users", "guest", "root", "test", "tests", "tester", "runner", "build", "builder",
    "developer", "home", "public", "default", "shared", "ubuntu", "debian", "fedora", "centos", "docker", "vagrant", "ec2-user",
    "macos", "windows", "linux", "local", "localhost", "owner", "work", "workspace", "code", "mcpp", "clang", "clangd", "server",
    "client", "data", "temp", "system", "service", "demo",
};

// Directories under a profile root that are no person's.
constexpr std::array<std::string_view, 8> NOBODY { "public", "default", "default user", "all users", "shared", "linuxbrew", "guest", "defaultapppool" };

} // namespace

bool distinctive_name(std::string_view name) {
    if (name.size() < 4) return false;
    const std::string lowerName { lowered(name) };
    return std::ranges::find(COMMON_NAMES, std::string_view { lowerName }) == COMMON_NAMES.end();
}

bool secret_name(std::string_view name) {
    const auto words = words_of(name);
    for (std::size_t i { 0 }; i < words.size(); ++i) {
        if (std::ranges::find(SECRET_WORDS, std::string_view { words[i] }) != SECRET_WORDS.end()) return true;
        if (i + 1 < words.size()) {
            for (const auto& [first, second] : SECRET_PAIRS) {
                if (words[i] == first && words[i + 1] == second) return true;
            }
        }
    }
    return false;
}

struct Redactor::Impl {
    Identity identity;
    std::vector<PathPattern> workspacePatterns;   // longest first
    std::vector<PathPattern> homePatterns;        // longest first
    std::vector<std::string> homeTexts;           // each home in its '/' and '\' spellings, for the residue check
    std::vector<std::string> shortNames;          // 8.3 prefixes of the user names ("RUNNER" of runneradmin)
    std::vector<std::string> users;
    std::vector<std::string> hosts;
    std::map<std::string, std::string, std::less<>> userPlaceholders;   // lower-case name -> <user>, <user-2>, ...
    std::map<std::string, std::size_t, std::less<>> hits;

    explicit Impl(Identity who) : identity { std::move(who) } {
        const auto byLength = [](const PathPattern& a, const PathPattern& b) { return a.pieces.size() > b.pieces.size(); };
        for (std::size_t i { 0 }; i < identity.workspaces.size(); ++i) {
            if (identity.workspaces[i].empty()) continue;
            const std::string placeholder { i == 0 ? std::string { "<workspace>" } : std::format("<workspace-{}>", i + 1) };
            for (auto& pattern : path_patterns(identity.workspaces[i], RULE_WORKSPACE, placeholder)) workspacePatterns.push_back(std::move(pattern));
        }
        std::ranges::stable_sort(workspacePatterns, byLength);
        for (const auto& home : identity.homes) {
            if (segments_of(home).empty()) continue;   // "/" or "C:/": nothing personal to hide
            for (auto& pattern : path_patterns(home, RULE_HOME, "~")) homePatterns.push_back(std::move(pattern));
            std::string backslashed { home };
            std::ranges::replace(backslashed, '/', '\\');
            homeTexts.push_back(home);
            homeTexts.push_back(std::move(backslashed));
        }
        std::ranges::stable_sort(homePatterns, byLength);
        for (const auto& user : identity.users) {
            if (user.empty() || std::ranges::find_if(users, [&](const std::string& known) { return lowered(known) == lowered(user); }) != users.end()) continue;
            users.push_back(user);
            userPlaceholders.emplace(lowered(user), "<user>");
            if (auto prefix = short_name_prefix(user)) shortNames.push_back(std::move(*prefix));
        }
        for (const auto& host : identity.hosts) {
            if (host.empty()) continue;
            hosts.push_back(host);
            // "mac-mini.local": the name before the domain is the machine too.
            if (const auto dot = host.find('.'); dot != std::string::npos && dot > 0) hosts.push_back(host.substr(0, dot));
        }
        std::ranges::sort(hosts, [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    }

    void count(std::string_view rule, std::size_t n) {
        if (n == 0) return;
        auto it = hits.find(rule);
        if (it == hits.end()) it = hits.emplace(std::string { rule }, 0).first;
        it->second += n;
    }

    std::string placeholder_for_user(std::string_view name) {
        const std::string key { lowered(name) };
        if (const auto known = userPlaceholders.find(key); known != userPlaceholders.end()) return known->second;
        // <user> is the user's own; everybody else counts from 2, in the order they are met.
        const auto others = std::ranges::count_if(userPlaceholders, [](const auto& item) { return item.second != "<user>"; });
        std::string placeholder { std::format("<user-{}>", others + 2) };
        userPlaceholders.emplace(key, placeholder);
        return placeholder;
    }

    // Profile directories of other people: /home/<name>, /Users/<name>, C:\Users\<name>, /mnt/c/Users/<name>.
    // What follows the profile root is the person's name, whoever it is.
    std::size_t replace_profile_directories(std::string& text) {
        return replace_profile_directories(text, "home") + replace_profile_directories(text, "users");
    }

    std::size_t replace_profile_directories(std::string& text, std::string_view root) {
        std::size_t count { 0 };
        std::string out;
        std::size_t copied { 0 };
        for (std::size_t at { ifind(text, root) }; at != std::string::npos; at = ifind(text, root, at + 1)) {
            if (at < copied || at == 0) continue;
            // The root segment follows a separator run that starts the path, follows a drive, or follows /mnt/<letter>.
            std::size_t runStart { at };
            while (runStart > 0 && (text[runStart - 1] == '/' || text[runStart - 1] == '\\')) --runStart;
            if (runStart == at) continue;
            const auto before = [&](std::size_t back) { return static_cast<unsigned char>(text[runStart - back]); };
            const bool atRoot { path_starts_at(text, runStart, false) };
            const bool afterDrive { runStart >= 2 && before(1) == ':' && is_alpha(before(2)) && path_starts_at(text, runStart - 2, true) };
            const bool afterMount { runStart >= 6 && is_alpha(before(1)) && lowered(std::string_view { text }.substr(runStart - 6, 5)) == "/mnt/" };
            if (!atRoot && !afterDrive && !afterMount) continue;
            const std::size_t separator { match_separator(text, at + root.size()) };
            if (separator == 0) continue;
            const std::size_t nameBegin { at + root.size() + separator };
            // The name ends at the next separator; a space belongs to it only when a separator follows the name.
            static constexpr std::string_view ENDS_NAME { "/\\\"'\n\r\t:<>|*?,;()[]{}" };
            std::size_t nameEnd { nameBegin };
            std::optional<std::size_t> firstSpace;
            while (nameEnd < text.size() && !ENDS_NAME.contains(text[nameEnd])) {
                if (text[nameEnd] == ' ' && !firstSpace) firstSpace = nameEnd;
                ++nameEnd;
            }
            const bool separatorFollows { nameEnd < text.size() && (text[nameEnd] == '/' || text[nameEnd] == '\\') };
            if (firstSpace && !separatorFollows) nameEnd = *firstSpace;
            while (nameEnd > nameBegin && text[nameEnd - 1] == '.') --nameEnd;   // the end of a sentence, not of the name
            if (nameEnd == nameBegin) continue;
            const std::string_view name { std::string_view { text }.substr(nameBegin, nameEnd - nameBegin) };
            if (name.starts_with('<') || name.starts_with('~') || name.starts_with('%') || name.starts_with('$')
                || std::ranges::find(NOBODY, std::string_view { lowered(name) }) != NOBODY.end()) continue;
            out.append(text, copied, nameBegin - copied);
            out += placeholder_for_user(name);
            copied = nameEnd;
            ++count;
        }
        if (count == 0) return 0;
        out.append(text, copied);
        text = std::move(out);
        return count;
    }
};

Redactor::Redactor(Identity identity) : impl_ { std::make_unique<Impl>(std::move(identity)) } {}
Redactor::~Redactor() = default;
Redactor::Redactor(Redactor&&) noexcept = default;
Redactor& Redactor::operator=(Redactor&&) noexcept = default;

const std::map<std::string, std::size_t, std::less<>>& Redactor::hits() const { return impl_->hits; }

std::string Redactor::redact(std::string_view input) {
    Impl& impl { *impl_ };
    // Secrets first, on the text as it came: a token in a URL or a define is found by its shape
    // before any path around it is rewritten.
    std::vector<Span> secrets;
    find_prefixed_tokens(input, secrets);
    find_url_credentials(input, secrets);
    find_authorization_schemes(input, secrets);
    find_json_secrets(input, secrets);
    find_assigned_secrets(input, secrets);
    find_header_secrets(input, secrets);
    std::string text;
    if (secrets.empty()) {
        text = std::string { input };
    } else {
        impl.count(RULE_SECRET, secrets.size());
        text = replace_spans(input, std::move(secrets), "<redacted>");
    }
    std::vector<Span> emails;
    find_emails(text, emails);
    if (!emails.empty()) {
        impl.count(RULE_EMAIL, emails.size());
        text = replace_spans(text, std::move(emails), "<redacted>");
    }
    // The project's own paths before the home they are under, the longest spelling first.
    for (const auto& pattern : impl.workspacePatterns) impl.count(RULE_WORKSPACE, replace_path(text, pattern));
    for (const auto& pattern : impl.homePatterns) impl.count(RULE_HOME, replace_path(text, pattern));
    // A profile under its 8.3 name without the drive before it (RUNNER~1).
    for (const auto& prefix : impl.shortNames) {
        std::size_t n { 0 };
        for (std::size_t at { ifind(text, prefix) }; at != std::string::npos; at = ifind(text, prefix, at + 1)) {
            if (at > 0 && is_word(static_cast<unsigned char>(text[at - 1]))) continue;
            const std::size_t length { match_short_name(text, at, prefix) };
            if (length == 0 || (at + length < text.size() && is_word(static_cast<unsigned char>(text[at + length])))) continue;
            text.replace(at, length, "<user>");
            ++n;
        }
        impl.count(RULE_SHORT_NAME, n);
    }
    impl.count(RULE_USER, impl.replace_profile_directories(text));
    for (const auto& user : impl.users) {
        impl.count(RULE_USER, distinctive_name(user) ? replace_words(text, user, "<user>", name_word_at)
                                                     : replace_words(text, user, "<user>", directory_name_at));
    }
    for (const auto& host : impl.hosts) {
        // A host name that is a common word ("ubuntu", "localhost") identifies nobody and stays.
        if (distinctive_name(host)) impl.count(RULE_HOST, replace_words(text, host, "<host>", name_word_at));
    }
    return text;
}

Json Redactor::redact_json(const Json& value) {
    const std::string redacted { redact(value.dump(-1, ' ', false, Json::error_handler_t::replace)) };
    Json parsed = Json::parse(redacted, nullptr, false);
    // Never expected: no placeholder has a quote or a backslash, and no rule ends inside an escape.
    // Should it happen, the text is kept rather than the original.
    return parsed.is_discarded() ? Json(redacted) : parsed;
}

std::vector<Residue> Redactor::residue(std::string_view text, std::size_t limit) const {
    const Impl& impl { *impl_ };
    std::vector<Residue> found;
    const auto add = [&](std::string_view rule, std::size_t offset) {
        if (found.size() < limit) found.push_back(Residue { std::string { rule }, offset });
    };
    for (const auto& pattern : impl.homePatterns) {
        if (const auto at = find_path(text, pattern)) add(RULE_HOME, *at);
    }
    // The home as a plain string, whatever follows it: stricter than the rule that replaced it, so a
    // spelling that rule does not know is found here rather than shipped.
    for (const auto& home : impl.homeTexts) {
        if (const auto at = ifind(text, home); at != std::string_view::npos) add(RULE_HOME, at);
    }
    for (const auto& pattern : impl.workspacePatterns) {
        if (const auto at = find_path(text, pattern)) add(RULE_WORKSPACE, *at);
    }
    for (const auto& prefix : impl.shortNames) {
        for (std::size_t at { ifind(text, prefix) }; at != std::string_view::npos; at = ifind(text, prefix, at + 1)) {
            if (match_short_name(text, at, prefix) > 0) {
                add(RULE_SHORT_NAME, at);
                break;
            }
        }
    }
    for (const auto& user : impl.users) {
        const bool distinctive { distinctive_name(user) };
        for (std::size_t at { ifind(text, user) }; at != std::string_view::npos; at = ifind(text, user, at + 1)) {
            if (distinctive ? name_word_at(text, at, user.size()) : directory_name_at(text, at, user.size())) {
                add(RULE_USER, at);
                break;
            }
        }
    }
    for (const auto& host : impl.hosts) {
        if (!distinctive_name(host)) continue;
        for (std::size_t at { ifind(text, host) }; at != std::string_view::npos; at = ifind(text, host, at + 1)) {
            if (name_word_at(text, at, host.size())) {
                add(RULE_HOST, at);
                break;
            }
        }
    }
    std::vector<Span> tokens;
    find_prefixed_tokens(text, tokens);
    for (const auto& token : tokens) add(RULE_SECRET, token.begin);
    std::ranges::sort(found, {}, &Residue::offset);
    return found;
}

} // namespace mcppls::bundle
