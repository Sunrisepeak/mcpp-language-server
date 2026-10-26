module mcppls.platform.dirs;

import std;
import mcpplibs.os;
import mcppls.os;
import mcppls.base.path;
import mcppls.platform.env;

namespace mcppls::platform::dirs {

namespace {

std::optional<std::string> non_empty(std::string_view name) {
    auto value = env::get(name);
    if (!value || value->empty()) return std::nullopt;
    return base::normalize_path(*value);
}

std::string spelled(const std::filesystem::path& path) { return base::normalize_path(path.generic_string()); }

} // namespace

// The conventions are mcpplibs.os's (dirs: $HOME, %USERPROFILE%; the user cache; the temporary
// directory), so a cache keeps its place across an upgrade; what is here is where a variable is
// missing altogether, and the override.
std::string home_directory() {
    if (auto home = mcpplibs::os::dirs::home()) return spelled(*home);
    if (mcppls::os::family() == mcppls::os::Family::windows) {
        auto drive = env::get("HOMEDRIVE");
        auto path = env::get("HOMEPATH");
        if (drive && path) return base::normalize_path(*drive + *path);
        return "C:/";
    }
    return "/";
}

std::string cache_directory() {
    if (auto overridden = non_empty("MCPPLS_CACHE_DIR")) return *overridden;
    if (auto cache = mcpplibs::os::dirs::cache(); cache && base::is_absolute_path(spelled(*cache)))
        return base::join_path(spelled(*cache), "mcppls");
    switch (mcppls::os::family()) {
    case mcppls::os::Family::windows: return base::join_path(home_directory(), "AppData/Local/mcppls");
    case mcppls::os::Family::macos: return base::join_path(home_directory(), "Library/Caches/mcppls");
    case mcppls::os::Family::linux: break;
    }
    return base::join_path(home_directory(), ".cache/mcppls");
}

std::string temp_directory() { return spelled(mcpplibs::os::dirs::temp()); }

} // namespace mcppls::platform::dirs
