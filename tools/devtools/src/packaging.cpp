module mcppls.devtools.packaging;

import std;
import nlohmann.json;
import mcpplibs.cmdline;
import mcppls.os;
import mcppls.base.error;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.pack.clangd;
import mcppls.pack.kit;
import mcppls.pack.fetch;
import mcppls.pack.lock;
import mcppls.pack.payload;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.devtools.common;
import mcppls.devtools.editors;

namespace mcppls::devtools {
namespace {

namespace platform = mcppls::platform;
namespace cmdline = mcpplibs::cmdline;
namespace pack = mcppls::pack;

// The openkal `--target` of the server a payload for `platformName` carries -- the same three
// triples common.cpp's host_triple() names for this host.
std::string_view triple_for_platform(std::string_view platformName) {
    if (platformName == "win32-x64") return "x86_64-windows-gnu";
    if (platformName == "darwin-arm64") return "aarch64-macos";
    return "x86_64-linux-gnu";
}

// The server a payload carries: the one given, or the one `mcpp build` makes for `platformName` --
// this host's own build when the platform is this host's, the openkal cross target otherwise.
std::optional<std::string> server_for(const std::string& root, const cmdline::ParsedArgs& arguments,
                                      std::string_view platformName) {
    if (const auto given = arguments.value("server"); given && !given->empty()) {
        if (platform::fs::is_regular_file(*given)) return *given;
        std::println(std::cerr, "mcppls-devtools: no server at {}", *given);
        return std::nullopt;
    }
    ServerBuild build {};
    if (arguments.is_flag_set("dev")) build.profile = "dev";
    if (platformName != mcppls::os::PLATFORM) build.target = std::string { triple_for_platform(platformName) };
    auto located = locate_server(root, build);
    if (!located) {
        std::println(std::cerr, "mcppls-devtools: {}", located.error().message);
        return std::nullopt;
    }
    return *located;
}

std::optional<std::string> parse_platform(const cmdline::ParsedArgs& arguments) {
    const std::string given { arguments.value("platform").value_or(std::string { mcppls::os::PLATFORM }) };
    if (std::ranges::any_of(pack::payload::PLATFORMS, [&](std::string_view p) { return p == given; })) return given;
    std::println(std::cerr, "mcppls-devtools: unknown platform {} — linux-x64, win32-x64, or darwin-arm64", given);
    return std::nullopt;
}

struct Payload {
    std::string directory;
    bool built { false };
};

// Fetch -> trim clangd -> build the semantic kit -> assemble, in that order (tooling architecture
// §4), all in this process. The kit step runs cmake and ninja over the libc++ sources; nothing
// here runs an interpreter.
Payload assemble_payload(const std::string& root, std::string_view platformName, const std::string& outDirectory,
                         const std::string& clangdDirectory, const std::string& kitDirectory,
                         const std::string& serverPath, const std::string& cacheDirectory,
                         std::optional<std::string> serverVersion, bool strip) {
    const std::string work { base::join_path(root, "target/pack") };
    (void) platform::fs::create_directories(work);

    auto lockData = pack::lock::load(base::join_path(root, "packaging/payload.lock.json"));
    if (!lockData) {
        std::println(std::cerr, "mcppls-devtools: {}", lockData.error().message);
        return {};
    }

    std::string clangd { clangdDirectory };
    if (clangd.empty()) {
        clangd = base::join_path(work, "clangd");
        std::println("  trimming clangd for {} ...", platformName);
        auto trimmed = pack::clangd::trim(pack::clangd::Options {
            .platform = std::string { platformName },
            .outDirectory = clangd,
            .cacheDirectory = cacheDirectory,
            .strip = strip,
        }, *lockData);
        if (!trimmed) {
            std::println(std::cerr, "  trimming clangd failed: {}", trimmed.error().message);
            return {};
        }
        std::println("  clangd: {} files, lib/clang/{}/include, {:.1f} MB", trimmed->filesKept, trimmed->clangMajor,
                     static_cast<double>(trimmed->totalBytes) / 1e6);
        if (trimmed->versionLine) std::println("  {}", *trimmed->versionLine);
    }

    std::string kit { kitDirectory };
    if (kit.empty()) {
        kit = base::join_path(work, "kit");
        std::println("  the semantic kit is compiled from libc++ sources; the first time takes a while");
        auto built = pack::kit::build(pack::kit::Options {
            .platform = std::string { platformName },
            .outDir = kit,
            .cacheDir = cacheDirectory,
            .lockPath = base::join_path(root, "packaging/payload.lock.json"),
            .workDir = base::join_path(work, "kit-build"),
        });
        if (!built) {
            std::println(std::cerr, "  building the semantic kit failed: {}", built.error().message);
            return {};
        }
        std::println("  kit {}: {} files, {:.1f} MB", built->name, built->fileCount, static_cast<double>(built->totalBytes) / 1e6);
    }

    auto assembled = pack::payload::assemble(pack::payload::AssembleOptions {
        .platform = std::string { platformName },
        .serverPath = serverPath,
        .clangdDirectory = clangd,
        .kitDirectory = kit,
        .outDirectory = outDirectory,
        .repositoryRoot = root,
        .serverVersion = serverVersion,
    }, *lockData);
    if (!assembled) {
        std::println(std::cerr, "  assembling the payload failed: {}", assembled.error().message);
        return {};
    }
    return Payload { *assembled, true };
}

void print_verify_result(std::string_view directory, const std::vector<std::string>& problems, bool wantJson) {
    if (wantJson) {
        nlohmann::json body;
        body["directory"] = std::string { directory };
        body["ok"] = problems.empty();
        body["problems"] = problems;
        // A digest of the server binary, for a caller (CI, replacing its own inline hashlib check)
        // that wants to compare it against a cross-built one without reading the file itself.
        if (auto digest = pack::fetch::digest_of(base::join_path(directory, "bin/mcppls")); digest) {
            body["server"] = { { "path", "bin/mcppls" }, { "sha256", *digest } };
        } else if (auto digestExe = pack::fetch::digest_of(base::join_path(directory, "bin/mcppls.exe")); digestExe) {
            body["server"] = { { "path", "bin/mcppls.exe" }, { "sha256", *digestExe } };
        }
        print_json(body);
        return;
    }
    if (!problems.empty()) {
        for (const auto& problem : problems) std::println(std::cerr, "mcppls-devtools: problem: {}", problem);
        return;
    }
    std::println("payload verified: {}", directory);
}

// ---- the editors -----------------------------------------------------------------------------
// Each one answers the same three questions: how is it built, how is it installed, how is it
// removed. VS Code has a command line for all three. Zed and CLion do not, so the install is what
// their own UI would leave on disk (mcppls.devtools.editors says exactly what), and for Zed the
// palette stays the recommended way: this tool prepares everything and names the one action left.

enum class Editor { vscode, zed, clion };

std::string_view editor_name(Editor editor) {
    switch (editor) {
    case Editor::zed: return "zed";
    case Editor::clion: return "clion";
    default: return "vscode";
    }
}

std::optional<Editor> parse_editor(std::string_view name) {
    if (name == "vscode" || name == "code") return Editor::vscode;
    if (name == "zed") return Editor::zed;
    if (name == "clion" || name == "intellij") return Editor::clion;
    return std::nullopt;
}

// The Zed extension is a Rust crate compiled to WebAssembly; the directory it lives in is what
// Zed loads. Zed's own CLI has no extension command (`zed --help` lists none), so installing is
// the editor's "install dev extension" action pointed at this directory, and removing it is its
// extension manager. Saying so is better than writing into Zed's extension index behind it.
bool build_zed(const std::string& root) {
    const std::string directory { base::join_path(root, "editors/zed") };
    auto cargo = tool("cargo", "a Zed extension is a Rust crate compiled to WebAssembly");
    if (!cargo) return false;
    // wasip2, not wasip1, and this is the whole reason the extension did not load. Zed parses the
    // file with a COMPONENT parser; `wasm32-wasip1` emits a core module, so Zed answered
    // "attempted to parse a wasm module with a component parser" and loaded nothing. The wasip2
    // target emits a component directly (header `00 61 73 6d 0d 00 01 00` rather than
    // `... 01 00 00 00`), so no adapter and no wasm-tools are needed — measured 2026-09-17.
    //
    // The target is part of the toolchain, not of the crate, so it is added here rather than left
    // as a sentence for the reader to act on. Already-present is a no-op.
    if (auto rustup = platform::env::find_executable("rustup")) {
        (void) step("checking the wasm32-wasip2 target", *rustup, { "target", "add", "wasm32-wasip2" }, directory);
    }
    if (!step("building the Zed extension", *cargo,
              { "build", "--release", "--target", "wasm32-wasip2" }, directory)) {
        std::println(std::cerr, "  If the target is missing: rustup target add wasm32-wasip2");
        return false;
    }
    const std::string wasm { base::join_path(directory, "target/wasm32-wasip2/release/zed_mcppls.wasm") };
    if (!platform::fs::is_regular_file(wasm)) {
        std::println(std::cerr, "  the build produced no {}", wasm);
        return false;
    }
    // Zed loads `extension.wasm` from the extension directory.
    const std::string installed { base::join_path(directory, "extension.wasm") };
    if (auto text = platform::fs::read_file(wasm)) {
        if (auto written = platform::fs::write_file_atomic(installed, *text); !written) {
            std::println(std::cerr, "  cannot write {}: {}", installed, written.error().message);
            return false;
        }
    }
    std::println("zed extension: {}", directory);
    return true;
}

// `--only clangd`: the trim alone, for CI's cache step (the payload job caches
// packaging/payload.lock.json's downloads and re-trims from cache every run; assembling a whole
// payload just to prime that cache would also need a server and a kit it does not have yet).
int command_payload_only_clangd(const std::string& root, std::string_view platformName,
                                const cmdline::ParsedArgs& arguments, bool wantJson) {
    const std::string out { arguments.value("out").value_or(base::join_path(root, "target/pack/clangd")) };
    const std::string cache { arguments.value("cache").value_or(base::join_path(root, ".payload-cache")) };
    const bool strip { !arguments.is_flag_set("no-strip") };

    auto lockData = pack::lock::load(base::join_path(root, "packaging/payload.lock.json"));
    if (!lockData) {
        std::println(std::cerr, "mcppls-devtools: {}", lockData.error().message);
        return 1;
    }
    std::println("mcppls-devtools: trimming clangd for {}", platformName);
    auto trimmed = pack::clangd::trim(pack::clangd::Options {
        .platform = std::string { platformName }, .outDirectory = out, .cacheDirectory = cache, .strip = strip,
    }, *lockData);
    if (!trimmed) {
        std::println(std::cerr, "mcppls-devtools: {}", trimmed.error().message);
        return 1;
    }
    if (wantJson) {
        nlohmann::json body;
        body["directory"] = trimmed->directory;
        body["binary"] = trimmed->binary;
        body["clangMajor"] = trimmed->clangMajor;
        body["filesKept"] = trimmed->filesKept;
        body["totalBytes"] = trimmed->totalBytes;
        body["stripped"] = trimmed->stripped;
        if (trimmed->versionLine) body["versionLine"] = *trimmed->versionLine;
        print_json(body);
    } else {
        std::println("clangd: {} files, lib/clang/{}/include, {:.1f} MB -> {}", trimmed->filesKept, trimmed->clangMajor,
                     static_cast<double>(trimmed->totalBytes) / 1e6, trimmed->directory);
        if (trimmed->versionLine) std::println("  {}", *trimmed->versionLine);
    }
    return 0;
}

int command_payload(const cmdline::ParsedArgs& arguments) {
    const std::string root { repository_root() };
    const bool wantJson { arguments.is_flag_set("json") };

    // Verify only -- nothing is built or fetched.
    if (auto verifyDirectory = arguments.value("verify"); verifyDirectory && !verifyDirectory->empty()) {
        auto problems = pack::payload::verify(*verifyDirectory);
        print_verify_result(*verifyDirectory, problems, wantJson);
        return problems.empty() ? 0 : 1;
    }

    auto platformName = parse_platform(arguments);
    if (!platformName) return 2;

    if (auto only = arguments.value("only"); only && !only->empty()) {
        if (*only != "clangd") {
            std::println(std::cerr, "mcppls-devtools: --only accepts clangd (the other parts have no standalone step here)");
            return 2;
        }
        return command_payload_only_clangd(root, *platformName, arguments, wantJson);
    }

    const std::string out { arguments.value("out").value_or(base::join_path(root, "target/pack/payload")) };

    // Copy an already-assembled payload rather than building one (assemble_payload.py's --from):
    // a caller (CI's extension-packaging job) that already produced one puts it where the next
    // step wants it, keeping the executable bits.
    if (auto from = arguments.value("from"); from && !from->empty()) {
        auto copied = pack::payload::copy_from(*from, out);
        if (!copied) {
            if (wantJson) {
                nlohmann::json body; body["ok"] = false; body["error"] = copied.error().message; print_json(body);
            } else {
                std::println(std::cerr, "mcppls-devtools: {}", copied.error().message);
            }
            return 1;
        }
        print_verify_result(*copied, {}, wantJson);
        return 0;
    }

    auto server = server_for(root, arguments, *platformName);
    if (!server) return 2;
    std::println("mcppls-devtools: payload for {} from {}", *platformName, *server);
    const std::string cache { arguments.value("cache").value_or(base::join_path(root, ".payload-cache")) };
    const bool strip { !arguments.is_flag_set("no-strip") };
    const auto payload = assemble_payload(root, *platformName, out, arguments.value("clangd").value_or(""),
                                          arguments.value("kit").value_or(""), *server, cache,
                                          arguments.value("server-version"), strip);
    if (!payload.built) return 1;

    auto problems = pack::payload::verify(payload.directory);
    print_verify_result(payload.directory, problems, wantJson);
    if (!wantJson && problems.empty()) std::println("payload: {}", payload.directory);
    return problems.empty() ? 0 : 1;
}

// The CLion plugin is a Gradle project: Kotlin against the IntelliJ platform's LSP API, which the
// paid IDEs have and CLion is one of. Gradle downloads the IDE it compiles against on first build,
// so the first one is long and needs the network. Returns the plugin archive.
std::optional<std::string> build_clion(const std::string& root) {
    const std::string directory { base::join_path(root, "editors/clion") };
    // `xim:gradle` declares the JDK it runs on, so the manifest naming the one package is what
    // gets both — which is why nothing here asks for a JDK.
    auto gradle = tool("gradle", "an IntelliJ-platform plugin is a Gradle project");
    if (!gradle) return std::nullopt;
    std::println("  the first build downloads the CLion SDK; it takes a while and needs the network");
    if (!step("building the CLion plugin", *gradle, { "buildPlugin", "--console=plain" }, directory)) return std::nullopt;

    const std::string distributions { base::join_path(directory, "build/distributions") };
    std::string zip;
    for (const auto& entry : platform::fs::list_directory(distributions)) {
        if (entry.ends_with(".zip")) zip = base::join_path(distributions, entry);
    }
    if (zip.empty()) {
        std::println(std::cerr, "  the build produced no plugin archive under {}", distributions);
        return std::nullopt;
    }
    std::println("clion plugin: {}", zip);
    return zip;
}

// A payload at `directory`: the one `--payload` names, copied (keeping the executable bits, and
// verified), or one assembled from the server this checkout builds. Every editor needs one — VS
// Code inside its VSIX, Zed and CLion at editors::server_directory() — and a caller that already
// has one (CI, which built the three platforms' payloads in an earlier job) passes it rather than
// making another. Such a caller has no server beside this tool and needs none: the payload it hands
// over already contains the built server.
int put_payload(const std::string& root, const cmdline::ParsedArgs& arguments, const std::string& directory) {
    if (const auto given = arguments.value("payload"); given && !given->empty()) {
        if (!platform::fs::is_regular_file(base::join_path(*given, "payload.json"))) {
            std::println(std::cerr, "mcppls-devtools: {} is not a payload (no payload.json)", *given);
            return 2;
        }
        if (base::normalize_path(*given) == base::normalize_path(directory)) {
            std::println("  using the payload already at {}", directory);
            return 0;
        }
        std::println("  putting the payload at {} ...", directory);
        auto copied = pack::payload::copy_from(*given, directory);
        if (!copied) {
            std::println(std::cerr, "  {}", copied.error().message);
            return 1;
        }
        return 0;
    }
    auto server = server_for(root, arguments, mcppls::os::PLATFORM);
    if (!server) return 2;
    const auto payload = assemble_payload(root, mcppls::os::PLATFORM, directory,
                                          arguments.value("clangd").value_or(""), arguments.value("kit").value_or(""),
                                          *server, base::join_path(root, ".payload-cache"), std::nullopt, true);
    return payload.built ? 0 : 1;
}

int build_vscode(const std::string& root, const cmdline::ParsedArgs& arguments, std::string& vsixOut) {
    const std::string extension { base::join_path(root, "editors/vscode") };
    // The extension carries its payload inside itself.
    if (const int status = put_payload(root, arguments, base::join_path(extension, "payload")); status != 0) return status;

    auto npm = tool("npm", "the VS Code extension is TypeScript");
    if (!npm) return 1;
    const bool haveModules { platform::fs::is_directory(base::join_path(extension, "node_modules")) };
    if (!step(haveModules ? "refreshing the extension's dependencies" : "installing the extension's dependencies",
              *npm, { haveModules ? "install" : "ci", "--no-audit", "--no-fund" }, extension)) {
        return 1;
    }
    if (!step("compiling the extension", *npm, { "run", "compile" }, extension)) return 1;

    const std::string vsix { arguments.value("out").value_or(
        base::join_path(root, std::format("target/pack/mcppls-{}.vsix", mcppls::os::PLATFORM))) };
    (void) platform::fs::create_directories(base::parent_path(vsix));
    auto npx = tool("npx", "packaging a VSIX runs vsce from the extension's own dependencies");
    if (!npx) return 1;
    if (!step("packaging the VSIX", *npx,
              { "--no-install", "vsce", "package", "--target", std::string { mcppls::os::PLATFORM }, "--out", vsix },
              extension)) {
        return 1;
    }
    std::println("vscode extension: {}", vsix);
    vsixOut = vsix;
    return 0;
}

int install_vscode(const std::string& root, const std::string& vsix) {
    auto code = tool("code", "installing a VSIX from a command line is the editor's own");
    if (!code) return 1;
    if (!step("installing into VS Code", *code, { "--install-extension", vsix, "--force" }, root)) return 1;
    std::println("Installed. Restart VS Code, then open a C++ project.");
    return 0;
}

// Where Zed and CLion find the server when PATH has none, overridable for tests.
std::string server_directory(const cmdline::ParsedArgs& arguments) {
    return arguments.value("server-dir").value_or(editors::server_directory());
}

std::string zed_directory(const cmdline::ParsedArgs& arguments) {
    return arguments.value("zed-dir").value_or(editors::zed_directory());
}

// `--clion-dir` names one plugins directory; otherwise every CLion that has run on this machine.
std::vector<std::string> clion_directories(const cmdline::ParsedArgs& arguments) {
    if (auto given = arguments.value("clion-dir"); given && !given->empty()) return { *given };
    return editors::clion_plugin_directories();
}

int install_server(const std::string& root, const cmdline::ParsedArgs& arguments) {
    const std::string directory { server_directory(arguments) };
    if (const int status = put_payload(root, arguments, directory); status != 0) return status;
    std::println("server: {}", base::join_path(directory, std::format("bin/mcppls{}", mcppls::os::EXECUTABLE_SUFFIX)));
    return 0;
}

// The recommended Zed install is its own palette action, so by default this prepares everything
// that action does not (the extension built, the server in place) and names the one step left.
// `--link` takes that step too: it is the same link the palette makes.
int install_zed(const std::string& root, const cmdline::ParsedArgs& arguments, const std::string& source) {
    if (const int status = install_server(root, arguments); status != 0) return status;
    if (!arguments.is_flag_set("link")) {
        std::println("");
        std::println("Last step, in Zed: command palette → \"zed: install dev extension\" → {}", source);
        std::println("  (or let this tool do it: add --link)");
        return 0;
    }
    const std::string zed { zed_directory(arguments) };
    auto installed = editors::install_zed(zed, source);
    if (!installed) {
        std::println(std::cerr, "mcppls-devtools: {}", installed.error().message);
        return 1;
    }
    std::println("Installed into Zed ({}, {}). A running Zed picks it up by itself.",
                 *installed == editors::ZedInstall::linked ? "a dev extension" : "a copy",
                 base::join_path(zed, std::format("extensions/installed/{}", editors::ZED_EXTENSION_ID)));
    return 0;
}

int install_clion(const std::string& root, const cmdline::ParsedArgs& arguments, const std::string& archive) {
    const auto directories = clion_directories(arguments);
    if (directories.empty()) {
        std::println(std::cerr, "mcppls-devtools: no CLion here (nothing under JetBrains' data directory); "
                                "start CLion once, or name its plugins directory with --clion-dir");
        return 1;
    }
    if (const int status = install_server(root, arguments); status != 0) return status;
    for (const auto& directory : directories) {
        if (auto installed = editors::install_clion(directory, archive); !installed) {
            std::println(std::cerr, "mcppls-devtools: {}", installed.error().message);
            return 1;
        }
        std::println("Installed into {}", directory);
    }
    std::println("Restart CLion to load it.");
    return 0;
}

// `--plugin`: an already-built Zed extension directory or CLion archive — a release asset, say —
// installed as it is, without building one here.
std::optional<std::string> given_plugin(const cmdline::ParsedArgs& arguments) {
    if (auto given = arguments.value("plugin"); given && !given->empty()) return *given;
    return std::nullopt;
}

int command_extension(const cmdline::ParsedArgs& arguments) {
    const std::string root { repository_root() };
    const std::string wanted { arguments.value("editor").value_or("all") };
    const bool install { arguments.is_flag_set("install") };

    std::vector<Editor> editors;
    if (wanted == "all") {
        editors = { Editor::vscode, Editor::zed, Editor::clion };
    } else if (auto one = parse_editor(wanted)) {
        editors = { *one };
    } else {
        std::println(std::cerr, "mcppls-devtools: unknown editor {} — vscode, zed, clion, or all", wanted);
        return 2;
    }
    if (install && editors.size() > 1) {
        std::println(std::cerr, "mcppls-devtools: --install wants one editor, so say which: --editor vscode|zed|clion");
        return 2;
    }
    const auto plugin = given_plugin(arguments);
    if (plugin && editors.size() > 1) {
        std::println(std::cerr, "mcppls-devtools: --plugin is one editor's plugin, so say which: --editor zed|clion");
        return 2;
    }

    for (const Editor editor : editors) {
        std::println("mcppls-devtools: {} extension for {}", editor_name(editor), mcppls::os::PLATFORM);
        if (editor == Editor::zed) {
            const std::string source { plugin.value_or(base::join_path(root, "editors/zed")) };
            if (!plugin && !build_zed(root)) return 1;
            if (install) {
                if (const int status = install_zed(root, arguments, source); status != 0) return status;
            } else {
                std::println("  Install it with: mcpp run -p devtools -- extension --editor zed --install");
            }
            continue;
        }
        if (editor == Editor::clion) {
            auto archive = plugin ? plugin : build_clion(root);
            if (!archive) return 1;
            if (install) {
                if (const int status = install_clion(root, arguments, *archive); status != 0) return status;
            } else {
                std::println("  Install it with: mcpp run --features clion -p devtools -- extension --editor clion --install");
            }
            continue;
        }
        std::string vsix;
        if (const int status = build_vscode(root, arguments, vsix); status != 0) return status;
        if (install) {
            if (const int status = install_vscode(root, vsix); status != 0) return status;
        } else {
            std::println("  Install it with: mcpp run -p devtools -- extension --editor vscode --install");
        }
    }
    return 0;
}

int command_uninstall(const cmdline::ParsedArgs& arguments) {
    const std::string root { repository_root() };
    const std::string wanted { arguments.value("editor").value_or("all") };
    std::vector<Editor> editors;
    if (wanted == "all") editors = { Editor::vscode, Editor::zed, Editor::clion };
    else if (auto one = parse_editor(wanted)) editors = { *one };
    else {
        std::println(std::cerr, "mcppls-devtools: unknown editor {} — vscode, zed, clion, or all", wanted);
        return 2;
    }

    int failures { 0 };
    for (const Editor editor : editors) {
        if (editor == Editor::zed) {
            const std::string zed { zed_directory(arguments) };
            std::println("zed: {}", editors::remove_zed(zed) ? "removed" : "was not installed");
            continue;
        }
        if (editor == Editor::clion) {
            bool any { false };
            for (const auto& directory : clion_directories(arguments)) {
                if (editors::remove_clion(directory)) {
                    any = true;
                    std::println("clion: removed from {} (restart CLion)", directory);
                }
            }
            if (!any) std::println("clion: was not installed");
            continue;
        }
        auto code = platform::env::find_executable("code");
        if (!code) {
            std::println("vscode: the `code` command is not on PATH; remove it from the Extensions view.");
            continue;
        }
        if (!step("removing from VS Code", *code, { "--uninstall-extension", "sunrisepeak.mcpp-language-server" }, root)) {
            ++failures;
        }
    }

    // The server Zed and CLion share goes with the last of them.
    const bool zedLeft { editors::zed_installed(zed_directory(arguments)) };
    const bool clionLeft { std::ranges::any_of(clion_directories(arguments), editors::clion_installed) };
    const std::string server { server_directory(arguments) };
    if (!zedLeft && !clionLeft && platform::fs::exists(server)) {
        platform::fs::remove_all(server);
        std::println("server: removed {}", server);
    }
    return failures == 0 ? 0 : 1;
}

} // namespace

cmdline::App payload_command(bool& handled, int& status) {
    cmdline::App command { "payload" };
    (void) command.description(
        "Assemble, verify or copy a payload: the server, a trimmed clangd and the semantic kit.\n"
        "  payload --platform win32-x64 --server cross/mcppls.exe --kit work/kit --out payload\n"
        "  payload --verify payload                      check an already-assembled payload\n"
        "  payload --from payload --out editors/vscode/payload   copy one, keeping exec bits\n"
        "  payload --only clangd --out work/clangd        trim clangd alone (CI's cache step)");
    (void) command.option("platform").takes_value().help("linux-x64 | win32-x64 | darwin-arm64 (default: this host)");
    (void) command.option("out").takes_value().help("Where to put it (default target/pack/payload)");
    (void) command.option("server").takes_value().help("The server to package (default: `mcpp build --profile release` for --platform)");
    (void) command.option("server-version").takes_value().help("Defaults to this build's own version");
    (void) command.option("dev").help("Package a dev-profile server rather than a release one");
    (void) command.option("clangd").takes_value().help("Use this already-trimmed clangd directory");
    (void) command.option("kit").takes_value().help("Use this already-built semantic kit directory");
    (void) command.option("cache").takes_value().help("Where fetched inputs are cached (default <root>/.payload-cache)");
    (void) command.option("no-strip").help("Keep clangd's debug symbols instead of stripping it");
    (void) command.option("only").takes_value().help("clangd: trim clangd alone and stop there");
    (void) command.option("verify").takes_value().value_name("DIR").help("Only check an already-assembled payload");
    (void) command.option("from").takes_value().value_name("DIR").help("Copy an already-assembled payload to --out instead of building one");
    (void) command.option("json").help("Machine-readable result on standard output");
    (void) command.action([&handled, &status](const cmdline::ParsedArgs& arguments) { handled = true; status = command_payload(arguments); });
    return command;
}

cmdline::App extension_command(bool& handled, int& status) {
    cmdline::App command { "extension" };
    (void) command.description("Build an editor extension: VS Code (VSIX), Zed (WebAssembly) or CLion (Gradle)");
    (void) command.option("editor").takes_value().help("vscode | zed | clion | all (default all)");
    (void) command.option("payload").takes_value().help("Use this payload instead of assembling one (CI)");
    (void) command.option("install").help("Also install it (Zed: everything but its palette action unless --link)");
    (void) command.option("out").takes_value().help("Where to write the .vsix");
    (void) command.option("server").takes_value().help("The server to package (default: `mcpp build --profile release` for this host)");
    (void) command.option("dev").help("Package a dev-profile server rather than a release one");
    (void) command.option("clangd").takes_value().help("Use this already-trimmed clangd directory");
    (void) command.option("kit").takes_value().help("Use this already-built semantic kit directory");
    (void) command.option("plugin").takes_value().help("Zed: an extension directory, CLion: a plugin archive, to install instead of building one");
    (void) command.option("link").help("Zed: link the extension into Zed here rather than naming its palette action");
    (void) command.option("server-dir").takes_value().help("Where Zed and CLion's server goes (default <user data>/mcppls/payload)");
    (void) command.option("zed-dir").takes_value().help("Zed's data directory (default: this user's)");
    (void) command.option("clion-dir").takes_value().help("A CLion plugins directory (default: every CLion found)");
    (void) command.action([&handled, &status](const cmdline::ParsedArgs& arguments) { handled = true; status = command_extension(arguments); });
    return command;
}

cmdline::App uninstall_command(bool& handled, int& status) {
    cmdline::App command { "uninstall" };
    (void) command.description("Remove an installed extension, and Zed and CLion's server with the last of them");
    (void) command.option("editor").takes_value().help("vscode | zed | clion | all (default all)");
    (void) command.option("server-dir").takes_value().help("Where Zed and CLion's server is (default <user data>/mcppls/payload)");
    (void) command.option("zed-dir").takes_value().help("Zed's data directory (default: this user's)");
    (void) command.option("clion-dir").takes_value().help("A CLion plugins directory (default: every CLion found)");
    (void) command.action([&handled, &status](const cmdline::ParsedArgs& arguments) { handled = true; status = command_uninstall(arguments); });
    return command;
}

} // namespace mcppls::devtools
