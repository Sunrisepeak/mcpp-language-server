module mcppls.base.path;

import std;
import mcppls.os;
import mcppls.base.text;

namespace mcppls::base {

namespace {

bool is_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

bool has_drive(std::string_view path) {
    return path.size() >= 2 && is_letter(path[0]) && path[1] == ':';
}

// The root prefix of a normalized path: "/", "C:/", "//server/share/" or "".
std::string_view root_of(std::string_view path, PathStyle style) {
    if (style == PathStyle::windows) {
        if (has_drive(path)) return (path.size() >= 3 && path[2] == '/') ? path.substr(0, 3) : path.substr(0, 2);
        if (path.starts_with("//")) {
            std::size_t server { path.find('/', 2) };
            if (server == std::string_view::npos) return path;
            std::size_t share { path.find('/', server + 1) };
            return share == std::string_view::npos ? path : path.substr(0, share + 1);
        }
    }
    if (path.starts_with('/')) return path.substr(0, 1);
    return {};
}

} // namespace

bool is_absolute_path(std::string_view path, PathStyle style) {
    if (style == PathStyle::windows) {
        if (has_drive(path)) return path.size() >= 3 && (path[2] == '/' || path[2] == '\\');
        return path.starts_with("//") || path.starts_with("\\\\") || path.starts_with('/') || path.starts_with('\\');
    }
    return path.starts_with('/');
}

std::string normalize_path(std::string_view input, PathStyle style) {
    std::string path { input };
    if (style == PathStyle::windows) std::replace(path.begin(), path.end(), '\\', '/');
    if (style == PathStyle::windows && has_drive(path) && path[0] >= 'a' && path[0] <= 'z') {
        path[0] = static_cast<char>(path[0] - 'a' + 'A');
    }
    const std::string root { root_of(path, style) };
    const bool absolute { !root.empty() };
    std::string_view rest { std::string_view { path }.substr(root.size()) };

    std::vector<std::string_view> parts;
    for (auto part : split(rest, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (!parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else if (!absolute) {
                parts.push_back(part);
            }
            continue;
        }
        parts.push_back(part);
    }

    std::string out { root };
    for (std::size_t i { 0 }; i < parts.size(); ++i) {
        if (i != 0) out += '/';
        out.append(parts[i]);
    }
    if (out.empty()) return ".";
    return out;
}

std::string join_path(std::string_view base, std::string_view child, PathStyle style) {
    if (child.empty()) return normalize_path(base, style);
    if (is_absolute_path(child, style) || base.empty()) return normalize_path(child, style);
    std::string joined { base };
    joined += '/';
    joined += child;
    return normalize_path(joined, style);
}

std::string parent_path(std::string_view path, PathStyle style) {
    const std::string normalized { normalize_path(path, style) };
    const std::string_view root { root_of(normalized, style) };
    const std::size_t slash { normalized.find_last_of('/') };
    if (slash == std::string::npos) return ".";
    if (slash + 1 <= root.size()) return std::string { root };
    return normalized.substr(0, slash);
}

std::string_view file_name(std::string_view path) {
    const std::size_t slash { path.find_last_of("/\\") };
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::string_view extension(std::string_view path) {
    const std::string_view name { file_name(path) };
    const std::size_t dot { name.find_last_of('.') };
    if (dot == std::string_view::npos || dot == 0) return {};
    return name.substr(dot);
}

std::optional<std::string> relative_path(std::string_view path, std::string_view base, PathStyle style) {
    const std::string p { normalize_path(path, style) };
    const std::string b { normalize_path(base, style) };
    if (!is_within(p, b)) return std::nullopt;
    if (p.size() == b.size()) return std::string { "." };
    std::size_t skip { b.size() };
    if (!b.ends_with('/')) ++skip;
    return p.substr(skip);
}

std::string path_key(std::string_view path, bool caseInsensitive) {
    return caseInsensitive ? to_lower_ascii(path) : std::string { path };
}

bool same_path(std::string_view a, std::string_view b, bool caseInsensitive) {
    return caseInsensitive ? iequals_ascii(a, b) : a == b;
}

bool is_within(std::string_view path, std::string_view directory, bool caseInsensitive) {
    if (directory.empty() || directory == ".") return true;
    if (path.size() < directory.size()) return false;
    if (!same_path(path.substr(0, directory.size()), directory, caseInsensitive)) return false;
    if (path.size() == directory.size()) return true;
    return directory.ends_with('/') || path[directory.size()] == '/';
}

} // namespace mcppls::base
