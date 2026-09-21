module mcppls.toolchain.discover;

import std;
import mcppls.os;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.platform.dirs;
import mcppls.spec.database;
import mcppls.toolchain.probe;

namespace mcppls::toolchain {

namespace {

void add(std::vector<CompilerCandidate>& out, std::string driver, std::string_view origin) {
    driver = base::normalize_path(driver);
    if (!platform::fs::is_regular_file(driver)) return;
    if (std::ranges::any_of(out, [&](const CompilerCandidate& candidate) { return base::same_path(candidate.driver, driver); })) return;
    out.push_back(CompilerCandidate { driver, classify_driver(driver), std::string { origin } });
}

// <store>/xim-x-gcc/<version>/bin/g++ and <store>/xim-x-llvm/<version>/bin/clang++, newest first.
void add_store(std::vector<CompilerCandidate>& out, std::string_view store, std::string_view origin) {
    const std::string suffix { mcppls::os::EXECUTABLE_SUFFIX };
    for (const auto& [package, driver] : { std::pair { "xim-x-llvm", "clang++" }, std::pair { "xim-x-gcc", "g++" } }) {
        auto versions = platform::fs::list_directory(base::join_path(store, package));
        std::ranges::sort(versions, std::greater<> {});
        for (const auto& version : versions) add(out, base::join_path(version, std::format("bin/{}{}", driver, suffix)), origin);
    }
}

} // namespace

bool macos_developer_tools_present() {
    if constexpr (mcppls::os::FAMILY != mcppls::os::Family::macos) {
        return false;
    } else {
        // The order xcrun follows: DEVELOPER_DIR, the directory `xcode-select -s` recorded, then the defaults.
        if (auto directory = platform::env::get("DEVELOPER_DIR"); directory && platform::fs::is_directory(*directory)) return true;
        if (const std::string selected { platform::fs::canonical_path("/var/db/xcode_select_link") };
            selected != "/var/db/xcode_select_link" && platform::fs::is_directory(base::join_path(selected, "usr/bin"))) {
            return true;
        }
        return platform::fs::is_regular_file("/Library/Developer/CommandLineTools/usr/bin/clang++")
            || platform::fs::is_directory("/Applications/Xcode.app/Contents/Developer/usr/bin");
    }
}

std::vector<CompilerCandidate> discover_compilers(const Runner& runner) {
    std::vector<CompilerCandidate> out;
    // On macOS the compilers in /usr/bin are shims; without developer tools, probing one asks the
    // person to install them (usable plan W5.4: the extension asks once, the server never).
    const bool shimsUsable { mcppls::os::FAMILY != mcppls::os::Family::macos || macos_developer_tools_present() };
    for (std::string_view name : { "c++", "g++", "clang++", "cl", "clang-cl" }) {
        auto found = platform::env::find_executable(name);
        if (!found) continue;
        if (!shimsUsable && base::is_within(*found, "/usr/bin")) continue;
        add(out, *found, "PATH");
    }
    const std::string home { platform::dirs::home_directory() };
    add_store(out, base::join_path(home, ".mcpp/registry/data/xpkgs"), "mcpp");
    add_store(out, base::join_path(home, ".xlings/data/xpkgs"), "xlings");
    if constexpr (mcppls::os::FAMILY == mcppls::os::Family::macos) {
        add(out, "/opt/homebrew/opt/llvm/bin/clang++", "homebrew");
    }
    if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) {
        const std::string vswhere { "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe" };
        if (platform::fs::is_regular_file(vswhere)) {
            const std::vector<std::string> argv { vswhere, "-latest", "-products", "*", "-requires",
                                                  "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath" };
            if (auto result = runner(argv); result && result->exitCode == 0) {
                const std::string installation { base::normalize_path(base::trim(result->output)) };
                auto versions = platform::fs::list_directory(base::join_path(installation, "VC/Tools/MSVC"));
                std::ranges::sort(versions, std::greater<> {});
                for (const auto& version : versions) add(out, base::join_path(version, "bin/Hostx64/x64/cl.exe"), "visual-studio");
                add(out, base::join_path(installation, "VC/Tools/Llvm/x64/bin/clang-cl.exe"), "visual-studio");
            }
        }
    }
    return out;
}

} // namespace mcppls::toolchain
