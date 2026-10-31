module mcppls.engine.payload;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.engineidentity;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.base.version;
import mcppls.toolchain.discover;
import mcppls.base.sha256;
import mcppls.spec.kit;
import mcppls.engine.clangd.process;
import mcpplibs.os;
import mcppls.base.home;
import mcppls.base.payload;
import mcppls.toolrun;

namespace os = mcpplibs::os;

namespace mcppls::engine {

namespace {

std::string absolute(std::string_view path) {
    if (path.empty() || base::is_absolute_path(path)) return base::normalize_path(path);
    return base::join_path(base::current_directory(), path);
}

// The payload this executable sits in: <payload>/bin/mcppls[.okb] next to <payload>/payload.json.
std::string enclosing_payload() {
    std::error_code ignored;
    const auto arguments = os::env::arguments();
    if (arguments.empty()) return {};
    std::string self { arguments.front() };
    if (!base::is_absolute_path(self)) {
        if (self.find('/') == std::string::npos && self.find('\\') == std::string::npos) {
            self = toolrun::find_program(self).value_or("");
        } else {
            self = base::join_path(base::current_directory(), self);
        }
    }
    if (self.empty()) return {};
    const std::string candidate { base::parent_path(base::parent_path(base::normalize_path(self))) };
    return std::filesystem::is_regular_file(base::join_path(candidate, "payload.json"), ignored) ? candidate : std::string {};
}

std::map<std::string, PayloadFileIntegrity> read_files(const nlohmann::json& document) {
    std::map<std::string, PayloadFileIntegrity> files;
    const auto listed = document.find("files");
    if (listed == document.end() || !listed->is_object()) return files;
    for (const auto& entry : listed->items()) {
        if (!entry.value().is_object()) continue;
        PayloadFileIntegrity integrity;
        integrity.size = entry.value().value("size", std::uint64_t { 0 });
        integrity.sha256 = entry.value().value("sha256", std::string {});
        if (!integrity.sha256.empty()) files.emplace(entry.key(), std::move(integrity));
    }
    return files;
}

const nlohmann::json* object_member(const nlohmann::json& document, std::string_view key) {
    const auto found = document.find(key);
    return found != document.end() && found->is_object() ? &*found : nullptr;
}

// Versions 1 to 4: one platform, its clangd under `engines.clangd` (3, 4) or `clangd` (1, 2).
PayloadEntry single_platform_entry(const nlohmann::json& document) {
    PayloadEntry entry;
    entry.platform = document.value("platform", std::string {});
    if (const auto* server = object_member(document, "server")) entry.runner = server->value("runner", std::string {});
    const nlohmann::json* clangd { nullptr };
    if (const auto* engines = object_member(document, "engines")) clangd = object_member(*engines, "clangd");
    if (clangd == nullptr) clangd = object_member(document, "clangd");
    if (clangd != nullptr) {
        entry.clangd = clangd->value("path", std::string {});
        entry.clangdRunner = clangd->value("runner", std::string {});
        entry.clangdVersion = clangd->value("version", std::string {});
        if (const auto identity = clangd->find("identity"); identity != clangd->end()) entry.identity = *identity;
    }
    return entry;
}

// The kit that would serve the target, by its name, when one is published; else empty.
std::string published_for(std::string_view name) {
    const auto published = published_kit_targets();
    return std::ranges::find(published, name) != published.end() ? std::string { name } : std::string {};
}

// The architecture as mcpp spells it in a triple's first component.
std::string_view mcpp_arch(std::string_view arch) {
    if (arch == "amd64" || arch == "x64") return "x86_64";
    if (arch == "arm64") return "aarch64";
    return arch;
}

// Kit packages xlings installed under one store, by their package directory (`xim-x-mcppls-kit-<target>`).
void collect_installed(std::string_view store, std::string_view wantedVersion, std::vector<std::string>& found) {
    std::error_code ignored;
    for (const auto& package : base::children(store)) {
        const std::string name { base::file_name(package) };
        if (name != "xim-x-mcppls-kit" && !name.starts_with("xim-x-mcppls-kit-")) continue;
        // The openkal kit follows the platform, not the engine: its newest is taken.
        const bool engineVersioned { name != "xim-x-mcppls-kit-openkal" };
        auto versions = base::children(package);
        std::ranges::sort(versions, std::greater<> {});
        for (const auto& version : versions) {
            if (engineVersioned && !wantedVersion.empty() && base::file_name(version) != wantedVersion) continue;
            std::string root;
            if (std::filesystem::is_regular_file(base::join_path(version, "kit.json"), ignored)) root = version;
            for (const auto& child : base::children(version)) {
                if (root.empty() && std::filesystem::is_regular_file(base::join_path(child, "kit.json"), ignored)) root = child;
            }
            if (root.empty()) continue;
            found.push_back(root);
            break;
        }
    }
}

// The installed kits of the engine's LLVM base at another version only: the notice says what to install.
std::string version_notice(std::string_view wantedVersion, std::string_view clangdVersion) {
    if (wantedVersion.empty()) return {};
    std::vector<std::string> any;
    for (std::string_view store : { ".xlings/data/xpkgs", ".mcpp/registry/data/xpkgs" }) {
        collect_installed(base::join_path(base::home_directory(), store), "", any);
    }
    for (const auto& root : any) {
        const auto kit = spec::load_kit(root);
        if (!kit || kit->stdlibName != "libc++" || !kit->variants.empty() || kit->stdlibVersion == wantedVersion) continue;
        const std::string name { kit_target_name(kit->target) };
        return std::format("the semantic kit for {} carries libc++ {}, but clangd {} needs libc++ {}; install mcppls-kit-{}@{}", name,
                           kit->stdlibVersion, clangdVersion, wantedVersion, name, wantedVersion);
    }
    return {};
}

} // namespace

base::Result<PayloadManifest> read_payload_manifest(std::string_view text) {
    const nlohmann::json document = nlohmann::json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) return base::fail("payload-manifest", "payload.json is not a JSON object");
    PayloadManifest manifest;
    manifest.version = document.value("payload-version", 0);
    if (manifest.version < 1 || manifest.version > HIGHEST_PAYLOAD_VERSION) {
        return base::fail("payload-manifest", std::format("payload-version {} is not one this server reads (1 to {})", manifest.version,
                                                          HIGHEST_PAYLOAD_VERSION));
    }
    if (const auto* server = object_member(document, "server")) manifest.serverPath = server->value("path", std::string {});
    if (const auto* kit = object_member(document, "kit")) manifest.kitPath = kit->value("path", std::string { "kit" });
    else manifest.kitPath = "kit";
    manifest.files = read_files(document);
    if (manifest.version < 5) {
        PayloadEntry entry { single_platform_entry(document) };
        // One that names no platform was assembled for the machine it is on.
        if (entry.platform.empty()) entry.platform = std::string { base::payload_platform() };
        // A version 1 to 4 payload's integrity files are its one platform's.
        entry.files = std::move(manifest.files);
        manifest.files.clear();
        manifest.entries.push_back(std::move(entry));
        return manifest;
    }
    const auto* platforms = object_member(document, "platforms");
    if (platforms == nullptr) return base::fail("payload-manifest", "payload.json (version 5) has no platforms");
    for (const auto& item : platforms->items()) {
        const std::string name { item.key() };
        const auto& value = item.value();
        if (!value.is_object()) return base::fail("payload-manifest", std::format("platforms.{} is not an object", name));
        PayloadEntry entry;
        entry.platform = name;
        entry.runner = value.value("runner", std::string {});
        if (const auto* engines = object_member(value, "engines")) {
            if (const auto* clangd = object_member(*engines, "clangd")) {
                entry.clangd = clangd->value("path", std::string {});
                entry.clangdRunner = clangd->value("runner", std::string {});
                entry.clangdVersion = clangd->value("version", std::string {});
                if (const auto identity = clangd->find("identity"); identity != clangd->end()) entry.identity = *identity;
            }
        }
        entry.files = read_files(value);
        manifest.entries.push_back(std::move(entry));
    }
    return manifest;
}

