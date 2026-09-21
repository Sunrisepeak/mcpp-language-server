module mcppls.devtools.release;

import std;
import mcpplibs.cmdline;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.fs;
import mcppls.pack.release;
import mcppls.pack.archive;
import mcppls.pack.fetch;
import mcppls.devtools.common;
import mcppls.devtools.version;

namespace mcppls::devtools::release {
namespace fs = mcppls::platform::fs;
namespace pack = mcppls::pack;
namespace {

// One xlings-res platform: mcppls.os's VS Code target triple, and the {os}_{arch} xlings names
// (payload.lock.json and the .lua.in templates both spell it this way).
struct PlatformInfo {
    std::string_view vscodeTarget;
    std::string_view osName;
    std::string_view arch;
};
constexpr std::array<PlatformInfo, 3> PLATFORMS { {
    { "linux-x64", "linux", "x86_64" },
    { "darwin-arm64", "macosx", "aarch64" },
    { "win32-x64", "windows", "x86_64" },
} };

std::string upper_ascii(std::string_view text) {
    std::string out { text };
    for (auto& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

// The first `@[A-Z0-9_]+@`-shaped placeholder still in `text`, for reporting a template that was
// not fully rendered -- without pulling in <regex> for one shape.
std::optional<std::string> leftover_placeholder(std::string_view text) {
    for (std::size_t i { 0 }; i < text.size(); ++i) {
        if (text[i] != '@') continue;
        std::size_t j { i + 1 };
        while (j < text.size() && ((text[j] >= 'A' && text[j] <= 'Z') || (text[j] >= '0' && text[j] <= '9') || text[j] == '_')) ++j;
        if (j > i + 1 && j < text.size() && text[j] == '@') return std::string { text.substr(i, j - i + 1) };
    }
    return std::nullopt;
}

} // namespace

base::Result<pack::release::CheckReport> check_release(const std::string& root, const std::string& version,
                                                        const std::string& directory, bool render) {
    auto manifest = pack::release::load_manifest(base::join_path(root, "packaging/release.manifest.json"));
    if (!manifest) return std::unexpected { manifest.error() };
    return pack::release::check(*manifest, version, directory, render);
}

base::Result<XlingsResult> make_xlings_artifacts(const std::string& root, const std::string& payloadsDir,
                                                  const std::string& outDir, std::optional<std::string> version,
                                                  std::optional<std::string> kitVersion,
                                                  std::optional<std::string> clangdVersion) {
    if (auto made = fs::create_directories(outDir); !made) return std::unexpected { made.error() };

    std::string productVersion;
    if (version && !version->empty()) {
        productVersion = *version;
    } else {
        auto read = mcppls::devtools::version::manifest_version(root);
        if (!read) return std::unexpected { read.error() };
        productVersion = *read;
    }

    const std::string lockPath { base::join_path(root, "packaging/payload.lock.json") };
    auto lockText = fs::read_file(lockPath);
    if (!lockText) return std::unexpected { lockText.error() };
    nlohmann::json lock;
    try {
        lock = nlohmann::json::parse(*lockText);
    } catch (const std::exception& error) {
        return base::fail("xlings-artifacts", std::format("{} is not valid JSON: {}", lockPath, error.what()));
    }

    std::string effectiveKitVersion { kitVersion.value_or("") };
    if (effectiveKitVersion.empty() && lock.contains("libcxx-version") && lock["libcxx-version"].is_string()) {
        effectiveKitVersion = lock["libcxx-version"].get<std::string>();
    }
    std::string effectiveClangdVersion { clangdVersion.value_or("") };
    if (effectiveClangdVersion.empty() && lock.contains("clangd-version") && lock["clangd-version"].is_string()) {
        effectiveClangdVersion = lock["clangd-version"].get<std::string>();
    }

    std::map<std::string, std::string> values;
    values["@VERSION@"] = productVersion;
    values["@KIT_VERSION@"] = effectiveKitVersion;
    values["@CLANGD_VERSION@"] = effectiveClangdVersion;

    XlingsResult result {};
    for (const auto& platform : PLATFORMS) {
        const std::string tarball { base::join_path(payloadsDir, std::format("payload-{}.tar.gz", platform.vscodeTarget)) };
        if (!fs::is_regular_file(tarball)) {
            return base::fail("xlings-artifacts", std::format("missing {}", tarball));
        }

        // The archive's own top level is `payload/` (what CI's payload artifacts carry); stripping
        // it (extract's default) leaves this scratch directory holding bin/, kit/, ... directly.
        const std::string scratch { base::join_path(outDir, std::format(".scratch-{}", platform.vscodeTarget)) };
        fs::remove_all(scratch);
        auto extracted = pack::archive::extract(tarball, scratch, [](std::string_view) { return true; });
        if (!extracted) return std::unexpected { extracted.error() };

        const bool windows { platform.vscodeTarget == "win32-x64" };
        const std::string exe { windows ? ".exe" : "" };

        const std::string serverName { std::format("mcpp-language-server-{}-{}-{}", productVersion, platform.osName, platform.arch) };
        const std::string serverArchive { base::join_path(outDir, serverName + ".tar.gz") };
        const std::vector<pack::archive::WriteEntry> serverEntries {
            { base::join_path(scratch, "bin/mcppls" + exe), "bin/mcppls" + exe },
            { base::join_path(root, "LICENSE"), "LICENSE" },
        };
        if (auto wrote = pack::archive::write(serverArchive, serverName, serverEntries); !wrote) {
            return std::unexpected { wrote.error() };
        }

        const std::string kitName { std::format("mcppls-kit-{}-{}-{}", effectiveKitVersion, platform.osName, platform.arch) };
        const std::string kitArchive { base::join_path(outDir, kitName + ".tar.gz") };
        const std::string kitDir { base::join_path(scratch, "kit") };
        // `list_directory` already returns each entry joined onto `kitDir`; the archive path is
        // the bare name alone (write() joins it under the kit's own root itself).
        std::vector<pack::archive::WriteEntry> kitEntries;
        for (const auto& path : fs::list_directory(kitDir)) {
            kitEntries.push_back({ path, std::string { base::file_name(path) } });
        }
        if (auto wrote = pack::archive::write(kitArchive, kitName, kitEntries); !wrote) {
            return std::unexpected { wrote.error() };
        }

        auto serverDigest = pack::fetch::digest_of(serverArchive);
        if (!serverDigest) return std::unexpected { serverDigest.error() };
        auto kitDigest = pack::fetch::digest_of(kitArchive);
        if (!kitDigest) return std::unexpected { kitDigest.error() };

        const std::string key { std::format("{}_{}", upper_ascii(platform.osName), upper_ascii(platform.arch)) };
        values[std::format("@SHA256_{}@", key)] = *serverDigest;
        values[std::format("@KIT_SHA256_{}@", key)] = *kitDigest;

        std::println("{}: {} {}", platform.vscodeTarget, std::filesystem::path { serverArchive }.filename().string(), *serverDigest);
        std::println("{}: {} {}", platform.vscodeTarget, std::filesystem::path { kitArchive }.filename().string(), *kitDigest);

        result.written.push_back(serverArchive);
        result.written.push_back(kitArchive);
        fs::remove_all(scratch);
    }

    for (const std::string_view templateName : { std::string_view { "mcpp-language-server.lua.in" }, std::string_view { "mcppls-kit.lua.in" } }) {
        const std::string templatePath { base::join_path(root, std::format("packaging/xlings/{}", templateName)) };
        auto text = fs::read_file(templatePath);
        if (!text) return std::unexpected { text.error() };
        std::string rendered { *text };
        for (const auto& [placeholder, value] : values) rendered = base::replace_all(rendered, placeholder, value);
        if (auto leftover = leftover_placeholder(rendered)) {
            return base::fail("xlings-artifacts", std::format("unrendered {} in {}", *leftover, templateName));
        }
        const std::string outPath { base::join_path(outDir, std::string { templateName.substr(0, templateName.size() - 3) }) };
        if (auto written = fs::write_file_atomic(outPath, rendered); !written) return std::unexpected { written.error() };
        std::println("rendered {}", outPath);
        result.written.push_back(outPath);
    }
    return result;
}

} // namespace mcppls::devtools::release

namespace mcppls::devtools {
namespace {

namespace cmdline = mcpplibs::cmdline;

int command_release_check(const cmdline::ParsedArgs& arguments) {
    const std::string root { repository_root() };
    const auto version = arguments.value("version");
    const auto directory = arguments.value("dir");
    if (!version || !directory) {
        std::println(std::cerr, "mcppls-devtools: release check needs --version and --dir");
        return 2;
    }
    auto report = release::check_release(root, *version, *directory, arguments.is_flag_set("render"));
    if (!report) {
        std::println(std::cerr, "mcppls-devtools: {}", report.error().message);
        return 1;
    }
    for (const auto& note : report->notes) std::println("  {}", note);
    if (!report->ok) {
        std::println(std::cerr, "\nrelease_manifest: the staged release does not match the manifest");
        for (const auto& problem : report->problems) std::println(std::cerr, "  {}", problem);
        return 1;
    }
    std::println("\nrelease_manifest: {} matches packaging/release.manifest.json", *version);
    return 0;
}

int command_release_xlings(const cmdline::ParsedArgs& arguments) {
    const std::string root { repository_root() };
    const auto payloads = arguments.value("payloads");
    const auto out = arguments.value("out");
    if (!payloads || !out) {
        std::println(std::cerr, "mcppls-devtools: release xlings needs --payloads and --out");
        return 2;
    }
    auto version = arguments.value("version");
    auto kitVersion = arguments.value("kit-version");
    auto clangdVersion = arguments.value("clangd-version");
    auto result = release::make_xlings_artifacts(root, *payloads, *out, version, kitVersion, clangdVersion);
    if (!result) {
        std::println(std::cerr, "mcppls-devtools: {}", result.error().message);
        return 1;
    }
    return 0;
}

// See mcppls.devtools.check's `dispatch` for why this second level of dispatch is done by hand:
// `cmdline::App::run` calls only the immediate matched subcommand's action, and `release` itself
// (the App main.cpp registers) never had one -- only `check`/`xlings` did, several levels too deep
// for the library to ever reach.
int dispatch(bool& handled, int& status, std::string_view verb, const cmdline::ParsedArgs& inner) {
    handled = true;
    if (verb == "check") return command_release_check(inner);
    if (verb == "xlings") return command_release_xlings(inner);
    std::println(std::cerr, "mcppls-devtools: release needs a verb: check, xlings");
    return 2;
}

} // namespace

cmdline::App release_command(bool& handled, int& status) {
    cmdline::App command { "release" };
    (void) command.description("Release-time checks and artifacts: the staged manifest, xlings-res archives");
    (void) command.subcommand("check")
        .description("Check a staged release directory against packaging/release.manifest.json")
        .option("version").takes_value().value_name("VERSION").help("The release being staged").option("dir")
        .takes_value().value_name("DIR").help("The staged release directory").option("render")
        .help("Also write MANIFEST.md into the directory");
    (void) command.subcommand("xlings")
        .description("Split CI's payload tarballs into xlings-res archives and render the .lua descriptors")
        .option("payloads").takes_value().value_name("DIR").help("Directory of payload-<platform>.tar.gz").option("out")
        .takes_value().value_name("DIR").help("Where to write the archives and rendered descriptors").option("version")
        .takes_value().value_name("VERSION").help("Default: mcpp.toml").option("kit-version")
        .takes_value().value_name("VERSION").help("Default: packaging/payload.lock.json's libcxx-version").option("clangd-version")
        .takes_value().value_name("VERSION").help("Default: packaging/payload.lock.json's clangd-version");
    (void) command.action([&handled, &status](const cmdline::ParsedArgs& arguments) {
        const auto sub = arguments.subcommand();
        static const cmdline::ParsedArgs EMPTY {};
        status = dispatch(handled, status, arguments.subcommand_name(), sub ? sub->get() : EMPTY);
    });
    return command;
}

} // namespace mcppls::devtools
