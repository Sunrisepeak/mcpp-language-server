module mcppls.base.text;

import std;

namespace mcppls::base {

namespace {

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// Bytes in the UTF-8 sequence that starts with `lead`; 1 for invalid leads so
// that malformed input still advances.
std::size_t sequence_length(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead >> 5) == 0x6) return 2;
    if ((lead >> 4) == 0xE) return 3;
    if ((lead >> 3) == 0x1E) return 4;
    return 1;
}

} // namespace

std::string_view trim(std::string_view text) {
    while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
    while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
    return text;
}

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start { 0 };
    while (true) {
        const std::size_t at { text.find(separator, start) };
        if (at == std::string_view::npos) {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, at - start));
        start = at + 1;
    }
}

std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start { 0 };
    for (std::size_t i { 0 }; i < text.size(); ++i) {
        if (text[i] == '\n') {
            std::size_t end { i };
            if (end > start && text[end - 1] == '\r') --end;
            lines.push_back(text.substr(start, end - start));
            start = i + 1;
        }
    }
    if (start < text.size()) {
        std::string_view last { text.substr(start) };
        if (!last.empty() && last.back() == '\r') last.remove_suffix(1);
        lines.push_back(last);
    }
    return lines;
}

std::string join(std::span<const std::string> parts, std::string_view separator) {
    std::string out;
    for (std::size_t i { 0 }; i < parts.size(); ++i) {
        if (i != 0) out += separator;
        out += parts[i];
    }
    return out;
}

std::string to_lower_ascii(std::string_view text) {
    std::string out { text };
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool iequals_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i { 0 }; i < a.size(); ++i) {
        char x { a[i] };
        char y { b[i] };
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

std::string replace_all(std::string_view text, std::string_view from, std::string_view to) {
    if (from.empty()) return std::string { text };
    std::string out;
    std::size_t start { 0 };
    while (true) {
        const std::size_t at { text.find(from, start) };
        if (at == std::string_view::npos) break;
        out.append(text.substr(start, at - start));
        out.append(to);
        start = at + from.size();
    }
    out.append(text.substr(start));
    return out;
}

std::size_t utf16_length(std::string_view utf8) {
    std::size_t units { 0 };
    std::size_t i { 0 };
    while (i < utf8.size()) {
        const std::size_t n { std::min(sequence_length(static_cast<unsigned char>(utf8[i])), utf8.size() - i) };
        units += (n == 4) ? 2 : 1;
        i += n;
    }
    return units;
}

Position position_at(std::string_view text, std::size_t byteOffset) {
    byteOffset = std::min(byteOffset, text.size());
    Position position {};
    std::size_t lineStart { 0 };
    for (std::size_t i { 0 }; i < byteOffset; ++i) {
        if (text[i] == '\n') {
            ++position.line;
            lineStart = i + 1;
        }
    }
    position.character = static_cast<int>(utf16_length(text.substr(lineStart, byteOffset - lineStart)));
    return position;
}

std::optional<std::size_t> offset_at(std::string_view text, Position position) {
    if (position.line < 0 || position.character < 0) return std::nullopt;
    std::size_t lineStart { 0 };
    for (int line { 0 }; line < position.line; ++line) {
        const std::size_t at { text.find('\n', lineStart) };
        if (at == std::string_view::npos) return std::nullopt;
        lineStart = at + 1;
    }
    const std::size_t wanted { static_cast<std::size_t>(position.character) };
    std::size_t offset { lineStart };
    std::size_t units { 0 };
    while (offset < text.size() && text[offset] != '\n' && units < wanted) {
        const std::size_t n { std::min(sequence_length(static_cast<unsigned char>(text[offset])), text.size() - offset) };
        units += (n == 4) ? 2 : 1;
        offset += n;
    }
    // A position past the end of a CRLF line clamps before the carriage return.
    if (offset < text.size() && text[offset] == '\n' && offset > lineStart && text[offset - 1] == '\r') --offset;
    return offset;
}

std::size_t byte_order_mark_size(std::string_view text) {
    static constexpr std::string_view MARK { "\xEF\xBB\xBF" };
    return text.starts_with(MARK) ? MARK.size() : 0;
}

bool is_identifier_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || static_cast<unsigned char>(c) >= 0x80;
}

bool is_identifier_char(char c) {
    return is_identifier_start(c) || (c >= '0' && c <= '9');
}

} // namespace mcppls::base