std::expected<PayloadEntry, std::string> payload_entry(const PayloadManifest& manifest, std::string_view platform) {
    const auto found = std::ranges::find(manifest.entries, platform, &PayloadEntry::platform);
    if (found != manifest.entries.end()) return *found;
    std::vector<std::string> carried;
    for (const auto& entry : manifest.entries) carried.push_back(entry.platform);
    return std::unexpected { std::format("this payload carries no engine for {}, the platform this server runs on; it carries {}", platform,
                                         carried.empty() ? std::string { "none" } : base::join(carried, ", ")) };
}

EngineCommand engine_command(std::string_view clangd, std::string_view runner) {
    if (runner.empty()) return EngineCommand { std::string { clangd }, {} };
    return EngineCommand { std::string { runner }, { std::string { clangd } } };
}

PayloadPaths resolve_payload(const PayloadRequest& requested) {
    std::error_code ignored;
    PayloadPaths paths;
    paths.platform = std::string { base::payload_platform() };
    const std::string suffix { os::conventions().exe_suffix };
    PayloadRequest request { requested };
    if (request.payloadDirectory.empty()) request.payloadDirectory = enclosing_payload();
    std::string payloadKit;
    bool clangdDeclared { false };
    std::string clangdRelative;
    if (!request.payloadDirectory.empty()) {
        paths.directory = absolute(request.payloadDirectory);
        if (auto text = os::fs::read(base::join_path(paths.directory, "payload.json"))) {
            if (auto manifest = read_payload_manifest(*text)) {
                for (const auto& entry : manifest->entries) paths.platforms.push_back(entry.platform);
                payloadKit = base::join_path(paths.directory, manifest->kitPath);
                paths.files = manifest->files;
                // One payload for every platform: this server takes its own platform's entry, and refuses a payload that
                // has none, rather than starting another platform's engine.
                if (auto entry = payload_entry(*manifest, paths.platform)) {
                    if (!entry->clangd.empty()) {
                        clangdRelative = entry->clangd;
                        paths.clangd = base::join_path(paths.directory, entry->clangd);
                        if (!entry->clangdRunner.empty()) paths.clangdRunner = base::join_path(paths.directory, entry->clangdRunner);
                        paths.clangdVersion = entry->clangdVersion;
                        clangdDeclared = true;
                    }
                    paths.files.insert(entry->files.begin(), entry->files.end());
                    // Version 4 and later bind each engine's identity (S4-4-10).
                    if (manifest->version >= 4) {
                        auto decoded = base::decode_engine_identity(entry->identity.is_null() ? nlohmann::json {} : entry->identity);
                        if (!decoded) paths.identityProblem = decoded.error().message;
                        else paths.engineIdentity = std::move(*decoded);
                    }
                } else {
                    paths.identityProblem = entry.error();
                    clangdDeclared = true;   // no engine of this platform: none of another is taken from the payload
                }
            }
        }
        // A payload that does not declare clangd may still carry it in the conventional place; one that
        // carries none leaves clangd to PATH below (a declared but missing clangd stays missing).
        if (!clangdDeclared) {
            for (const std::string conventional : { base::join_path(paths.directory, std::format("clangd/{}/bin/clangd{}", paths.platform, suffix)),
                                                    base::join_path(paths.directory, "clangd/bin/clangd" + suffix) }) {
                if (paths.clangd.empty() && std::filesystem::is_regular_file(conventional, ignored)) paths.clangd = conventional;
            }
        }
        if (payloadKit.empty()) payloadKit = base::join_path(paths.directory, "kit");
    }
    if (paths.engineIdentity) {
        const auto expected = paths.files.find(clangdRelative.empty() ? "clangd/bin/clangd" + suffix : clangdRelative);
        // An engine its runner starts is the ok image, whose identity names openkal, not a system.
        const auto problem = base::engine_identity_problem(*paths.engineIdentity, paths.clangdVersion,
            paths.clangdRunner.empty() ? std::string_view { paths.platform } : std::string_view { "openkal" },
            expected == paths.files.end() ? std::string_view {} : std::string_view { expected->second.sha256 });
        if (problem) paths.identityProblem = *problem;
    }
    if (!request.clangd.empty()) {
        paths.clangd = absolute(request.clangd);
        paths.clangdRunner.clear();
        paths.clangdVersion.clear();
        paths.engineIdentity.reset();
        paths.identityProblem.clear();
    }
    if (paths.clangd.empty()) {
        if (auto found = toolrun::find_program("clangd")) paths.clangd = *found;
    }
    if (paths.clangdVersion.empty() && !paths.clangd.empty() && std::filesystem::is_regular_file(paths.clangd, ignored)) {
        const auto command = engine_command(paths.clangd, paths.clangdRunner);
        auto arguments = command.arguments;
        arguments.push_back("--version");
        if (auto result = mcppls::toolrun::run({ .program = command.program, .arguments = arguments,
                                                   .purpose = "engine-version",
                                                   .bounds = mcpplibs::os::process::bounds { .hard = std::chrono::seconds { 20 } } });
            result && !result->timed_out) {
            paths.clangdVersion = engine::clangd::parse_clangd_version(result->out + result->err);
        }
    }

    // The kits to choose from per target (S4 section 5): an explicit --kit, which serves every target; the payload's
    // (the openkal kit), taken whatever its libc++, which is the one its headers carry (S4-4-5); and those xlings
    // installed, a target's at the engine's LLVM base version.
    const std::string wanted { request.engine == "none" || paths.clangdVersion.empty() ? std::string {} : std::string { base::llvm_base_version(paths.clangdVersion) } };
    if (!request.kit.empty()) {
        const std::string kit { absolute(request.kit) };
        if (std::filesystem::is_regular_file(base::join_path(kit, "kit.json"), ignored)) {
            paths.kits.push_back(kit);
            paths.explicitKit = true;
        }
    }
    if (!paths.explicitKit) {
        if (!payloadKit.empty() && std::filesystem::is_regular_file(base::join_path(payloadKit, "kit.json"), ignored)) paths.payloadKit = payloadKit;
        paths.kitVersion = wanted;
        paths.kits = kit_roots(paths);
        const auto host = kit_for_target(paths, "");
        if (host.kit) paths.kit = host.kit->root;
        if (!host.missingTarget.empty()) paths.kitNotice = version_notice(wanted, paths.clangdVersion);
    } else {
        paths.kit = paths.kits.front();
    }
    return paths;
}

