module mcppls.base.home;

import std;
import mcpplibs.os;
import mcppls.base.path;

namespace mcppls::base {

std::string home_directory() {
    if (const auto home = mcpplibs::os::dirs::home(); home && !home->empty()) return spelled(*home);
    if (mcpplibs::os::host() == mcpplibs::os::platform::windows) {
        const auto drive = mcpplibs::os::env::get("HOMEDRIVE");
        const auto path = mcpplibs::os::env::get("HOMEPATH");
        if (drive && path) return normalize_path(*drive + *path);
        return "C:/";
    }
    return "/";
}

std::string cache_directory() {
    if (const auto overridden = mcpplibs::os::env::get("MCPPLS_CACHE_DIR"); overridden && !overridden->empty()) return normalize_path(*overridden);
    if (const auto cache = mcpplibs::os::dirs::cache(); cache && is_absolute_path(spelled(*cache))) return join_path(spelled(*cache), "mcppls");
    switch (mcpplibs::os::host()) {
    case mcpplibs::os::platform::windows: return join_path(home_directory(), "AppData/Local/mcppls");
    case mcpplibs::os::platform::macos: return join_path(home_directory(), "Library/Caches/mcppls");
    default: break;
    }
    return join_path(home_directory(), ".cache/mcppls");
}

} // namespace mcppls::base
