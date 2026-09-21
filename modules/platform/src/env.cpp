module mcppls.platform.env;

import std;
import openkal.types;
import openkal.env;
import mcppls.os;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.fs;

namespace mcppls::platform::env {

namespace {

// openkal answers a length rather than a terminated string: the length the
// value has, which may exceed the capacity offered, or a negated error.
template <class Read>
std::optional<std::string> read_sized(Read read) {
    std::string buffer(256, '\0');
    while (true) {
        const kal_intptr length { read(buffer.data(), buffer.size()) };
        if (length < 0) return std::nullopt;
        if (static_cast<std::size_t>(length) <= buffer.size()) {
            buffer.resize(static_cast<std::size_t>(length));
            return buffer;
        }
        buffer.resize(static_cast<std::size_t>(length));
    }
}

std::optional<std::string> variable_name_at(kal_uintptr index) {
    return read_sized([&](char* out, kal_uintptr cap) { return kal_env_var_at(index, out, cap); });
}

std::optional<std::string> exact_value(std::string_view name) {
    return read_sized([&](char* out, kal_uintptr cap) { return kal_env_var(name.data(), name.size(), out, cap); });
}

std::vector<std::string> names() {
    std::vector<std::string> result;
    const kal_uintptr count { kal_env_var_count() };
    result.reserve(count);
    for (kal_uintptr i { 0 }; i < count; ++i) {
        if (auto name = variable_name_at(i); name && !name->empty()) result.push_back(std::move(*name));
    }
    return result;
}

} // namespace

std::vector<std::string> variables() {
    std::vector<std::string> result;
    for (const auto& name : names()) {
        if (auto value = exact_value(name)) result.push_back(name + "=" + *value);
    }
    return result;
}

std::optional<std::string> get(std::string_view name) {
    if (auto value = exact_value(name)) return value;
    if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) {
        // Windows names compare without regard to case ("Path" is PATH).
        for (const auto& candidate : names()) {
            if (base::iequals_ascii(candidate, name)) return exact_value(candidate);
        }
    }
    return std::nullopt;
}

namespace {

std::vector<std::string> split_path_list(std::string_view pathList) {
    std::vector<std::string> result;
    for (auto piece : base::split(pathList, mcppls::os::PATH_LIST_SEPARATOR)) {
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

std::optional<std::string> find_executable(std::string_view name, std::string_view pathList) {
    if (name.empty()) return std::nullopt;

    // What to look for, in order.
    //
    // On POSIX: the name, and nothing else. Any file can be executable.
    //
    // On Windows: what makes a file runnable is its extension, and there are several — npm, npx,
    // code, gradle and xlings are `.cmd` shims and there is no `npm.exe` anywhere. So each
    // executable extension is tried, `.exe` first, which is the order Windows itself resolves in.
    //
    // A name that already carries one of those extensions is taken as written and nothing is
    // appended — `npm.cmd` must not become `npm.cmd.exe`. A name carrying anything else is not
    // carrying an extension at all (`python3.13`), so that one is still appended to.
    //
    // What is deliberately NOT tried is the bare name on Windows. npm's directory holds both
    // `npm.cmd` and an extension-less `npm`, the POSIX shell script; picking the latter gets as far
    // as spawning it and then fails with an I/O error, because Windows cannot run it.
    std::vector<std::string> wanted;
    if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) {
        constexpr std::array runnable { ".exe", ".cmd", ".bat", ".com" };
        const std::string own { base::to_lower_ascii(std::string { base::extension(name) }) };
        if (std::ranges::contains(runnable, own)) {
            wanted.emplace_back(name);
        } else {
            for (const std::string_view extension : runnable) {
                wanted.push_back(std::string { name } + std::string { extension });
            }
        }
    } else {
        wanted.emplace_back(name);
    }

    for (const auto& file : wanted) {
        if (base::is_absolute_path(file)) {
            if (fs::is_regular_file(file)) return base::normalize_path(file);
            continue;
        }
        if (file.find('/') != std::string::npos || file.find('\\') != std::string::npos) continue;
        for (const auto& directory : split_path_list(pathList)) {
            if (!base::is_absolute_path(directory)) continue;
            const std::string candidate { base::join_path(directory, file) };
            if (fs::is_regular_file(candidate)) return candidate;
        }
    }
    return std::nullopt;
}

std::vector<std::string> arguments() {
    std::vector<std::string> result;
    const kal_uintptr count { kal_env_arg_count() };
    for (kal_uintptr i { 0 }; i < count; ++i) {
        result.push_back(read_sized([&](char* out, kal_uintptr cap) { return kal_env_arg(i, out, cap); }).value_or(std::string {}));
    }
    return result;
}

} // namespace mcppls::platform::env
