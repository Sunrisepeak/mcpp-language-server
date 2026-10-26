module mcppls.platform.preopen;

import std;
import mcpplibs.os;
import mcppls.base.log;
import mcppls.base.path;

namespace mcppls::platform {

bool has_root_preopen(std::span<const std::string> names) {
    if (base::native_path_style() != base::PathStyle::posix) return true;
    return std::ranges::any_of(names, [](const std::string& name) { return name == "/"; });
}

std::vector<std::string> preopen_names() {
    static const std::vector<std::string> names { [] {
        auto table = mcpplibs::os::fs::preopens();
        if (!has_root_preopen(table)) {
            std::string shown;
            for (const auto& name : table) {
                std::string clean;
                for (const char c : name) clean += (c >= 0x20 && c < 0x7f) ? c : '?';
                shown += std::format("{}'{}'", shown.empty() ? "" : ", ", clean);
            }
            base::log::warning("no preopened directory is named \"/\" (the table is: {}); programs cannot be started by "
                               "absolute path", shown.empty() ? std::string { "empty" } : shown);
        }
        return table;
    }() };
    return names;
}

} // namespace mcppls::platform