// ---- the kit of a target ---------------------------------------------------------------------

std::span<const std::string_view> published_kit_targets() {
    static constexpr std::array<std::string_view, 4> PUBLISHED { "x86_64-linux-gnu", "aarch64-linux-gnu", "x86_64-w64-mingw32", "arm64-apple-darwin" };
    return PUBLISHED;
}

std::string kit_target_name(std::string_view triple) {
    std::vector<std::string_view> parts;
    for (auto part : std::views::split(triple, '-')) parts.emplace_back(part.begin(), part.end());
    if (parts.empty() || parts.front().empty()) return std::string { triple };
    const std::string_view arch { mcpp_arch(parts.front()) };
    const auto has = [&](std::string_view word) { return std::ranges::find(parts, word) != parts.end(); };
    const auto prefixed = [&](std::string_view prefix) {
        return std::ranges::any_of(parts | std::views::drop(1), [&](std::string_view part) { return part.starts_with(prefix); });
    };
    if (has("openkal")) return std::format("{}-openkal", arch);
    if (has("windows") || has("mingw32") || has("w64") || has("win32")) return std::format("{}-w64-mingw32", arch);
    if (has("apple") || prefixed("darwin") || prefixed("macos")) return std::format("{}-apple-darwin", arch == "aarch64" ? "arm64" : arch);
    if (has("linux")) {
        if (prefixed("musl")) return std::format("{}-linux-musl", arch);
        if (prefixed("android")) return std::format("{}-linux-android", arch);
        return std::format("{}-linux-gnu", arch);
    }
    std::string rest { arch };
    for (auto part : parts | std::views::drop(1)) rest += std::format("-{}", part);
    return rest;
}

