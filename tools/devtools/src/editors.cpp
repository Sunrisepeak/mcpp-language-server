module mcppls.devtools.editors;

import std;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.pack.archive;
import mcpplibs.os;
import mcppls.base.home;

namespace os = mcpplibs::os;
namespace mcppls::devtools::editors {
namespace {
namespace archive = mcppls::pack::archive;

bool windows() { return os::host() == os::platform::windows; }
bool macos() { return os::host() == os::platform::macos; }

std::optional<std::string> absolute_variable(std::string_view name) {
    auto value = os::env::get(name);
    if (!value || value->empty() || !base::is_absolute_path(*value)) return std::nullopt;
    return base::normalize_path(*value);
}

// A link is only ever removed as a link: following it would delete the repository's editors/zed.
bool is_link(const std::string& path) {
    std::error_code error;
    return std::filesystem::is_symlink(std::filesystem::path { path }, error);
}

void remove_entry(const std::string& path) {
    std::error_code ignored;
    std::error_code error;
    if (is_link(path)) {
        std::filesystem::remove(std::filesystem::path { path }, error);
        return;
    }
    std::filesystem::remove_all(path, ignored);
}

// The `id = "..."` line of an extension.toml, which is all this needs to know whether an entry
// under installed/ is this extension.
std::optional<std::string> extension_id(const std::string& directory) {
    auto text = os::fs::read(base::join_path(directory, "extension.toml"));
    if (!text) return std::nullopt;
    for (const auto& line : base::split_lines(*text)) {
        const std::string_view trimmed { base::trim(line) };
        if (!trimmed.starts_with("id")) continue;
        const std::string_view value { base::trim(trimmed.substr(2)) };
        if (!value.starts_with('=')) continue;   // `identity = ...` is some other key
        const std::string_view quoted { base::trim(value.substr(1)) };
        if (quoted.size() < 2 || quoted.front() != '"' || quoted.back() != '"') continue;
        return std::string { quoted.substr(1, quoted.size() - 2) };
    }
    return std::nullopt;
}

std::string jetbrains_directory() {
    if (windows()) {
        if (auto roaming = absolute_variable("APPDATA")) return base::join_path(*roaming, "JetBrains");
        return base::join_path(base::home_directory(), "AppData/Roaming/JetBrains");
    } else {
        return base::join_path(user_data_directory(), "JetBrains");
    }
}

// "CLion2025.2" -> {2025, 2}, and nothing for CLionNova, CLion-backup or a stray file.
std::optional<std::pair<int, int>> clion_version(std::string_view name) {
    if (!name.starts_with("CLion")) return std::nullopt;
    name.remove_prefix(5);
    const auto dot = name.find('.');
    if (dot == std::string_view::npos) return std::nullopt;
    int major { 0 };
    int minor { 0 };
    const auto parse = [](std::string_view digits, int& out) {
        if (digits.empty()) return false;
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), out);
        return error == std::errc {} && end == digits.data() + digits.size();
    };
    if (!parse(name.substr(0, dot), major) || !parse(name.substr(dot + 1), minor)) return std::nullopt;
    return std::pair { major, minor };
}

} // namespace

std::string user_data_directory() {
    if (windows()) {
        if (auto local = absolute_variable("LOCALAPPDATA")) return *local;
        return base::join_path(base::home_directory(), "AppData/Local");
    } else if (macos()) {
        return base::join_path(base::home_directory(), "Library/Application Support");
    } else {
        if (auto xdg = absolute_variable("XDG_DATA_HOME")) return *xdg;
        return base::join_path(base::home_directory(), ".local/share");
    }
}

std::string server_directory() { return base::join_path(user_data_directory(), "mcppls/payload"); }

std::string zed_directory() {
    return base::join_path(user_data_directory(), (windows() || macos()) ? "Zed" : "zed");
}

