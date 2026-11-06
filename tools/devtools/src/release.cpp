module mcppls.devtools.release;

import std;
import mcpplibs.cmdline;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.base.version;
import mcppls.pack.release;
import mcppls.pack.archive;
import mcppls.pack.fetch;
import mcppls.pack.lock;
import mcppls.pack.targets;
import mcppls.devtools.common;
import mcppls.devtools.version;
import mcpplibs.os;

namespace os = mcpplibs::os;

namespace mcppls::devtools::release {
namespace pack = mcppls::pack;
namespace {

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
    // "{kit}": each platform's kit, as the lock names it.
    if (auto lockData = pack::lock::load(base::join_path(root, "packaging/payload.lock.json"))) {
        for (const auto& [platform, row] : lockData->platforms) {
            if (!row.kit.name.empty()) manifest->kits[platform] = row.kit.name;
        }
    }
    return pack::release::check(*manifest, version, directory, render);
}

namespace {

// A kit archive of the release, `mcppls-kit-<name>-<version>.tar.gz`: its name and version (a version has no `-`).
struct KitAsset {
    std::string file;
    std::string name;
    std::string version;
    std::string sha256;
};

std::optional<KitAsset> kit_asset(const std::string& path) {
    const std::string file { base::file_name(path) };
    constexpr std::string_view PREFIX { "mcppls-kit-" };
    constexpr std::string_view SUFFIX { ".tar.gz" };
    if (!file.starts_with(PREFIX) || !file.ends_with(SUFFIX)) return std::nullopt;
    const std::string stem { file.substr(PREFIX.size(), file.size() - PREFIX.size() - SUFFIX.size()) };
    const auto dash = stem.rfind('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 == stem.size()) return std::nullopt;
    return KitAsset { file, stem.substr(0, dash), stem.substr(dash + 1), {} };
}

// What a kit holds and is licensed under, for its descriptor.
std::pair<std::string, std::string> kit_description(std::string_view name) {
    if (name == "openkal") {
        return { "libc++ headers and std module sources over openkal-musl's headers, with the generated headers of x86_64 and aarch64",
                 "\"Apache-2.0 WITH LLVM-exception\", \"MIT\", \"Apache-2.0\"" };
    }
    if (name.ends_with("-w64-mingw32")) {
        return { "libc++ headers and std module sources with the MinGW-w64 headers", "\"Apache-2.0 WITH LLVM-exception\", \"ZPL-2.1\"" };
    }
    if (name.ends_with("-apple-darwin")) {
        return { "libc++ headers and std module sources; the C library headers are the installed macOS SDK's", "\"Apache-2.0 WITH LLVM-exception\"" };
    }
    return { "libc++ headers and std module sources with glibc's and the Linux kernel's headers",
             "\"Apache-2.0 WITH LLVM-exception\", \"LGPL-2.1-or-later\", \"GPL-2.0-only WITH Linux-syscall-note\"" };
}

base::Result<std::string> render_template(const std::string& root, std::string_view templateName, const std::map<std::string, std::string>& values) {
    const std::string templatePath { base::join_path(root, std::format("packaging/xlings/{}", templateName)) };
    auto text = os::fs::read(templatePath);
    if (!text) return base::fail("read-file", text.error());
    std::string rendered { *text };
    for (const auto& [placeholder, value] : values) rendered = base::replace_all(rendered, placeholder, value);
    if (auto leftover = leftover_placeholder(rendered)) return base::fail("xlings-artifacts", std::format("unrendered {} in {}", *leftover, templateName));
    return rendered;
}

// The `<os>-<arch>` the openkal tree runs on by itself (its mcpp-package.json `runs_on.self`).
base::Result<std::vector<std::string>> tree_runs_on(const std::string& archive, const std::string& scratch) {
    std::error_code ignored;
    std::filesystem::remove_all(scratch, ignored);
    auto extracted = pack::archive::extract(archive, scratch, [](std::string_view path) { return path == "mcpp-package.json"; });
    if (!extracted) return std::unexpected { extracted.error() };
    const auto package = nlohmann::json::parse(os::fs::read(base::join_path(scratch, "mcpp-package.json")).value_or("{}"), nullptr, false);
    std::filesystem::remove_all(scratch, ignored);
    std::vector<std::string> pairs;
    if (package.is_object()) {
        for (const auto& pair : package.value("runs_on", nlohmann::json::object()).value("self", nlohmann::json::array())) {
            if (pair.is_string()) pairs.push_back(pair.get<std::string>());
        }
    }
    if (pairs.empty()) return base::fail("xlings-artifacts", std::format("{} says it runs nowhere by itself (mcpp-package.json runs_on.self)", archive));
    return pairs;
}

} // namespace

base::Result<XlingsResult> make_xlings_artifacts(const std::string& root, const std::string& releaseDir, const std::string& outDir,
                                                  std::optional<std::string> version, std::optional<std::string> url,
                                                  std::optional<std::string> clangdVersion) {
    std::error_code ignored;
    if (std::error_code failed; !std::filesystem::create_directories(outDir, failed) && failed) {
        return base::fail("create-directory", std::format("cannot create {}: {}", outDir, failed.message()));
    }
    std::string productVersion { version.value_or("") };
    if (productVersion.empty()) {
        auto read = mcppls::devtools::version::manifest_version(root);
        if (!read) return std::unexpected { read.error() };
        productVersion = *read;
    }
    auto lockData = pack::lock::load(base::join_path(root, "packaging/payload.lock.json"));
    if (!lockData) return std::unexpected { lockData.error() };
    const std::string engine { clangdVersion.value_or(lockData->clangdVersion) };
    std::string prefix { url.value_or(std::format("https://github.com/Sunrisepeak/mcpp-language-server/releases/download/v{}", productVersion)) };
    while (prefix.ends_with('/')) prefix.pop_back();

    XlingsResult result {};
    // The kits: one descriptor each, one asset for every system.
    std::map<std::string, KitAsset> kits;
    for (const auto& path : base::children(releaseDir)) {
        auto kit = kit_asset(path);
        if (!kit) continue;
        auto digest = pack::fetch::digest_of(path);
        if (!digest) return std::unexpected { digest.error() };
        kit->sha256 = *digest;
        kits[kit->name] = *kit;
    }
    if (!kits.contains("openkal")) return base::fail("xlings-artifacts", std::format("{} holds no mcppls-kit-openkal-<version>.tar.gz", releaseDir));
    for (const auto& [platform, row] : lockData->platforms) {
        if (!row.kit.name.empty() && !kits.contains(row.kit.name)) {
            return base::fail("xlings-artifacts", std::format("{} holds no mcppls-kit-{}-<version>.tar.gz, the kit of {}", releaseDir, row.kit.name, platform));
        }
    }
    for (const auto& [name, kit] : kits) {
        const auto [what, licenses] = kit_description(name);
        auto rendered = render_template(root, "mcppls-kit.lua.in", {
            { "@KIT_NAME@", name }, { "@KIT_TARGET@", name }, { "@KIT_WHAT@", what }, { "@KIT_LICENSES@", licenses },
            { "@KIT_VERSION@", kit.version }, { "@KIT_URL@", std::format("{}/{}", prefix, kit.file) }, { "@KIT_SHA256@", kit.sha256 } });
        if (!rendered) return std::unexpected { rendered.error() };
        const std::string outPath { base::join_path(outDir, std::format("mcppls-kit-{}.lua", name)) };
        if (auto written = os::fs::write_atomic(outPath, *rendered); !written) return base::fail("write-file", written.error());
        std::println("rendered {} ({} {})", outPath, kit.file, kit.sha256);
        result.written.push_back(outPath);
    }

    // The server: the openkal tree, one asset; each system's block names the architectures it runs on by itself.
    const std::string treeFile { std::format("mcppls-{}-openkal.zip", productVersion) };
    const std::string tree { base::join_path(releaseDir, treeFile) };
    if (!std::filesystem::is_regular_file(tree, ignored)) return base::fail("xlings-artifacts", std::format("missing {}", tree));
    auto runsOn = tree_runs_on(tree, base::join_path(outDir, ".scratch-tree"));
    if (!runsOn) return std::unexpected { runsOn.error() };
    auto treeDigest = pack::fetch::digest_of(tree);
    if (!treeDigest) return std::unexpected { treeDigest.error() };
    std::set<std::string> archs;
    std::string systems;
    for (const auto& [system, key] : std::vector<std::pair<std::string_view, std::string_view>> { { "linux", "linux" }, { "macos", "macosx" }, { "windows", "windows" } }) {
        std::string shas;
        for (const auto& pair : *runsOn) {
            if (!pair.starts_with(std::format("{}-", system))) continue;
            const std::string arch { pair.substr(system.size() + 1) };
            archs.insert(arch);
            shas += std::format("{}{} = \"{}\"", shas.empty() ? "" : ", ", arch, *treeDigest);
        }
        if (shas.empty()) continue;
        systems += std::format("        {} = {{\n            [\"latest\"] = {{ ref = \"{}\" }},\n            [\"{}\"] = {{ url = \"{}/{}\", sha256 = {{ {} }} }},\n        }},\n",
                               key, productVersion, productVersion, prefix, treeFile, shas);
    }
    std::string archList;
    for (const auto& arch : archs) archList += std::format("{}\"{}\"", archList.empty() ? "" : ", ", arch);
    auto rendered = render_template(root, "mcpp-language-server.lua.in", {
        { "@ARCHS@", archList }, { "@XPM_SYSTEMS@", systems }, { "@CLANGD_VERSION@", std::string { base::llvm_base_version(engine) } },
        { "@OPENKAL_KIT_VERSION@", kits["openkal"].version } });
    if (!rendered) return std::unexpected { rendered.error() };
    const std::string outPath { base::join_path(outDir, "mcpp-language-server.lua") };
    if (auto written = os::fs::write_atomic(outPath, *rendered); !written) return base::fail("write-file", written.error());
    std::println("rendered {} ({} on {})", outPath, treeFile, base::join(*runsOn, ", "));
    result.written.push_back(outPath);
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
    const auto releaseDir = arguments.value("release");
    const auto out = arguments.value("out");
    if (!releaseDir || !out) {
        std::println(std::cerr, "mcppls-devtools: release xlings needs --release and --out");
        return 2;
    }
    auto result = release::make_xlings_artifacts(root, *releaseDir, *out, arguments.value("version"), arguments.value("url"), arguments.value("clangd-version"));
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
    (void) command.description("Release-time checks and artifacts: the staged manifest, the xim-pkgindex descriptors");
    (void) command.subcommand("check")
        .description("Check a staged release directory against packaging/release.manifest.json")
        .option("version").takes_value().value_name("VERSION").help("The release being staged").option("dir")
        .takes_value().value_name("DIR").help("The staged release directory").option("render")
        .help("Also write MANIFEST.md into the directory");
    (void) command.subcommand("xlings")
        .description("Render the xim-pkgindex descriptors of a release: the server's one asset and each kit's")
        .option("release").takes_value().value_name("DIR").help("A release's directory: mcppls-<version>-openkal.zip and mcppls-kit-*.tar.gz").option("out")
        .takes_value().value_name("DIR").help("Where to write the rendered descriptors").option("version")
        .takes_value().value_name("VERSION").help("Default: mcpp.toml").option("url")
        .takes_value().value_name("PREFIX").help("Where the assets are published (default: the GitHub release of the version)").option("clangd-version")
        .takes_value().value_name("VERSION").help("Default: packaging/payload.lock.json's clangd-version");
    (void) command.action([&handled, &status](const cmdline::ParsedArgs& arguments) {
        const auto sub = arguments.subcommand();
        static const cmdline::ParsedArgs EMPTY {};
        status = dispatch(handled, status, arguments.subcommand_name(), sub ? sub->get() : EMPTY);
    });
    return command;
}

} // namespace mcppls::devtools