std::string host_kit_target() {
    const std::string_view arch { os::ARCH == os::architecture::aarch64 ? "aarch64" : "x86_64" };
    if (os::host() == os::platform::windows) return std::format("{}-w64-mingw32", arch);
    if (os::host() == os::platform::macos) return std::format("{}-apple-darwin", arch == "aarch64" ? "arm64" : arch);
    return std::format("{}-linux-gnu", arch);
}

KitChoice choose_kit(std::span<const spec::Kit> kits, bool explicitKit, std::string_view target, std::string_view host) {
    KitChoice choice;
    const std::string hostName { kit_target_name(host) };
    choice.target = target.empty() ? hostName : kit_target_name(target);
    if (explicitKit && !kits.empty()) {
        // --kit is the person's choice for every target; a kit of several targets serves the one it has.
        choice.kit = spec::kit_variant(kits.front(), choice.target).value_or(kits.front());
        return choice;
    }
    // A kit of one target serves it; a kit of several serves each of its `targets`.
    const auto serving = [&](std::string_view name) -> std::optional<spec::Kit> {
        for (const auto& kit : kits) {
            if (kit.variants.empty() && kit_target_name(kit.target) == name) return kit;
            if (auto variant = spec::kit_variant(kit, name)) return variant;
        }
        return std::nullopt;
    };
    if ((choice.kit = serving(choice.target))) return choice;
    choice.missingTarget = published_for(choice.target);
    if (choice.target != hostName) {
        if ((choice.kit = serving(hostName))) return choice;
        if (choice.missingTarget.empty()) choice.missingTarget = published_for(hostName);
    }
    // The openkal kit of this architecture: `import std` and the standard library, without the system's own headers.
    const std::string_view arch { hostName.starts_with("x86_64") ? "x86_64" : "aarch64" };
    choice.kit = serving(std::format("{}-openkal", arch));
    return choice;
}

