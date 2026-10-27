module mcppls.base.path;

import std;
import mcpplibs.os;
import mcppls.base.error;
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

// ---- asking the file system, in this spelling ----------------------------------------------------

std::string spelled(const std::filesystem::path& path) { return normalize_path(mcpplibs::os::fs::generic_utf8(path)); }

std::vector<std::string> children(std::string_view directory) {
    std::vector<std::string> result;
    std::error_code error;
    for (std::filesystem::directory_iterator it { std::filesystem::path { directory }, error }, end; !error && it != end; it.increment(error)) {
        result.push_back(join_path(directory, mcpplibs::os::fs::generic_utf8(it->path().filename())));
    }
    std::ranges::sort(result);
    return result;
}

std::vector<std::string> files_under(std::string_view root, std::span<const std::string_view> extensions,
                                     std::span<const std::string_view> skipDirectories) {
    std::vector<std::string> result;
    std::vector<std::string> pending { normalize_path(root) };
    while (!pending.empty()) {
        std::string directory { std::move(pending.back()) };
        pending.pop_back();
        std::error_code error;
        for (std::filesystem::directory_iterator it { std::filesystem::path { directory }, error }, end; !error && it != end; it.increment(error)) {
            const std::string name { mcpplibs::os::fs::generic_utf8(it->path().filename()) };
            const std::string full { join_path(directory, name) };
            std::error_code statusError;
            if (it->is_directory(statusError)) {
                if (name.starts_with('.')) continue;
                if (std::ranges::any_of(skipDirectories, [&](std::string_view skip) { return name == skip; })) continue;
                if (it->is_symlink(statusError)) continue;
                pending.push_back(full);
            } else if (it->is_regular_file(statusError)) {
                const std::string_view suffix { extension(name) };
                if (extensions.empty() || std::ranges::any_of(extensions, [&](std::string_view wanted) { return iequals_ascii(wanted, suffix); })) {
                    result.push_back(full);
                }
            }
        }
    }
    std::ranges::sort(result);
    return result;
}

std::string current_directory() {
    std::error_code error;
    const auto path = std::filesystem::current_path(error);
    return error ? std::string {} : spelled(path);
}

std::string canonical_path(std::string_view path) {
    if (path.empty()) return {};
    return spelled(mcpplibs::os::fs::canonical(std::filesystem::path { path }));
}

Result<void> make_executable(std::span<const std::string> paths) {
    for (const auto& path : paths) {
        auto marked = mcpplibs::os::fs::set_executable(std::filesystem::path { path }, true);
        if (!marked && marked.error().code != mcpplibs::os::errc::not_supported)
            return fail("chmod", std::format("cannot make {} executable: {}", path, marked.error().message()));
    }
    return {};
}

std::optional<mcpplibs::os::fs::stamp> file_stamp(std::string_view path) {
    auto stamp = mcpplibs::os::fs::stamp_of(std::filesystem::path { path });
    if (!stamp) return std::nullopt;
    return *stamp;
}

std::int64_t file_time_now() {
    // The file clock's own reading of now: a file just written carries it. The system clock is not that reading:
    // Linux stamps a file from a coarse clock that lags it by up to a tick, so a file written right after
    // `system_clock::now()` can carry an earlier time, and a sweep bounded by it would take that file. One scratch
    // name in the temporary directory, reused.
    static const std::filesystem::path scratch { [] {
        auto temporary = mcpplibs::os::dirs::temp();
        std::error_code ignored;
        std::filesystem::create_directories(temporary, ignored);
        return temporary / ".mcppls-clock";
    }() };
    (void)mcpplibs::os::fs::write(scratch, "now");
    const auto stamp = mcpplibs::os::fs::stamp_of(scratch);
    return stamp ? static_cast<std::int64_t>(stamp->modified_ns) : std::int64_t { 0 };
}

} // namespace mcppls::base
