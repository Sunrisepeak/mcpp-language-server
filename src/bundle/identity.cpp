module mcppls.bundle.identity;

import std;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.bundle.redact;
import mcpplibs.os;
import mcppls.base.home;

namespace os = mcpplibs::os;

namespace mcppls::bundle {

namespace {

void add_unique(std::vector<std::string>& list, std::string value) {
    value = std::string { base::trim(value) };
    if (value.empty()) return;
    if (std::ranges::any_of(list, [&](const std::string& known) { return base::iequals_ascii(known, value); })) return;
    list.push_back(std::move(value));
}

std::string first_line(std::string_view text) {
    const auto lines = base::split_lines(text);
    return lines.empty() ? std::string {} : std::string { base::trim(lines.front()) };
}

// The value of `<key>name</key><string>value</string>` in an XML property list.
std::optional<std::string> plist_string(std::string_view plist, std::string_view key) {
    const std::string marker { std::format("<key>{}</key>", key) };
    const std::size_t at { plist.find(marker) };
    if (at == std::string_view::npos) return std::nullopt;
    const std::size_t open { plist.find("<string>", at + marker.size()) };
    if (open == std::string_view::npos || open - (at + marker.size()) > 64) return std::nullopt;
    const std::size_t close { plist.find("</string>", open) };
    if (close == std::string_view::npos) return std::nullopt;
    return std::string { plist.substr(open + 8, close - open - 8) };
}

std::vector<std::string> host_names() {
    std::vector<std::string> hosts;
    for (const std::string_view name : { "COMPUTERNAME", "HOSTNAME", "HOST" }) {
        if (auto value = os::env::get(name)) add_unique(hosts, *value);
    }
    if (os::host() == os::platform::linux) {
        for (const std::string_view file : { "/proc/sys/kernel/hostname", "/etc/hostname" }) {
            if (auto text = os::fs::read(file)) add_unique(hosts, first_line(*text));
        }
    } else if (os::host() == os::platform::macos) {
        // What `scutil --get LocalHostName` and `ComputerName` answer, without starting scutil. A
        // binary property list is not read; the environment and the bundle's own check remain.
        if (auto plist = os::fs::read("/Library/Preferences/SystemConfiguration/preferences.plist"); plist && !plist->starts_with("bplist")) {
            for (const std::string_view key : { "LocalHostName", "ComputerName", "HostName" }) {
                if (auto value = plist_string(*plist, key)) add_unique(hosts, *value);
            }
        }
    }
    return hosts;
}

} // namespace

Identity current_identity() {
    Identity identity;
    const std::string home { base::home_directory() };
    add_unique(identity.homes, base::normalize_path(home));
    add_unique(identity.homes, base::normalize_path(base::canonical_path(home)));
    if (os::host() == os::platform::windows) {
        // HOMEDRIVE and HOMEPATH can name another directory than USERPROFILE (a redirected home).
        auto drive = os::env::get("HOMEDRIVE");
        auto path = os::env::get("HOMEPATH");
        if (drive && path && !path->empty() && *path != "\\") add_unique(identity.homes, base::normalize_path(*drive + *path));
    }
    for (const std::string_view name : { "USER", "LOGNAME", "USERNAME" }) {
        if (auto value = os::env::get(name)) add_unique(identity.users, *value);
    }
    for (const auto& spelling : identity.homes) add_unique(identity.users, std::string { base::file_name(spelling) });
    identity.hosts = host_names();
    return identity;
}

Identity hiding_workspaces(Identity identity, std::vector<std::string> roots) {
    for (auto& root : roots) {
        if (!root.empty()) identity.workspaces.push_back(base::normalize_path(root));
    }
    return identity;
}

} // namespace mcppls::bundle