std::vector<std::string> kit_roots(const PayloadPaths& payload) {
    if (payload.explicitKit) return { payload.kits.front() };
    std::vector<std::string> roots;
    if (!payload.payloadKit.empty()) roots.push_back(payload.payloadKit);
    for (auto& root : installed_kits(payload.kitVersion)) roots.push_back(std::move(root));
    return roots;
}

std::vector<spec::Kit> load_kits(const PayloadPaths& payload) {
    std::vector<spec::Kit> kits;
    for (const auto& root : payload.kits) {
        if (auto kit = spec::load_kit(root)) kits.push_back(std::move(*kit));
        else base::log::warning("semantic kit unusable ({}): {}", root, kit.error().message);
    }
    return kits;
}

KitChoice kit_for_target(const PayloadPaths& payload, std::string_view target) {
    const auto kits = load_kits(payload);
    return choose_kit(kits, payload.explicitKit, target, host_kit_target());
}

std::vector<std::string> installed_kits(std::string_view wantedVersion) {
    std::vector<std::string> found;
    for (std::string_view store : { ".xlings/data/xpkgs", ".mcpp/registry/data/xpkgs" }) {
        collect_installed(base::join_path(base::home_directory(), store), wantedVersion, found);
    }
    return found;
}

std::string macos_sdk_path() {
    std::error_code ignored;
    if (os::host() != os::platform::macos) {
        return {};
    } else {
        if (auto sdkroot = os::env::get("SDKROOT"); sdkroot && std::filesystem::is_directory(*sdkroot, ignored)) return base::normalize_path(*sdkroot);
        // xcrun is a shim: without developer tools it opens the installation dialog, and this is asked every 30 s.
        if (toolchain::macos_developer_tools_present() && std::filesystem::is_regular_file("/usr/bin/xcrun", ignored)) {
            if (auto result = mcppls::toolrun::run({ .program = "/usr/bin/xcrun", .arguments = { "--show-sdk-path" },
                                                       .purpose = "sdk",
                                                       .bounds = mcpplibs::os::process::bounds { .hard = std::chrono::seconds { 20 } } });
                result && result->status.success()) {
                const std::string path { base::trim(result->out) };
                if (std::filesystem::is_directory(path, ignored)) return path;
            }
        }
        for (std::string_view candidate : { "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk",
                                            "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk" }) {
            if (std::filesystem::is_directory(candidate, ignored)) return std::string { candidate };
        }
        return {};
    }
}

