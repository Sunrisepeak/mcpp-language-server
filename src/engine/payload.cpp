module mcppls.engine.payload;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.engineidentity;
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

} // namespace

namespace {

// The payload this executable sits in: <payload>/bin/mcppls next to <payload>/payload.json.
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

// A kit installed by xlings: <store>/xim-x-mcppls-kit/<version>[/<archive root>]/kit.json. With a
// wanted version only that version is taken (S4-4-5: the kit's libc++ is the engine's version);
// without one, the newest.
std::string installed_kit(std::string_view wantedVersion) {
    std::error_code ignored;
    const std::string home { base::home_directory() };
    for (std::string_view store : { ".xlings/data/xpkgs", ".mcpp/registry/data/xpkgs" }) {
        auto versions = base::children(base::join_path(home, base::join_path(store, "xim-x-mcppls-kit")));
        std::ranges::sort(versions, std::greater<> {});
        for (const auto& version : versions) {
            if (!wantedVersion.empty() && base::file_name(version) != wantedVersion) continue;
            if (std::filesystem::is_regular_file(base::join_path(version, "kit.json"), ignored)) return version;
            for (const auto& child : base::children(version)) {
                if (std::filesystem::is_regular_file(base::join_path(child, "kit.json"), ignored)) return child;
            }
        }
    }
    return {};
}

std::string kit_version(std::string_view kitRoot) {
    auto kit = spec::load_kit(kitRoot);
    return kit ? kit->stdlibVersion : std::string {};
}

} // namespace

