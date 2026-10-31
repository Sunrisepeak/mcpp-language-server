module mcppls.devtools.packaging;

import std;
import nlohmann.json;
import mcpplibs.cmdline;
import mcppls.base.error;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.pack.clangd;
import mcppls.pack.kit;
import mcppls.pack.fetch;
import mcppls.pack.lock;
import mcppls.pack.payload;
import mcppls.devtools.kit;
import mcppls.base.version;
import mcppls.devtools.common;
import mcppls.devtools.editors;
import mcpplibs.os;
import mcppls.base.payload;
import mcppls.toolrun;

namespace os = mcpplibs::os;

namespace mcppls::devtools {
namespace {
namespace cmdline = mcpplibs::cmdline;
namespace pack = mcppls::pack;

// packaging/payload.lock.json, the one table of platforms; nullopt (said on stderr) when it is
// missing or does not parse.
std::optional<pack::lock::Lock> load_lock(const std::string& root) {
    auto loaded = pack::lock::load(base::join_path(root, "packaging/payload.lock.json"));
    if (!loaded) {
        std::println(std::cerr, "mcppls-devtools: {}", loaded.error().message);
        return std::nullopt;
    }
    return std::move(*loaded);
}

// What one payload is made of (ecosystem design 2026-10-29 §9): the server's okb and the tree that carries the
// runner of every system, the engine of every platform and the openkal kit. Each is given, or made here.
struct PayloadInputs {
    std::string serverImage;                    // --server-image: the okb
    std::string serverTree;                     // --server-tree: the tree, a directory or its archive
    std::string kit;                            // --kit: the openkal kit; empty: built from mcpp's build database
    std::map<std::string, std::string> clangd;  // --clangd <platform>=DIR, or openkal=DIR for the ok image: an engine already trimmed (a candidate's)
    std::vector<std::string> platforms;         // --platforms: which of the lock's; empty: every one
    std::string cache;
    std::string profile { "release" };
    std::optional<std::string> serverVersion;
    bool strip { true };
    bool candidate { false };
};

std::vector<std::string> list_option(const cmdline::ParsedArgs& arguments, std::string_view name) {
    std::vector<std::string> values;
    if (auto option = arguments.option(name)) {
        for (const auto& value : option->get().values) {
            for (const auto part : base::split(value, ',')) {
                if (!part.empty()) values.emplace_back(base::trim(part));
            }
        }
    }
    return values;
}

std::optional<PayloadInputs> payload_inputs(const std::string& root, const cmdline::ParsedArgs& arguments) {
    PayloadInputs inputs {
        .serverImage = arguments.value("server-image").value_or(""),
        .serverTree = arguments.value("server-tree").value_or(""),
        .kit = arguments.value("kit").value_or(""),
        .platforms = list_option(arguments, "platforms"),
        .cache = arguments.value("cache").value_or(base::join_path(root, ".payload-cache")),
        .profile = arguments.is_flag_set("dev") ? "dev" : "release",
        .serverVersion = arguments.value("server-version"),
        .strip = !arguments.is_flag_set("no-strip"),
        .candidate = arguments.is_flag_set("candidate"),
    };
    if (inputs.serverImage.empty() != inputs.serverTree.empty()) {
        std::println(std::cerr, "mcppls-devtools: --server-image and --server-tree go together: the okb, and the tree whose runners start it");
        return std::nullopt;
    }
    for (const auto& item : list_option(arguments, "clangd")) {
        const auto equals = item.find('=');
        if (equals == std::string::npos) {
            std::println(std::cerr, "mcppls-devtools: --clangd takes <platform>=<directory>, got {}", item);
            return std::nullopt;
        }
        inputs.clangd[item.substr(0, equals)] = item.substr(equals + 1);
    }
    const auto lockData = load_lock(root);
    if (!lockData) return std::nullopt;
    const auto known = pack::lock::platform_names(*lockData);
    if (inputs.platforms.empty()) inputs.platforms = known;
    for (const auto& platform : inputs.platforms) {
        if (std::ranges::find(known, platform) != known.end()) continue;
        std::println(std::cerr, "mcppls-devtools: unknown platform {} — packaging/payload.lock.json has {}", platform, base::join(known, ", "));
        return std::nullopt;
    }
    return inputs;
}

struct Payload {
    std::string directory;
    bool built { false };
};

// The engine of one platform, in the form its lock row selects: the one given for that form (`--clangd
// <platform>=DIR` for a native engine, `--clangd openkal=DIR` for the ok image), else the lock's, trimmed.
// Platforms whose rows name the same entry (the ok image) share one trimmed directory, `trimmed` keyed by
// the entry.
std::optional<std::string> engine_of(const std::string& root, const PayloadInputs& inputs, const pack::lock::Lock& lockData, const std::string& platform,
                                     std::map<std::string, std::string>& trimmed) {
    const std::string entry { pack::lock::platform(lockData, platform)->clangd };
    const std::string form { entry == pack::lock::OK_ENGINE_ENTRY ? std::string { "openkal" } : platform };
    if (const auto given = inputs.clangd.find(form); given != inputs.clangd.end()) return given->second;
    if (const auto done = trimmed.find(entry); done != trimmed.end()) return done->second;
    const std::string directory { base::join_path(root, std::format("target/pack/clangd/{}", form)) };
    std::println("  the engine of {} ...", platform);
    auto part = pack::clangd::trim(pack::clangd::Options {
        .platform = platform,
        .outDirectory = directory,
        .cacheDirectory = inputs.cache,
        .strip = inputs.strip,
    }, lockData);
    if (!part) {
        std::println(std::cerr, "  the engine of {} failed: {}", platform, part.error().message);
        return std::nullopt;
    }
    std::println("  clangd ({}): {} files, lib/clang/{}/include, {:.1f} MB", platform, part->filesKept, part->clangMajor,
                 static_cast<double>(part->totalBytes) / 1e6);
    trimmed[entry] = directory;
    return directory;
}

// The server, the engines, the kit -> assemble, in that order (tooling architecture §4), all in this process.
Payload assemble_payload(const std::string& root, const PayloadInputs& inputs, const std::string& outDirectory) {
    std::error_code ignored;
    (void)std::filesystem::create_directories(base::join_path(root, "target/pack"), ignored);
    auto lockData = pack::lock::load(base::join_path(root, "packaging/payload.lock.json"));
    if (!lockData) {
        std::println(std::cerr, "mcppls-devtools: {}", lockData.error().message);
        return {};
    }
    std::string image { inputs.serverImage };
    std::string tree;
    if (image.empty()) {
        auto packed = pack_server(root, inputs.profile);
        if (!packed) {
            std::println(std::cerr, "mcppls-devtools: {}", packed.error().message);
            return {};
        }
        image = packed->image;
        tree = packed->tree;
    } else {
        auto unpacked = server_tree(inputs.serverTree, base::join_path(root, "target/pack/server-tree"));
        if (!unpacked) {
            std::println(std::cerr, "mcppls-devtools: {}", unpacked.error().message);
            return {};
        }
        tree = *unpacked;
    }

    std::vector<pack::payload::PlatformEngine> engines;
    std::map<std::string, std::string> trimmed;
    for (const auto& platform : inputs.platforms) {
        auto engine = engine_of(root, inputs, *lockData, platform, trimmed);
        if (!engine) return {};
        engines.push_back({ platform, *engine });
    }

    std::string kit { inputs.kit };
    if (kit.empty()) {
        std::println("  the openkal kit, from mcpp's build database of each openkal target");
        auto built = openkal_kit(root, base::join_path(root, "target/pack/kit-openkal"));
        if (!built) {
            std::println(std::cerr, "  the openkal kit failed: {}", built.error().message);
            return {};
        }
        kit = *built;
    }

    auto assembled = pack::payload::assemble(pack::payload::AssembleOptions {
        .serverImage = image,
        .serverTree = tree,
        .engines = std::move(engines),
        .kitDirectory = kit,
        .outDirectory = outDirectory,
        .repositoryRoot = root,
        .serverVersion = inputs.serverVersion,
        .candidateEngine = inputs.candidate,
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
        if (auto image = pack::fetch::digest_of(base::join_path(directory, "bin/mcppls.okb")); image) {
            body["server"] = { { "path", "bin/mcppls.okb" }, { "sha256", *image } };
        }
        // The platforms the payload serves, as its payload.json says.
        if (auto text = os::fs::read(base::join_path(directory, "payload.json"))) {
            const auto manifest = nlohmann::json::parse(*text, nullptr, false);
            nlohmann::json platforms = nlohmann::json::array();
            if (manifest.is_object()) {
                for (const auto& item : manifest.value("platforms", nlohmann::json::object()).items()) platforms.push_back(item.key());
            }
            body["platforms"] = platforms;
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
    std::error_code ignored;
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
    if (auto rustup = toolrun::find_program("rustup")) {
        (void) step("checking the wasm32-wasip2 target", *rustup, { "target", "add", "wasm32-wasip2" }, directory);
    }
    if (!step("building the Zed extension", *cargo,
              { "build", "--release", "--target", "wasm32-wasip2" }, directory)) {
        std::println(std::cerr, "  If the target is missing: rustup target add wasm32-wasip2");
        return false;
    }
    const std::string wasm { base::join_path(directory, "target/wasm32-wasip2/release/zed_mcppls.wasm") };
    if (!std::filesystem::is_regular_file(wasm, ignored)) {
        std::println(std::cerr, "  the build produced no {}", wasm);
        return false;
    }
    // Zed loads `extension.wasm` from the extension directory.
    const std::string installed { base::join_path(directory, "extension.wasm") };
    if (auto text = os::fs::read(wasm)) {
        if (auto written = os::fs::write_atomic(installed, *text); !written) {
            std::println(std::cerr, "  cannot write {}: {}", installed, written.error().message());
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

    if (auto only = arguments.value("only"); only && !only->empty()) {
        if (*only != "clangd") {
            std::println(std::cerr, "mcppls-devtools: --only accepts clangd (the other parts have no standalone step here)");
            return 2;
        }
        const std::string platform { arguments.value("platform").value_or(std::string { base::payload_platform() }) };
        return command_payload_only_clangd(root, platform, arguments, wantJson);
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

    const auto inputs = payload_inputs(root, arguments);
    if (!inputs) return 2;
    std::println("mcppls-devtools: one payload for {}", base::join(inputs->platforms, ", "));
    const auto payload = assemble_payload(root, *inputs, out);
    if (!payload.built) return 1;

    auto problems = pack::payload::verify(payload.directory);
    print_verify_result(payload.directory, problems, wantJson);
    if (!wantJson && problems.empty()) std::println("payload: {}", payload.directory);
    return problems.empty() ? 0 : 1;
}

// A payload at `directory`: the one `--payload` names, copied (keeping the executable bits, and
// verified), or one assembled from the server this checkout builds. Every editor needs one — VS
// Code inside its VSIX, CLion inside its package, Zed at editors::server_directory() — and a caller that
// already has one (CI, which built the payload in an earlier job) passes it rather than making
// another. Such a caller has no server beside this tool and needs none: the payload it hands over
// already contains the built server.
int put_payload(const std::string& root, const cmdline::ParsedArgs& arguments, const std::string& directory) {
    std::error_code ignored;
    if (const auto given = arguments.value("payload"); given && !given->empty()) {
        if (!std::filesystem::is_regular_file(base::join_path(*given, "payload.json"), ignored)) {
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
    const auto inputs = payload_inputs(root, arguments);
    if (!inputs) return 2;
    const auto payload = assemble_payload(root, *inputs, directory);
    return payload.built ? 0 : 1;
}

// The CLion plugin is a Gradle project: Kotlin against the IntelliJ platform's LSP API, which the
// paid IDEs have and CLion is one of. Gradle downloads the IDE it compiles against on first build,
// so the first one is long and needs the network.
//
// One package for every system (ecosystem design 2026-10-29 §9.3): it carries the one payload at
// `<plugin>/payload`, given to Gradle with -Pmcppls.payload. Named the way the release names it,
// `mcppls-clion-<version>.zip` under target/pack/.
std::optional<std::string> build_clion(const std::string& root, const cmdline::ParsedArgs& arguments) {
    std::error_code ignored;
    const std::string directory { base::join_path(root, "editors/clion") };
    const std::string payload { base::join_path(root, "target/pack/clion/payload") };
    if (put_payload(root, arguments, payload) != 0) return std::nullopt;
    // `xim:gradle` declares the JDK it runs on, so the manifest naming the one package is what
    // gets both — which is why nothing here asks for a JDK.
    auto gradle = tool("gradle", "an IntelliJ-platform plugin is a Gradle project");
    if (!gradle) return std::nullopt;
    const std::string distributions { base::join_path(directory, "build/distributions") };
    std::filesystem::remove_all(distributions, ignored);
    std::println("  the first build downloads the CLion SDK; it takes a while and needs the network");
    if (!step("building the CLion plugin", *gradle, { "--console=plain", std::format("-Pmcppls.payload={}", payload), "buildPlugin" }, directory)) {
        return std::nullopt;
    }
    std::string produced;
    for (const auto& entry : base::children(distributions)) {
        if (entry.ends_with(".zip")) produced = entry;
    }
    if (produced.empty()) {
        std::println(std::cerr, "  the build produced no package under {}", distributions);
        return std::nullopt;
    }
    const std::string archive { base::join_path(root, std::format("target/pack/{}", base::file_name(produced))) };
    (void) std::filesystem::create_directories(base::parent_path(archive), ignored);
    std::filesystem::copy_file(produced, archive, std::filesystem::copy_options::overwrite_existing, ignored);
    std::println("clion plugin: {}", archive);
    return archive;
}

int build_vscode(const std::string& root, const cmdline::ParsedArgs& arguments, std::string& vsixOut) {
    std::error_code ignored;
    const std::string extension { base::join_path(root, "editors/vscode") };
    // The extension carries its payload inside itself.
    if (const int status = put_payload(root, arguments, base::join_path(extension, "payload")); status != 0) return status;

    auto npm = tool("npm", "the VS Code extension is TypeScript");
    if (!npm) return 1;
    const bool haveModules { std::filesystem::is_directory(base::join_path(extension, "node_modules"), ignored) };
    if (!step(haveModules ? "refreshing the extension's dependencies" : "installing the extension's dependencies",
              *npm, { haveModules ? "install" : "ci", "--no-audit", "--no-fund" }, extension)) {
        return 1;
    }
    if (!step("compiling the extension", *npm, { "run", "compile" }, extension)) return 1;

    // One VSIX for every system (ecosystem design 2026-10-29 §9.3): no target platform, the one payload inside.
    const std::string vsix { arguments.value("out").value_or(
        base::join_path(root, std::format("target/pack/mcppls-{}.vsix", base::VERSION))) };
    (void)std::filesystem::create_directories(base::parent_path(vsix), ignored);
    auto npx = tool("npx", "packaging a VSIX runs vsce from the extension's own dependencies");
    if (!npx) return 1;
    if (!step("packaging the VSIX", *npx, { "--no-install", "vsce", "package", "--out", vsix }, extension)) {
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

// Where Zed finds the server when PATH has none, overridable for tests.
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
    std::println("server: the payload at {} (payload.json's `server`)", directory);
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

// The package carries the server; a plugin archive that carries none starts `mcppls` from PATH.
int install_clion(const cmdline::ParsedArgs& arguments, const std::string& archive) {
    const auto directories = clion_directories(arguments);
    if (directories.empty()) {
        std::println(std::cerr, "mcppls-devtools: no CLion here (nothing under JetBrains' data directory); "
                                "start CLion once, or name its plugins directory with --clion-dir");
        return 1;
    }
    if (auto carries = editors::clion_archive_carries_payload(archive); !carries) {
        std::println(std::cerr, "mcppls-devtools: {}", carries.error().message);
        return 1;
    } else if (!*carries) {
        std::println("  {} carries no payload: the plugin starts mcppls from the PATH CLion sees", archive);
    }
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
        std::println("mcppls-devtools: {} extension", editor_name(editor));
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
            std::optional<std::string> archive { plugin };
            if (!archive) {
                archive = build_clion(root, arguments);
                if (!archive) return 1;
            }
            if (install) {
                if (const int status = install_clion(arguments, *archive); status != 0) return status;
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
    std::error_code ignored;
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
        auto code = toolrun::find_program("code");
        if (!code) {
            std::println("vscode: the `code` command is not on PATH; remove it from the Extensions view.");
            continue;
        }
        if (!step("removing from VS Code", *code, { "--uninstall-extension", "sunrisepeak.mcpp-language-server" }, root)) {
            ++failures;
        }
    }

    // The server Zed installed goes with Zed; the CLion plugin carries its own.
    const bool zedLeft { editors::zed_installed(zed_directory(arguments)) };
    const std::string server { server_directory(arguments) };
    if (!zedLeft && std::filesystem::exists(server, ignored)) {
        std::filesystem::remove_all(server, ignored);
        std::println("server: removed {}", server);
    }
    return failures == 0 ? 0 : 1;
}

} // namespace

cmdline::App payload_command(bool& handled, int& status) {
    cmdline::App command { "payload" };
    (void) command.description(
        "Assemble, verify or copy the payload: the server, the engine of every platform and the openkal kit.\n"
        "  payload                                       one payload: the okb and the runners packed here, every platform's engine\n"
        "  payload --server-image mcppls.okb --server-tree mcppls-<v>-openkal.zip --kit kit --out payload\n"
        "  payload --verify payload                      check an already-assembled payload\n"
        "  payload --from payload --out editors/vscode/payload   copy one, keeping exec bits\n"
        "  payload --only clangd --platform linux-x64 --out work/clangd   trim one platform's clangd alone");
    (void) command.option("platforms").takes_value().multiple().help("Which of packaging/payload.lock.json's platforms it serves (default: every one)");
    (void) command.option("platform").takes_value().help("--only clangd: the platform (default: this host)");
    (void) command.option("out").takes_value().help("Where to put it (default target/pack/payload)");
    (void) command.option("server-version").takes_value().help("Defaults to this build's own version");
    (void) command.option("server-image").takes_value().help("The server's okb (default: packed here)");
    (void) command.option("server-tree").takes_value().help("With --server-image: the openkal tree packed self-contained for every system, or its archive");
    (void) command.option("dev").help("Package a dev-profile server rather than a release one");
    (void) command.option("clangd").takes_value().multiple().help("<platform>=DIR, or openkal=DIR for the ok image: an engine already trimmed (repeatable)");
    (void) command.option("kit").takes_value().help("Use this already-built openkal kit directory");
    (void) command.option("cache").takes_value().help("Where fetched inputs are cached (default <root>/.payload-cache)");
    (void) command.option("no-strip").help("Keep clangd's debug symbols instead of stripping it");
    (void) command.option("candidate").help("With --clangd: a new engine under test, another fork version of the locked LLVM base (CI only)");
    (void) command.option("only").takes_value().help("clangd: trim clangd alone and stop there");
    (void) command.option("verify").takes_value().value_name("DIR").help("Only check an already-assembled payload");
    (void) command.option("from").takes_value().value_name("DIR").help("Copy an already-assembled payload to --out instead of building one");
    (void) command.option("json").help("Machine-readable result on standard output");
    (void) command.action([&handled, &status](const cmdline::ParsedArgs& arguments) { handled = true; status = command_payload(arguments); });
    return command;
}

cmdline::App extension_command(bool& handled, int& status) {
    cmdline::App command { "extension" };
    (void) command.description("Build an editor extension: VS Code (VSIX), Zed (WebAssembly) or CLion (Gradle); each carries the one payload");
    (void) command.option("editor").takes_value().help("vscode | zed | clion | all (default all)");
    (void) command.option("payload").takes_value().help("Use this payload instead of assembling one (CI)");
    (void) command.option("install").help("Also install it (Zed: everything but its palette action unless --link)");
    (void) command.option("out").takes_value().help("Where to write the .vsix");
    (void) command.option("server-image").takes_value().help("The server's okb (default: packed here)");
    (void) command.option("server-tree").takes_value().help("With --server-image: the openkal tree packed self-contained for every system, or its archive");
    (void) command.option("dev").help("Package a dev-profile server rather than a release one");
    (void) command.option("platforms").takes_value().multiple().help("Which platforms the payload serves (default: every one)");
    (void) command.option("clangd").takes_value().multiple().help("<platform>=DIR, or openkal=DIR for the ok image: an engine already trimmed (repeatable)");
    (void) command.option("kit").takes_value().help("Use this already-built openkal kit directory");
    (void) command.option("cache").takes_value().help("Where fetched inputs are cached (default <root>/.payload-cache)");
    (void) command.option("plugin").takes_value().help("Zed: an extension directory, CLion: a plugin archive, to install instead of building one");
    (void) command.option("link").help("Zed: link the extension into Zed here rather than naming its palette action");
    (void) command.option("server-dir").takes_value().help("Where Zed's server goes (default <user data>/mcppls/payload)");
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
