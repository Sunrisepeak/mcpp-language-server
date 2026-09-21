module mcppls.base.glob;

import std;
import mcppls.base.text;

namespace mcppls::base {

namespace {

// Every alternative a pattern's braces spell out, outermost first: "a/{b,c{d,e}}" is a/b, a/cd, a/ce.
std::vector<std::string> expand_braces(std::string_view pattern) {
    std::size_t open { std::string_view::npos };
    int depth { 0 };
    for (std::size_t i { 0 }; i < pattern.size(); ++i) {
        if (pattern[i] == '{') {
            if (depth++ == 0) open = i;
        } else if (pattern[i] == '}' && depth > 0 && --depth == 0) {
            std::vector<std::string> alternatives;
            std::size_t start { open + 1 };
            int inner { 0 };
            for (std::size_t j { open + 1 }; j <= i; ++j) {
                if (j < i && pattern[j] == '{') ++inner;
                else if (j < i && pattern[j] == '}') --inner;
                else if (j == i || (pattern[j] == ',' && inner == 0)) {
                    alternatives.emplace_back(pattern.substr(start, j - start));
                    start = j + 1;
                }
            }
            std::vector<std::string> result;
            const std::string_view before { pattern.substr(0, open) };
            for (const auto& rest : expand_braces(pattern.substr(i + 1))) {
                for (const auto& alternative : alternatives) {
                    for (const auto& middle : expand_braces(alternative)) result.push_back(std::string { before } + middle + rest);
                }
            }
            return result;
        }
    }
    return { std::string { pattern } };
}

bool same_character(char a, char b, bool caseInsensitive) {
    if (!caseInsensitive) return a == b;
    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
}

// `[...]` at pattern[at]: whether it matches `c`, and where the class ends (npos when it does not close).
std::pair<bool, std::size_t> match_class(std::string_view pattern, std::size_t at, char c, bool caseInsensitive) {
    std::size_t i { at + 1 };
    const bool negated { i < pattern.size() && (pattern[i] == '!' || pattern[i] == '^') };
    if (negated) ++i;
    bool matched { false };
    const std::size_t first { i };
    for (; i < pattern.size() && (pattern[i] != ']' || i == first); ++i) {
        if (i + 2 < pattern.size() && pattern[i + 1] == '-' && pattern[i + 2] != ']') {
            char low { pattern[i] };
            char high { pattern[i + 2] };
            char value { c };
            if (caseInsensitive) {
                low = static_cast<char>(std::tolower(static_cast<unsigned char>(low)));
                high = static_cast<char>(std::tolower(static_cast<unsigned char>(high)));
                value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
            }
            matched = matched || (low <= value && value <= high);
            i += 2;
        } else {
            matched = matched || same_character(pattern[i], c, caseInsensitive);
        }
    }
    if (i >= pattern.size()) return { false, std::string_view::npos };
    return { matched != negated, i };
}

// One segment: `*` any run of characters, `?` one character, `[...]` a class.
bool match_segment(std::string_view pattern, std::string_view text, bool caseInsensitive) {
    std::size_t p { 0 };
    std::size_t t { 0 };
    std::size_t starPattern { std::string_view::npos };
    std::size_t starText { 0 };
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            starPattern = p++;
            starText = t;
            continue;
        }
        if (p < pattern.size() && pattern[p] == '?') {
            ++p;
            ++t;
            continue;
        }
        if (p < pattern.size() && pattern[p] == '[') {
            const auto [matched, end] = match_class(pattern, p, text[t], caseInsensitive);
            if (end != std::string_view::npos && matched) {
                p = end + 1;
                ++t;
                continue;
            }
            if (end == std::string_view::npos && same_character('[', text[t], caseInsensitive)) {
                ++p;
                ++t;
                continue;
            }
        } else if (p < pattern.size() && same_character(pattern[p], text[t], caseInsensitive)) {
            ++p;
            ++t;
            continue;
        }
        if (starPattern == std::string_view::npos) return false;
        p = starPattern + 1;
        t = ++starText;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool match_segments(std::span<const std::string_view> pattern, std::span<const std::string_view> path, bool caseInsensitive) {
    if (pattern.empty()) return path.empty();
    if (pattern.front() == "**") {
        if (match_segments(pattern.subspan(1), path, caseInsensitive)) return true;
        return !path.empty() && match_segments(pattern, path.subspan(1), caseInsensitive);
    }
    if (path.empty()) return false;
    return match_segment(pattern.front(), path.front(), caseInsensitive) && match_segments(pattern.subspan(1), path.subspan(1), caseInsensitive);
}

} // namespace

bool glob_match(std::string_view pattern, std::string_view path, bool caseInsensitive) {
    const auto pathSegments = split(path, '/');
    for (const auto& alternative : expand_braces(pattern)) {
        const auto patternSegments = split(std::string_view { alternative }, '/');
        if (match_segments(patternSegments, pathSegments, caseInsensitive)) return true;
    }
    return false;
}

} // namespace mcppls::base
