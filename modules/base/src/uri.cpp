module mcppls.base.uri;

import std;
import mcppls.base.error;
import mcppls.base.path;

namespace mcppls::base {

namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool is_unreserved(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
        || c == '-' || c == '.' || c == '_' || c == '~' || c == '/';
}

} // namespace

std::string percent_decode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i { 0 }; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int hi { hex_value(text[i + 1]) };
            const int lo { hex_value(text[i + 2]) };
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

std::string percent_encode_path(std::string_view path) {
    static constexpr std::string_view HEX { "0123456789ABCDEF" };
    std::string out;
    out.reserve(path.size());
    for (char c : path) {
        if (is_unreserved(c)) {
            out += c;
        } else {
            const auto byte = static_cast<unsigned char>(c);
            out += '%';
            out += HEX[byte >> 4];
            out += HEX[byte & 0xF];
        }
    }
    return out;
}

Result<std::string> uri_to_path(std::string_view uri, PathStyle style) {
    constexpr std::string_view SCHEME { "file://" };
    if (uri.size() < SCHEME.size() || !uri.substr(0, SCHEME.size()).starts_with("file://")) {
        return fail("invalid-uri", std::format("not a file URI: {}", uri));
    }
    std::string_view rest { uri.substr(SCHEME.size()) };
    std::string authority;
    const std::size_t slash { rest.find('/') };
    if (slash == std::string_view::npos) return fail("invalid-uri", std::format("file URI without a path: {}", uri));
    authority = std::string { rest.substr(0, slash) };
    rest = rest.substr(slash);
    std::string path { percent_decode(rest) };
    if (style == PathStyle::windows) {
        // "/c:/Users" -> "c:/Users"; a non-local authority becomes a UNC path.
        if (path.size() >= 3 && path[0] == '/' && path[2] == ':') path.erase(0, 1);
        if (!authority.empty() && authority != "localhost") path = "//" + authority + path;
    }
    return normalize_path(path, style);
}

std::string path_to_uri(std::string_view path, PathStyle style) {
    std::string normalized { normalize_path(path, style) };
    if (style == PathStyle::windows && normalized.size() >= 2 && normalized[1] == ':') {
        // VS Code spelling: lower-case drive letter, colon escaped.
        char drive { normalized[0] };
        if (drive >= 'A' && drive <= 'Z') drive = static_cast<char>(drive - 'A' + 'a');
        return std::format("file:///{}%3A{}", drive, percent_encode_path(std::string_view { normalized }.substr(2)));
    }
    if (style == PathStyle::windows && normalized.starts_with("//")) {
        return "file:" + percent_encode_path(normalized);
    }
    return "file://" + percent_encode_path(normalized);
}

} // namespace mcppls::base
