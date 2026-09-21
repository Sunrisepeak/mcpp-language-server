module mcppls.platform.dirs;

import std;
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

} // namespace

std::string home_directory() {
    if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) {
        if (auto profile = non_empty("USERPROFILE")) return *profile;
        auto drive = env::get("HOMEDRIVE");
        auto path = env::get("HOMEPATH");
        if (drive && path) return base::normalize_path(*drive + *path);
        return "C:/";
    } else {
        if (auto home = non_empty("HOME")) return *home;
        return "/";
    }
}

std::string cache_directory() {
    if (auto overridden = non_empty("MCPPLS_CACHE_DIR")) return *overridden;
    if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) {
        if (auto local = non_empty("LOCALAPPDATA")) return base::join_path(*local, "mcppls");
        return base::join_path(home_directory(), "AppData/Local/mcppls");
    } else if constexpr (mcppls::os::FAMILY == mcppls::os::Family::macos) {
        return base::join_path(home_directory(), "Library/Caches/mcppls");
    } else {
        if (auto xdg = non_empty("XDG_CACHE_HOME"); xdg && base::is_absolute_path(*xdg)) return base::join_path(*xdg, "mcppls");
        return base::join_path(home_directory(), ".cache/mcppls");
    }
}

std::string temp_directory() {
    if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) {
        for (std::string_view name : { "TEMP", "TMP" }) {
            if (auto value = non_empty(name)) return *value;
        }
        return base::join_path(home_directory(), "AppData/Local/Temp");
    } else {
        if (auto value = non_empty("TMPDIR")) return *value;
        return "/tmp";
    }
}

} // namespace mcppls::platform::dirs