namespace {

// A block at a time, so a ~90MB clangd executable is never held in memory whole (mirroring
// src/tools/conformance.cpp's own digest()).
std::optional<std::string> file_sha256(std::string_view path) {
    std::ifstream stream { std::filesystem::path { path }, std::ios::binary };
    if (!stream) return std::nullopt;
    base::Sha256 hasher;
    std::vector<char> block(std::size_t { 1 } << 16);
    while (stream.read(block.data(), static_cast<std::streamsize>(block.size())) || stream.gcount() > 0) {
        hasher.update(std::string_view { block.data(), static_cast<std::size_t>(stream.gcount()) });
    }
    return hasher.finish();
}

struct IntegrityCacheEntry {
    std::uint64_t size { 0 };
    std::int64_t modified { 0 };
    std::string sha256;
};

std::map<std::string, IntegrityCacheEntry> read_integrity_cache(std::string_view cacheFile) {
    std::map<std::string, IntegrityCacheEntry> cache;
    auto text = os::fs::read(cacheFile);
    if (!text) return cache;
    nlohmann::json document = nlohmann::json::parse(*text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) return cache;
    for (const auto& entry : document.items()) {
        if (!entry.value().is_object()) continue;
        cache.emplace(entry.key(), IntegrityCacheEntry { entry.value().value("size", std::uint64_t { 0 }),
                                                          entry.value().value("modified", std::int64_t { 0 }),
                                                          entry.value().value("sha256", std::string {}) });
    }
    return cache;
}

void write_integrity_cache(std::string_view cacheFile, const std::map<std::string, IntegrityCacheEntry>& cache) {
    nlohmann::json document = nlohmann::json::object();
    for (const auto& [path, entry] : cache) {
        document[path] = nlohmann::json { { "size", entry.size }, { "modified", entry.modified }, { "sha256", entry.sha256 } };
    }
    (void)os::fs::write_atomic(cacheFile, document.dump());
}

} // namespace

std::vector<PayloadIntegrityIssue> verify_payload_integrity(const PayloadPaths& payload, std::string_view cacheFile) {
    std::vector<PayloadIntegrityIssue> issues;
    if (!payload.identityProblem.empty()) issues.push_back(PayloadIntegrityIssue { "payload.json", payload.identityProblem });
    if (payload.files.empty()) return issues;
    auto cache = read_integrity_cache(cacheFile);
    bool cacheChanged { false };
    for (const auto& [relative, expected] : payload.files) {
        const std::string path { base::join_path(payload.directory, relative) };
        const auto stamp = base::file_stamp(path);
        if (!stamp) {
            issues.push_back(PayloadIntegrityIssue { relative, "is missing" });
            continue;
        }
        if (stamp->size != expected.size) {
            issues.push_back(PayloadIntegrityIssue { relative, std::format("is {} bytes, expected {}", stamp->size, expected.size) });
            continue;
        }
        std::string actual;
        if (const auto cached = cache.find(path);
            cached != cache.end() && cached->second.size == stamp->size && cached->second.modified == static_cast<std::int64_t>(stamp->modified_ns)) {
            actual = cached->second.sha256;
        } else {
            auto computed = file_sha256(path);
            if (!computed) {
                issues.push_back(PayloadIntegrityIssue { relative, "could not be read" });
                continue;
            }
            actual = *computed;
            cache[path] = IntegrityCacheEntry { stamp->size, static_cast<std::int64_t>(stamp->modified_ns), actual };
            cacheChanged = true;
        }
        if (actual != expected.sha256) {
            issues.push_back(PayloadIntegrityIssue { relative, "does not match the payload manifest" });
        }
    }
    if (cacheChanged) write_integrity_cache(cacheFile, cache);
    return issues;
}

} // namespace mcppls::engine