base::Result<ZedInstall> install_zed(const std::string& zedDirectory, const std::string& source) {
    std::error_code ignored;
    for (std::string_view file : { "extension.toml", "extension.wasm" }) {
        if (!std::filesystem::is_regular_file(base::join_path(source, file), ignored)) {
            return base::fail("zed-source", std::format("{} has no {}; build the extension first", source, file));
        }
    }
    const std::string installed { base::join_path(zedDirectory, "extensions/installed") };
    if (std::error_code failed; !std::filesystem::create_directories(installed, failed) && failed) return base::fail("create-directory", std::format("cannot create {}: {}", installed, failed.message()));
    const std::string target { base::join_path(installed, ZED_EXTENSION_ID) };

    if (is_link(target) || std::filesystem::exists(target, ignored)) {
        if (!is_link(target) && extension_id(target) != std::string { ZED_EXTENSION_ID }) {
            return base::fail("zed-occupied", std::format("{} exists and is not this extension; not touching it", target));
        }
        remove_entry(target);
    }

    std::error_code error;
    std::filesystem::create_directory_symlink(std::filesystem::path { source }, std::filesystem::path { target }, error);
    if (!error) return ZedInstall::linked;

    // Windows grants a link only to an administrator or in developer mode. A copy of the two files
    // Zed loads is what an extension from its store looks like, so it loads the same way; it is
    // just not rebuilt in place.
    if (std::error_code failed; !std::filesystem::create_directories(target, failed) && failed) return base::fail("create-directory", std::format("cannot create {}: {}", target, failed.message()));
    for (std::string_view file : { "extension.toml", "extension.wasm" }) {
        auto content = os::fs::read(base::join_path(source, file));
        if (!content) return base::fail("read-file", content.error());
        if (auto written = os::fs::write_atomic(base::join_path(target, file), *content); !written) {
            return base::fail("write-file", written.error());
        }
    }
    return ZedInstall::copied;
}

bool zed_installed(const std::string& zedDirectory) {
    std::error_code ignored;
    const std::string target { base::join_path(zedDirectory, std::format("extensions/installed/{}", ZED_EXTENSION_ID)) };
    return is_link(target) || std::filesystem::exists(target, ignored);
}

bool remove_zed(const std::string& zedDirectory) {
    std::error_code ignored;
    const bool was { zed_installed(zedDirectory) };
    remove_entry(base::join_path(zedDirectory, std::format("extensions/installed/{}", ZED_EXTENSION_ID)));
    // Zed's own uninstall removes the work directory as well; a stale one would outlive us.
    std::filesystem::remove_all(base::join_path(zedDirectory, std::format("extensions/work/{}", ZED_EXTENSION_ID)), ignored);
    return was;
}

std::vector<std::string> clion_plugin_directories() {
    std::error_code ignored;
    const std::string root { jetbrains_directory() };
    std::vector<std::pair<std::pair<int, int>, std::string>> found;
    for (const auto& path : base::children(root)) {
        if (!std::filesystem::is_directory(path, ignored)) continue;
        auto version = clion_version(base::file_name(path));
        if (!version) continue;
        // Linux keeps plugins directly in the versioned data directory; macOS and Windows keep
        // them in its `plugins/` (JetBrains, "Directories used by the IDE").
        found.emplace_back(*version, (windows() || macos()) ? base::join_path(path, "plugins") : path);
    }
    std::ranges::sort(found, std::greater {}, [](const auto& entry) { return entry.first; });
    std::vector<std::string> directories;
    for (auto& [version, path] : found) directories.push_back(std::move(path));
    return directories;
}

base::Result<void> install_clion(const std::string& pluginsDirectory, const std::string& archivePath) {
    std::error_code ignored;
    auto entries = archive::list(archivePath);
    if (!entries) return std::unexpected { entries.error() };
    const std::string prefix { std::format("{}/", CLION_PLUGIN_DIRECTORY) };
    const bool ours { !entries->empty() && std::ranges::all_of(*entries, [&](const archive::Entry& entry) {
        return entry.path.starts_with(prefix) || entry.path == CLION_PLUGIN_DIRECTORY;
    }) };
    if (!ours) {
        return base::fail("clion-archive", std::format("{} does not unpack to {}/; not a build of this plugin",
                                                       archivePath, CLION_PLUGIN_DIRECTORY));
    }
    if (std::error_code failed; !std::filesystem::create_directories(pluginsDirectory, failed) && failed) return base::fail("create-directory", std::format("cannot create {}: {}", pluginsDirectory, failed.message()));
    std::filesystem::remove_all(base::join_path(pluginsDirectory, CLION_PLUGIN_DIRECTORY), ignored);
    auto written = archive::extract(archivePath, pluginsDirectory, [](std::string_view) { return true; },
                                    archive::Options { .stripTopLevel = false });
    if (!written) return std::unexpected { written.error() };
    return {};
}

bool clion_installed(const std::string& pluginsDirectory) {
    std::error_code ignored;
    return std::filesystem::is_directory(base::join_path(pluginsDirectory, CLION_PLUGIN_DIRECTORY), ignored);
}

bool remove_clion(const std::string& pluginsDirectory) {
    std::error_code ignored;
    const bool was { clion_installed(pluginsDirectory) };
    std::filesystem::remove_all(base::join_path(pluginsDirectory, CLION_PLUGIN_DIRECTORY), ignored);
    return was;
}

} // namespace mcppls::devtools::editors