PayloadPaths resolve_payload(const PayloadRequest& requested) {
    std::error_code ignored;
    PayloadPaths paths;
    paths.platform = std::string { base::payload_platform() };
    const std::string suffix { os::conventions().exe_suffix };
    PayloadRequest request { requested };
    if (request.payloadDirectory.empty()) request.payloadDirectory = enclosing_payload();
    std::string payloadKit;
    bool clangdDeclared { false };
    if (!request.payloadDirectory.empty()) {
        paths.directory = absolute(request.payloadDirectory);
        const std::string manifest { base::join_path(paths.directory, "payload.json") };
        if (auto text = os::fs::read(manifest)) {
            nlohmann::json document = nlohmann::json::parse(*text, nullptr, false);
            if (!document.is_discarded() && document.is_object()) {
                // Payload version 3 names each engine's executable, version and kit; versions 1 and 2 the clangd and kit parts.
                const nlohmann::json* clangd { nullptr };
                if (auto engines = document.find("engines"); engines != document.end() && engines->is_object()) {
                    if (auto entry = engines->find("clangd"); entry != engines->end() && entry->is_object()) clangd = &*entry;
                }
                if (clangd == nullptr) {
                    if (auto entry = document.find("clangd"); entry != document.end() && entry->is_object()) clangd = &*entry;
                }
                if (clangd != nullptr && !clangd->value("path", std::string {}).empty()) {
                    paths.clangd = base::join_path(paths.directory, clangd->value("path", std::string {}));
                    paths.clangdVersion = clangd->value("version", std::string {});
                    clangdDeclared = true;
                }
                const nlohmann::json* kit { nullptr };
                if (clangd != nullptr) {
                    if (auto entry = clangd->find("kit"); entry != clangd->end() && entry->is_object()) kit = &*entry;
                }
                if (kit == nullptr) {
                    if (auto entry = document.find("kit"); entry != document.end() && entry->is_object()) kit = &*entry;
                }
                if (kit != nullptr) payloadKit = base::join_path(paths.directory, kit->value("path", std::string { "kit" }));
                paths.platform = document.value("platform", paths.platform);
                // Payload version 4 (mcppls.pack.payload's IDENTIFIED_PAYLOAD_VERSION) binds the engine's identity.
                if (document.value("payload-version", 0) == 4) {
                    if (paths.platform != base::payload_platform()) paths.identityProblem = "engine payload platform does not match this host";
                    auto decoded = base::decode_engine_identity(clangd == nullptr ? nlohmann::json {} : clangd->value("identity", nlohmann::json {}));
                    if (!decoded) paths.identityProblem = decoded.error().message;
                    else paths.engineIdentity = std::move(*decoded);
                }
                // usable plan W9.4: "files": { "clangd/bin/clangd": {"size":N,"sha256":"..."}, ... },
                // written by `mcppls-devtools payload` (mcppls.pack.payload). Absent in an older payload; nothing is checked then.
                if (auto files = document.find("files"); files != document.end() && files->is_object()) {
                    for (const auto& entry : files->items()) {
                        if (!entry.value().is_object()) continue;
                        PayloadFileIntegrity integrity;
                        integrity.size = entry.value().value("size", std::uint64_t { 0 });
                        integrity.sha256 = entry.value().value("sha256", std::string {});
                        if (integrity.sha256.empty()) continue;
                        paths.files.emplace(entry.key(), std::move(integrity));
                    }
                }
            }
        }
        // A payload that does not declare clangd may still carry it in the conventional place; one that
        // carries none leaves clangd to PATH below (a declared but missing clangd stays missing).
        if (!clangdDeclared) {
            const std::string conventional { base::join_path(paths.directory, "clangd/bin/clangd" + suffix) };
            if (std::filesystem::is_regular_file(conventional, ignored)) paths.clangd = conventional;
        }
        if (payloadKit.empty()) payloadKit = base::join_path(paths.directory, "kit");
    }
    if (paths.engineIdentity) {
        const auto expected = paths.files.find("clangd/bin/clangd" + suffix);
        const auto problem = base::engine_identity_problem(*paths.engineIdentity, paths.clangdVersion, paths.platform,
            expected == paths.files.end() ? std::string_view {} : std::string_view { expected->second.sha256 });
        if (problem) paths.identityProblem = *problem;
    }
    if (!request.clangd.empty()) {
        paths.clangd = absolute(request.clangd);
        paths.clangdVersion.clear();
        paths.engineIdentity.reset();
        paths.identityProblem.clear();
    }
    if (paths.clangd.empty()) {
        if (auto found = toolrun::find_program("clangd")) paths.clangd = *found;
    }
    if (paths.clangdVersion.empty() && !paths.clangd.empty() && std::filesystem::is_regular_file(paths.clangd, ignored)) {
        if (auto result = mcppls::toolrun::run({ .program = paths.clangd, .arguments = { "--version" },
                                                   .purpose = "engine-version",
                                                   .bounds = mcpplibs::os::process::bounds { .hard = std::chrono::seconds { 20 } } });
            result && !result->timed_out) {
            paths.clangdVersion = engine::clangd::parse_clangd_version(result->out + result->err);
        }
    }

    // The kit that matches the core engine (S4-4-5): an explicit --kit is taken as given; otherwise
    // the payload's when its libc++ is the engine's version, else that version installed by xlings.
    const std::string wanted { request.engine == "none" || paths.clangdVersion.empty() ? std::string {} : std::string { base::llvm_base_version(paths.clangdVersion) } };
    if (!request.kit.empty()) {
        const std::string kit { absolute(request.kit) };
        if (std::filesystem::is_regular_file(base::join_path(kit, "kit.json"), ignored)) paths.kit = kit;
    } else {
        std::string mismatch;
        if (!payloadKit.empty() && std::filesystem::is_regular_file(base::join_path(payloadKit, "kit.json"), ignored)) {
            const std::string version { kit_version(payloadKit) };
            if (wanted.empty() || version == wanted) {
                paths.kit = payloadKit;
            } else {
                mismatch = version;
            }
        }
        if (paths.kit.empty()) paths.kit = installed_kit(wanted);
        if (paths.kit.empty() && !mismatch.empty()) {
            paths.kitNotice = std::format("the semantic kit carries libc++ {}, but clangd {} needs libc++ {}; install mcppls-kit {}", mismatch,
                                          paths.clangdVersion, wanted, wanted);
        }
    }
    return paths;
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
