module mcppls.project.boundary;

import std;
import mcppls.base.path;
import mcppls.platform.fs;

namespace mcppls::project {

namespace {

constexpr std::array<std::string_view, 3> MANIFESTS { "mcpp.toml", "CMakeLists.txt", "compile_commands.json" };

bool has(std::string_view directory, std::string_view manifest) { return platform::fs::is_regular_file(base::join_path(directory, manifest)); }

// `path` relative to `root`, with '/' separators; empty when `path` is not inside it.
std::string relative_to(std::string_view root, std::string_view path) {
    const std::string rootKey { base::path_key(root) };
    std::string key { base::path_key(path) };
    if (key.size() <= rootKey.size() || !key.starts_with(rootKey)) return {};
    key.erase(0, rootKey.size());
    while (!key.empty() && (key.front() == '/' || key.front() == '\\')) key.erase(0, 1);
    std::ranges::replace(key, '\\', '/');
    return key;
}

// A member as mcpp.toml writes it: a path, or a path ending in `/*` for every directory below it.
bool matches_member(std::string_view member, std::string_view relative) {
    std::string pattern { member };
    std::ranges::replace(pattern, '\\', '/');
    while (pattern.ends_with('/')) pattern.pop_back();
    if (pattern.ends_with("/*")) {
        const std::string_view prefix { std::string_view { pattern }.substr(0, pattern.size() - 1) };
        return relative.starts_with(prefix) && relative.find('/', prefix.size()) == std::string_view::npos;
    }
    return base::path_key(pattern) == base::path_key(relative);
}

} // namespace

std::vector<std::string> workspace_members(std::string_view text) {
    std::vector<std::string> members;
    const auto section = text.find("[workspace]");
    if (section == std::string_view::npos) return members;
    const std::string_view rest { text.substr(section + 11) };
    const auto nextSection = rest.find("\n[");
    const std::string_view body { rest.substr(0, nextSection) };
    const auto key = body.find("members");
    if (key == std::string_view::npos) return members;
    const auto open = body.find('[', key);
    const auto close = open == std::string_view::npos ? std::string_view::npos : body.find(']', open);
    if (close == std::string_view::npos) return members;
    const std::string_view list { body.substr(open + 1, close - open - 1) };
    for (std::size_t at { 0 }; (at = list.find('"', at)) != std::string_view::npos;) {
        const auto end = list.find('"', at + 1);
        if (end == std::string_view::npos) break;
        members.emplace_back(list.substr(at + 1, end - at - 1));
        at = end + 1;
    }
    return members;
}

std::string enclosing_project_root(std::string_view directory) {
    std::string current { directory };
    for (int guard { 0 }; guard < 128 && !current.empty(); ++guard) {
        if (std::ranges::any_of(MANIFESTS, [&](std::string_view manifest) { return has(current, manifest); })) return current;
        const std::string parent { base::parent_path(current) };
        if (parent == current) break;
        current = parent;
    }
    return std::string { directory };
}

ProjectBoundaries::ProjectBoundaries(std::string_view outer) : outer_ { outer } {
    outerIsCMake_ = has(outer_, "CMakeLists.txt");
    if (const auto manifest = platform::fs::read_file(base::join_path(outer_, "mcpp.toml"))) members_ = workspace_members(*manifest);
}

bool ProjectBoundaries::separate(std::string_view directory) const {
    if (has(directory, "compile_commands.json")) return true;
    if (has(directory, "CMakeLists.txt") && !outerIsCMake_) return true;
    if (has(directory, "mcpp.toml")) {
        const std::string relative { relative_to(outer_, directory) };
        return std::ranges::none_of(members_, [&](const std::string& member) { return matches_member(member, relative); });
    }
    return false;
}

bool ProjectBoundaries::crossed(std::string_view file, std::string_view stop) const {
    const std::string stopKey { base::path_key(stop) };
    std::string directory { base::parent_path(file) };
    for (int guard { 0 }; guard < 128 && !directory.empty(); ++guard) {
        const std::string key { base::path_key(directory) };
        if (key == stopKey || key.size() <= stopKey.size()) return false;
        if (separate(directory)) return true;
        const std::string parent { base::parent_path(directory) };
        if (parent == directory) break;
        directory = parent;
    }
    return false;
}

} // namespace mcppls::project
