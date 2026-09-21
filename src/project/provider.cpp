module mcppls.project.provider;

import std;
import mcppls.os;
import mcppls.platform.env;
import mcppls.platform.fs;

namespace mcppls::project {

std::optional<std::string> find_tool(std::string_view name, std::span<const std::string> fallbacks) {
    if (auto found = platform::env::find_executable(name)) return found;
    for (const auto& fallback : fallbacks) {
        const std::string candidate { fallback + std::string { mcppls::os::EXECUTABLE_SUFFIX } };
        if (platform::fs::is_regular_file(candidate)) return candidate;
    }
    return std::nullopt;
}

} // namespace mcppls::project
