module mcppls.platform.env;

import std;
import mcpplibs.os;
import mcppls.os;
import mcppls.base.path;
import mcppls.base.text;

namespace mcppls::platform::env {

namespace osenv = mcpplibs::os::env;

std::vector<std::string> variables() { return osenv::variables(); }

std::optional<std::string> get(std::string_view name) {
    if (auto value = osenv::get(name)) return value;
    if (mcppls::os::family() == mcppls::os::Family::windows) {
        // Windows names compare without regard to case ("Path" is PATH).
        for (const auto& entry : osenv::variables()) {
            const auto equals = entry.find('=', 1);
            if (equals != std::string::npos && base::iequals_ascii(std::string_view { entry }.substr(0, equals), name))
                return entry.substr(equals + 1);
        }
    }
    return std::nullopt;
}

namespace {

std::vector<std::string> split_path_list(std::string_view pathList) {
    std::vector<std::string> result;
    for (auto piece : base::split(pathList, mcppls::os::path_list_separator())) {
        piece = base::trim(piece);
        if (piece.size() >= 2 && piece.front() == '"' && piece.back() == '"') piece = piece.substr(1, piece.size() - 2);
        if (piece.empty()) continue;
        result.push_back(base::normalize_path(piece));
    }
    return result;
}

} // namespace

std::vector<std::string> search_path() {
    const auto path = get("PATH");
    return path ? split_path_list(*path) : std::vector<std::string> {};
}

std::optional<std::string> find_executable(std::string_view name) { return find_executable(name, get("PATH").value_or("")); }

// What is looked for, and in what order, is mcpplibs.os's: on POSIX the name; on Windows a name
// with one of PATHEXT's extensions as written, any other with each appended and never bare (npm,
// npx, code, gradle and xlings are `.cmd` shims beside an extension-less POSIX script). Here only
// the spelling: a relative entry of the list is not searched, and the answer is normalized.
std::optional<std::string> find_executable(std::string_view name, std::string_view pathList) {
    if (name.empty()) return std::nullopt;
    std::string absolute;
    for (const auto& directory : split_path_list(pathList)) {
        if (!base::is_absolute_path(directory)) continue;
        if (!absolute.empty()) absolute += mcppls::os::path_list_separator();
        absolute += directory;
    }
    if (!base::is_absolute_path(name) && mcpplibs::os::fs::names_a_place(name)) return std::nullopt;
    const auto found = osenv::find_executable(name, absolute);
    if (!found) return std::nullopt;
    return base::normalize_path(found->generic_string());
}

std::vector<std::string> arguments() { return osenv::arguments(); }

std::vector<std::string> self_command() {
    std::vector<std::string> command;
    for (const auto& word : osenv::self_command()) command.push_back(base::normalize_path(word));
    return command;
}

} // namespace mcppls::platform::env
