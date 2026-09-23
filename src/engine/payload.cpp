module mcppls.engine.payload;

import std;
import nlohmann.json;
import mcppls.os;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.fs;
import mcppls.toolchain.discover;
import mcppls.platform.env;
import mcppls.platform.dirs;
import mcppls.platform.process;
import mcppls.platform.toolrun;
import mcppls.base.sha256;
import mcppls.spec.kit;
import mcppls.engine.clangd.process;

namespace mcppls::engine {

namespace {

std::string absolute(std::string_view path) {
    if (path.empty() || base::is_absolute_path(path)) return base::normalize_path(path);
    return base::join_path(platform::fs::current_directory(), path);
}

} // namespace

namespace {

// The payload this executable sits in: <payload>/bin/mcppls next to <payload>/payload.json.
std::string enclosing_payload() {
    const auto arguments = platform::env::arguments();
    if (arguments.empty()) return {};
    std::string self { arguments.front() };
    if (!base::is_absolute_path(self)) {
        if (self.find('/') == std::string::npos && self.find('\\') == std::string::npos) {
            self = platform::env::find_executable(self).value_or("");
        } else {
            self = base::join_path(platform::fs::current_directory(), self);
        }
    }
    if (self.empty()) return {};
    const std::string candidate { base::parent_path(base::parent_path(base::normalize_path(self))) };
    return platform::fs::is_regular_file(base::join_path(candidate, "payload.json")) ? candidate : std::string {};
}

// A kit installed by xlings: <store>/xim-x-mcppls-kit/<version>[/<archive root>]/kit.json. With a
// wanted version only that version is taken (S4-4-5: the kit's libc++ is the engine's version);
// without one, the newest.
std::string installed_kit(std::string_view wantedVersion) {
    const std::string home { platform::dirs::home_directory() };
    for (std::string_view store : { ".xlings/data/xpkgs", ".mcpp/registry/data/xpkgs" }) {
        auto versions = platform::fs::list_directory(base::join_path(home, base::join_path(store, "xim-x-mcppls-kit")));
        std::ranges::sort(versions, std::greater<> {});
        for (const auto& version : versions) {
            if (!wantedVersion.empty() && base::file_name(version) != wantedVersion) continue;
            if (platform::fs::is_regular_file(base::join_path(version, "kit.json"))) return version;
            for (const auto& child : platform::fs::list_directory(version)) {
                if (platform::fs::is_regular_file(base::join_path(child, "kit.json"))) return child;
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
    PayloadPaths paths;
    paths.platform = std::string { mcppls::os::PLATFORM };
    const std::string suffix { mcppls::os::EXECUTABLE_SUFFIX };
    PayloadRequest request { requested };
    if (request.payloadDirectory.empty()) request.payloadDirectory = enclosing_payload();
    std::string payloadKit;
    bool clangdDeclared { false };
    if (!request.payloadDirectory.empty()) {
        paths.directory = absolute(request.payloadDirectory);
        const std::string manifest { base::join_path(paths.directory, "payload.json") };
        if (auto text = platform::fs::read_file(manifest)) {
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
            if (platform::fs::is_regular_file(conventional)) paths.clangd = conventional;
        }
        if (payloadKit.empty()) payloadKit = base::join_path(paths.directory, "kit");
    }
    if (!request.clangd.empty()) {
        paths.clangd = absolute(request.clangd);
        paths.clangdVersion.clear();
    }
    if (paths.clangd.empty()) {
        if (auto found = platform::env::find_executable("clangd")) paths.clangd = *found;
    }
    if (paths.clangdVersion.empty() && !paths.clangd.empty() && platform::fs::is_regular_file(paths.clangd)) {
        if (auto result = platform::toolrun::run({ .program = paths.clangd, .arguments = { "--version" },
                                                   .purpose = "engine-version",
                                                   .bounds = platform::RunBounds { .hard = std::chrono::seconds { 20 } } });
            result && !result->timedOut) {
            paths.clangdVersion = engine::clangd::parse_clangd_version(result->output + result->error);
        }
    }

    // The kit that matches the core engine (S4-4-5): an explicit --kit is taken as given; otherwise
    // the payload's when its libc++ is the engine's version, else that version installed by xlings.
    const std::string wanted { request.engine == "none" || paths.clangdVersion.empty() ? std::string {} : paths.clangdVersion };
    if (!request.kit.empty()) {
        const std::string kit { absolute(request.kit) };
        if (platform::fs::is_regular_file(base::join_path(kit, "kit.json"))) paths.kit = kit;
    } else {
        std::string mismatch;
        if (!payloadKit.empty() && platform::fs::is_regular_file(base::join_path(payloadKit, "kit.json"))) {
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
    if constexpr (mcppls::os::FAMILY != mcppls::os::Family::macos) {
        return {};
    } else {
        if (auto sdkroot = platform::env::get("SDKROOT"); sdkroot && platform::fs::is_directory(*sdkroot)) return base::normalize_path(*sdkroot);
        // xcrun is a shim: without developer tools it opens the installation dialog, and this is asked every 30 s.
        if (toolchain::macos_developer_tools_present() && platform::fs::is_regular_file("/usr/bin/xcrun")) {
            if (auto result = platform::toolrun::run({ .program = "/usr/bin/xcrun", .arguments = { "--show-sdk-path" },
                                                       .purpose = "sdk",
                                                       .bounds = platform::RunBounds { .hard = std::chrono::seconds { 20 } } });
                result && result->exitCode == 0) {
                const std::string path { base::trim(result->output) };
                if (platform::fs::is_directory(path)) return path;
            }
        }
        for (std::string_view candidate : { "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk",
                                            "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk" }) {
            if (platform::fs::is_directory(candidate)) return std::string { candidate };
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
    auto text = platform::fs::read_file(cacheFile);
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
    (void)platform::fs::write_file_atomic(cacheFile, document.dump());
}

} // namespace

std::vector<PayloadIntegrityIssue> verify_payload_integrity(const PayloadPaths& payload, std::string_view cacheFile) {
    std::vector<PayloadIntegrityIssue> issues;
    if (payload.files.empty()) return issues;
    auto cache = read_integrity_cache(cacheFile);
    bool cacheChanged { false };
    for (const auto& [relative, expected] : payload.files) {
        const std::string path { base::join_path(payload.directory, relative) };
        const auto stamp = platform::fs::stamp(path);
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
            cached != cache.end() && cached->second.size == stamp->size && cached->second.modified == stamp->modified) {
            actual = cached->second.sha256;
        } else {
            auto computed = file_sha256(path);
            if (!computed) {
                issues.push_back(PayloadIntegrityIssue { relative, "could not be read" });
                continue;
            }
            actual = *computed;
            cache[path] = IntegrityCacheEntry { stamp->size, stamp->modified, actual };
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
