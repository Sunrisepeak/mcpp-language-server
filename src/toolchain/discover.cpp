module mcppls.toolchain.discover;

import std;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.spec.database;
import mcppls.toolchain.probe;
import mcpplibs.os;
import mcppls.base.home;
import mcppls.toolrun;

namespace os = mcpplibs::os;

namespace mcppls::toolchain {

namespace {

void add(std::vector<CompilerCandidate>& out, std::string driver, std::string_view origin) {
    std::error_code ignored;
    driver = base::normalize_path(driver);
    if (!std::filesystem::is_regular_file(driver, ignored)) return;
    if (std::ranges::any_of(out, [&](const CompilerCandidate& candidate) { return base::same_path(candidate.driver, driver); })) return;
    out.push_back(CompilerCandidate { driver, classify_driver(driver), std::string { origin } });
}

// <store>/xim-x-gcc/<version>/bin/g++ and <store>/xim-x-llvm/<version>/bin/clang++, newest first.
void add_store(std::vector<CompilerCandidate>& out, std::string_view store, std::string_view origin) {
    const std::string suffix { os::conventions().exe_suffix };
    for (const auto& [package, driver] : { std::pair { "xim-x-llvm", "clang++" }, std::pair { "xim-x-gcc", "g++" } }) {
        auto versions = base::children(base::join_path(store, package));
        std::ranges::sort(versions, std::greater<> {});
        for (const auto& version : versions) add(out, base::join_path(version, std::format("bin/{}{}", driver, suffix)), origin);
    }
}

} // namespace

bool macos_developer_tools_present() {
    std::error_code ignored;
    if (os::host() != os::platform::macos) {
        return false;
    } else {
        // The order xcrun follows: DEVELOPER_DIR, the directory `xcode-select -s` recorded, then the defaults.
        if (auto directory = os::env::get("DEVELOPER_DIR"); directory && std::filesystem::is_directory(*directory, ignored)) return true;
        if (const std::string selected { base::canonical_path("/var/db/xcode_select_link") };
            selected != "/var/db/xcode_select_link" && std::filesystem::is_directory(base::join_path(selected, "usr/bin"), ignored)) {
            return true;
        }
        return std::filesystem::is_regular_file("/Library/Developer/CommandLineTools/usr/bin/clang++", ignored)
            || std::filesystem::is_directory("/Applications/Xcode.app/Contents/Developer/usr/bin", ignored);
    }
}

std::vector<CompilerCandidate> discover_compilers(const Runner& runner) {
    std::error_code ignored;
    std::vector<CompilerCandidate> out;
    // On macOS the compilers in /usr/bin are shims; without developer tools, probing one asks the
    // person to install them (usable plan W5.4: the extension asks once, the server never).
    const bool shimsUsable { os::host() != os::platform::macos || macos_developer_tools_present() };
    for (std::string_view name : { "c++", "g++", "clang++", "cl", "clang-cl" }) {
        auto found = toolrun::find_program(name);
        if (!found) continue;
        if (!shimsUsable && base::is_within(*found, "/usr/bin")) continue;
        add(out, *found, "PATH");
    }
    const std::string home { base::home_directory() };
    add_store(out, base::join_path(home, ".mcpp/registry/data/xpkgs"), "mcpp");
    add_store(out, base::join_path(home, ".xlings/data/xpkgs"), "xlings");
    if (os::host() == os::platform::macos) {
        add(out, "/opt/homebrew/opt/llvm/bin/clang++", "homebrew");
    }
    if (os::host() == os::platform::windows) {
        const std::string vswhere { "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe" };
        if (std::filesystem::is_regular_file(vswhere, ignored)) {
            const std::vector<std::string> argv { vswhere, "-latest", "-products", "*", "-requires",
                                                  "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath" };
            if (auto result = runner(argv); result && result->status.success()) {
                const std::string installation { base::normalize_path(base::trim(result->out)) };
                auto versions = base::children(base::join_path(installation, "VC/Tools/MSVC"));
                std::ranges::sort(versions, std::greater<> {});
                for (const auto& version : versions) add(out, base::join_path(version, "bin/Hostx64/x64/cl.exe"), "visual-studio");
                add(out, base::join_path(installation, "VC/Tools/Llvm/x64/bin/clang-cl.exe"), "visual-studio");
            }
        }
    }
    return out;
}

} // namespace mcppls::toolchain
