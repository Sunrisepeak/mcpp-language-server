// Text utilities, including the UTF-8 / UTF-16 position arithmetic the
// Language Server Protocol requires: LSP columns count UTF-16 code units.
export module mcppls.base.text;

import std;

export namespace mcppls::base {

struct Position {
    int line { 0 };
    int character { 0 };
    auto operator<=>(const Position&) const = default;
};

struct Range {
    Position start;
    Position end;
    bool operator==(const Range&) const = default;
    bool contains(Position position) const { return start <= position && position <= end; }
};

std::string_view trim(std::string_view text);
std::vector<std::string_view> split(std::string_view text, char separator);
std::vector<std::string_view> split_lines(std::string_view text);
std::string join(std::span<const std::string> parts, std::string_view separator);
std::string to_lower_ascii(std::string_view text);
bool iequals_ascii(std::string_view a, std::string_view b);
std::string replace_all(std::string_view text, std::string_view from, std::string_view to);

std::size_t utf16_length(std::string_view utf8);
Position position_at(std::string_view text, std::size_t byteOffset);
std::optional<std::size_t> offset_at(std::string_view text, Position position);
// A UTF-8 byte order mark that begins a file's text: its size, 3, or 0 when there is none. Editors do
// not show it, so the scanners start after it and give it no column (fix plan F2).
std::size_t byte_order_mark_size(std::string_view text);

bool is_identifier_start(char c);
bool is_identifier_char(char c);

} // namespace mcppls::base
