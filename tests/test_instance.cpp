// Instances sharing a workspace (overall design 6.3), the payload's entry for this platform (payload version 5) and the
// kit of a target (S4 section 5).
import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.base.path;
import mcppls.base.sha256;
import mcppls.engine.payload;
import mcppls.orchestrator.instance;
import mcpplibs.os;
import mcppls.base.payload;
import mcppls.spec.kit;
import mcppls.cli.status;
import mcppls.project.model;

namespace base = mcppls::base;

namespace os = mcpplibs::os;

using Json = nlohmann::json;
namespace orch = mcppls::orchestrator;
namespace eng = mcppls::engine;

namespace {

std::string scratch(std::string_view name) {
    std::error_code ignored;
    const std::string root { mcppls::base::join_path(base::spelled(os::dirs::temp()),
        std::format("mcppls-test-{}-{}", name, std::chrono::steady_clock::now().time_since_epoch().count())) };
    (void)std::filesystem::create_directories(root, ignored);
    return root;
}

// A kit of this host's own target, so the host's choice takes it on every system the tests run on.
void write_kit(std::string_view root, std::string_view version) {
    std::error_code ignored;
    (void)std::filesystem::create_directories(root, ignored);
    const Json kit { { "kit-version", 1 }, { "name", std::format("mcppls-kit-libcxx-{}", version) }, { "target", eng::host_kit_target() },
                     { "stdlib", Json { { "name", "libc++" }, { "version", std::string { version } }, { "module-metadata", "lib/libc++.modules.json" } } },
                     { "system-include-directories", Json::array({ "include/c++/v1" }) }, { "licenses", Json::array() } };
    (void)os::fs::write(mcppls::base::join_path(root, "kit.json"), kit.dump());
}

mcppls::spec::Kit kit_of(std::string name, std::string target, std::vector<std::string> variants = {}) {
    mcppls::spec::Kit kit { .root = "/kits/" + name, .name = name, .target = target, .stdlibName = "libc++", .stdlibVersion = "23.1.0" };
    for (auto& variant : variants) {
        const std::string triple { variant.starts_with("x86_64") ? "x86_64-unknown-none-elf" : "aarch64-unknown-none-elf" };
        kit.variants.push_back(mcppls::spec::KitVariant { .name = variant, .target = triple,
                                                          .systemIncludeDirectories = { std::format("/kits/{}/include/{}", name, variant) } });
    }
    return kit;
}

} // namespace

int main() {
    std::error_code ignored;
    using namespace mcppls::testing;

    "the first instance owns a workspace and a second works in a private directory"_test = [] {
        std::error_code ignored;
        const std::string workspace { scratch("lease") };
        const auto now = std::chrono::system_clock::now();
        auto owner = orch::WorkspaceLease::acquire(workspace, now);
        expect(!owner.shared() && owner.directory() == workspace);
        auto guest = orch::WorkspaceLease::acquire(workspace, now + std::chrono::seconds { 5 });
        expect(guest.shared()) << "the fresh lease of a live owner is shared";
        expect(guest.directory() != workspace) << guest.directory();
        expect(std::filesystem::is_directory(guest.directory(), ignored)) << guest.directory();
        const std::string guestDirectory { guest.directory() };
        guest.release();
        expect(!std::filesystem::exists(guestDirectory, ignored)) << "a guest removes its private directory";

        // An owner that stopped renewing its lease is taken over; one that renews keeps it.
        owner.renew(now + std::chrono::seconds { 20 });
        expect(orch::WorkspaceLease::acquire(workspace, now + std::chrono::seconds { 45 }).shared()) << "renewed 25 seconds ago";
        auto successor = orch::WorkspaceLease::acquire(workspace, now + std::chrono::seconds { 60 });
        expect(!successor.shared()) << "40 seconds without renewal";
        owner.release();
        expect(std::filesystem::exists(mcppls::base::join_path(workspace, "owner.lease"), ignored)) << "a stale owner does not remove its successor's lease";
        successor.release();
        expect(!std::filesystem::exists(mcppls::base::join_path(workspace, "owner.lease"), ignored));
        std::filesystem::remove_all(workspace, ignored);
    };

    // C-5 (plan 2026-09-30): a server that crashed left its lease fresh, the editor restarted it at once, and the new one
    // took itself for a second instance and started cold in a private directory. A lease names its process now.
    "a fresh lease whose process is gone, or whose pid another process has, is taken over at once"_test = [] {
        std::error_code ignored;
        if (os::host() != os::platform::linux) return;   // the heartbeat alone decides elsewhere
        const std::string workspace { scratch("lease-owner") };
        const auto now = std::chrono::system_clock::now();
        const auto heartbeat = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        const std::string lease { mcppls::base::join_path(workspace, "owner.lease") };
        (void)os::fs::write(lease, Json { { "token", "crashed" }, { "heartbeat", heartbeat }, { "pid", 999999999 }, { "started", "12345" } }.dump());
        auto restarted = orch::WorkspaceLease::acquire(workspace, now + std::chrono::seconds { 2 });
        expect(!restarted.shared() && restarted.directory() == workspace) << "the owner's process is gone";
        restarted.release();

        const auto self = os::fs::read("/proc/self/stat");
        expect(fatal(self.has_value()));
        const std::int64_t pid { std::stoll(self->substr(0, self->find(' '))) };
        // An older server's lease names its start in another scheme, and its pid is alive: it keeps its workspace.
        (void)os::fs::write(lease, Json { { "token", "older" }, { "heartbeat", heartbeat }, { "pid", pid }, { "started", "1" } }.dump());
        auto beside = orch::WorkspaceLease::acquire(workspace, now + std::chrono::seconds { 2 });
        expect(beside.shared()) << "a live pid under a lease of another scheme is not taken over";
        beside.release();
        (void)os::fs::write(lease, Json { { "token", "reused" }, { "heartbeat", heartbeat }, { "pid", pid }, { "started", "ok:1" } }.dump());
        auto reused = orch::WorkspaceLease::acquire(workspace, now + std::chrono::seconds { 2 });
        expect(!reused.shared()) << "the pid is another process's now";
        // The live owner itself: a second instance still works in a private directory.
        auto guest = orch::WorkspaceLease::acquire(workspace, now + std::chrono::seconds { 3 });
        expect(guest.shared()) << "the owner is this very process, alive";
        guest.release();
        reused.release();
        std::filesystem::remove_all(workspace, ignored);
    };

    "a payload's kit is taken whatever its libc++, which is the version its headers carry (S4-4-5)"_test = [] {
        std::error_code ignored;
        const std::string payload { scratch("payload-v3") };
        (void)std::filesystem::create_directories(mcppls::base::join_path(payload, "clangd/bin"), ignored);
        (void)os::fs::write(mcppls::base::join_path(payload, "clangd/bin/clangd"), "not really clangd");
        write_kit(mcppls::base::join_path(payload, "kit"), "23.1.0");
        const Json manifest { { "payload-version", 3 }, { "platform", base::payload_platform() },
                              { "engines", Json { { "clangd", Json { { "version", "23.1.0" }, { "path", "clangd/bin/clangd" },
                                                                     { "kit", Json { { "name", "mcppls-kit-libcxx-23.1.0" }, { "path", "kit" } } } } } } } };
        (void)os::fs::write(mcppls::base::join_path(payload, "payload.json"), manifest.dump());

        const auto matching = eng::resolve_payload(eng::PayloadRequest { payload, "", "", "clangd" });
        expect(matching.clangd == mcppls::base::join_path(payload, "clangd/bin/clangd") && matching.clangdVersion == "23.1.0") << matching.clangd;
        expect(matching.kit == mcppls::base::join_path(payload, "kit")) << matching.kit;
        expect(matching.kitVersion == "23.1.0") << "an installed target kit is taken at the engine's LLVM base";

        // Retain the full maintained-engine identity while selecting its base kit.
        Json forkManifest = manifest;
        forkManifest["engines"]["clangd"]["version"] = "23.1.0-mcppls.1";
        (void)os::fs::write(mcppls::base::join_path(payload, "payload.json"), forkManifest.dump());
        const auto fork = eng::resolve_payload(eng::PayloadRequest { payload, "", "", "clangd" });
        expect(fork.clangdVersion == "23.1.0-mcppls.1" && fork.kitVersion == "23.1.0");
        expect(fork.kit == mcppls::base::join_path(payload, "kit"));
        (void)os::fs::write(mcppls::base::join_path(payload, "payload.json"), manifest.dump());

        // The payload's kit carries the libc++ its headers are: another version than the engine's is still its own.
        write_kit(mcppls::base::join_path(payload, "kit"), "22.1.8");
        expect(eng::resolve_payload(eng::PayloadRequest { payload, "", "", "clangd" }).kit == mcppls::base::join_path(payload, "kit"));
        // An explicit --kit is taken as given, for every target.
        const auto given = eng::resolve_payload(eng::PayloadRequest { payload, "", mcppls::base::join_path(payload, "kit"), "clangd" });
        expect(given.explicitKit && given.kit == mcppls::base::join_path(payload, "kit") && given.kits.size() == 1);
        std::filesystem::remove_all(payload, ignored);
    };

    "payload version 5: one payload, the entry of each platform"_test = [] {
        const Json manifest { { "payload-version", 5 }, { "server", Json { { "version", "0.0.20" }, { "path", "bin/mcppls.okb" } } },
                              { "kit", Json { { "name", "mcppls-kit-openkal" }, { "path", "kit" } } },
                              { "files", Json { { "kit/kit.json", Json { { "size", 2 }, { "sha256", std::string(64, 'a') } } } } },
                              { "platforms", Json {
                                  { "linux-x64", Json { { "runner", "libexec/okrun/linux-x86_64/okrun" },
                                                        { "engines", Json { { "clangd", Json { { "version", "23.1.0-mcppls.5" }, { "path", "clangd/linux-x64/bin/clangd" } } } } },
                                                        { "files", Json { { "clangd/linux-x64/bin/clangd", Json { { "size", 3 }, { "sha256", std::string(64, 'b') } } } } } } },
                                  { "win32-x64", Json { { "runner", "libexec/okrun/windows-x86_64/okrun.exe" },
                                                        { "engines", Json { { "clangd", Json { { "version", "23.1.0-mcppls.5" }, { "path", "clangd/win32-x64/bin/clangd.exe" } } } } } } } } } };
        const auto read = eng::read_payload_manifest(manifest.dump());
        expect(read.has_value()) << (read ? "" : read.error().message);
        if (!read) return;
        expect(read->version == 5 && read->serverPath == "bin/mcppls.okb" && read->kitPath == "kit" && read->entries.size() == 2);
        expect(read->files.contains("kit/kit.json")) << "the files every platform shares";
        const auto windows = eng::payload_entry(*read, "win32-x64");
        expect(windows.has_value() && windows->runner == "libexec/okrun/windows-x86_64/okrun.exe" && windows->clangd == "clangd/win32-x64/bin/clangd.exe");
        const auto linux = eng::payload_entry(*read, "linux-x64");
        expect(linux.has_value() && linux->files.contains("clangd/linux-x64/bin/clangd")) << "a platform's own integrity files are its entry's";
        const auto mac = eng::payload_entry(*read, "darwin-arm64");
        expect(!mac.has_value());
        if (!mac) expect(mac.error().find("darwin-arm64") != std::string::npos && mac.error().find("linux-x64, win32-x64") != std::string::npos) << mac.error();

        // Versions 1 to 4 are one platform's; a version this server does not read is refused.
        const auto legacy = eng::read_payload_manifest(Json { { "payload-version", 4 }, { "platform", "darwin-arm64" },
                                                              { "server", Json { { "path", "bin/mcppls.okb" }, { "runner", "bin/okrun" } } } }.dump());
        expect(legacy.has_value() && legacy->entries.size() == 1 && legacy->entries.front().platform == "darwin-arm64"
               && legacy->entries.front().runner == "bin/okrun");
        expect(!eng::read_payload_manifest(Json { { "payload-version", 6 } }.dump()).has_value());
        expect(!eng::read_payload_manifest(Json { { "payload-version", 5 } }.dump()).has_value()) << "version 5 needs platforms";
    };

    "a payload takes this platform's entry and refuses one without it"_test = [] {
        std::error_code ignored;
        const std::string payload { scratch("payload-v5") };
        const std::string exe { os::conventions().exe_suffix };
        const std::string platform { base::payload_platform() };
        const std::string own { std::format("clangd/{}/bin/clangd{}", platform, exe) };
        (void)std::filesystem::create_directories(base::parent_path(mcppls::base::join_path(payload, own)), ignored);
        (void)os::fs::write(mcppls::base::join_path(payload, own), "this platform's engine");
        write_kit(mcppls::base::join_path(payload, "kit"), "23.1.0");
        const std::string other { platform == "win32-x64" ? "linux-x64" : "win32-x64" };
        const std::string digest { mcppls::base::sha256_hex("this platform's engine") };
        const Json identity { { "engine-version", "23.1.0-mcppls.5" }, { "llvm-base-version", "23.1.0" },
                              { "llvm-commit", std::string(40, 'a') }, { "fork-commit", std::string(40, 'b') },
                              { "patch-series-sha256", std::string(64, 'c') }, { "platform", platform },
                              { "sha256", digest }, { "features", Json::array() } };
        Json manifest { { "payload-version", 5 }, { "server", Json { { "version", "0.0.20" }, { "path", "bin/mcppls.okb" } } },
                        { "kit", Json { { "name", "k" }, { "path", "kit" } } },
                        { "platforms", Json {
                            { platform, Json { { "runner", "libexec/okrun/here/okrun" },
                                               { "engines", Json { { "clangd", Json { { "version", "23.1.0-mcppls.5" }, { "path", own }, { "identity", identity } } } } },
                                               { "files", Json { { own, Json { { "size", 22 }, { "sha256", digest } } } } } } },
                            { other, Json { { "runner", "libexec/okrun/there/okrun" },
                                            { "engines", Json { { "clangd", Json { { "version", "23.1.0" }, { "path", "clangd/there/bin/clangd" } } } } } } } } } };
        (void)os::fs::write(mcppls::base::join_path(payload, "payload.json"), manifest.dump());
        const auto taken = eng::resolve_payload(eng::PayloadRequest { payload, "", "", "clangd" });
        expect(taken.clangd == mcppls::base::join_path(payload, own) && taken.identityProblem.empty()) << taken.clangd << taken.identityProblem;
        expect(taken.platforms.size() == 2);
        expect(taken.engineIdentity.has_value() && taken.files.contains(own)) << "this platform's identity and integrity files";
        expect(eng::verify_payload_integrity(taken, mcppls::base::join_path(payload, "integrity-cache.json")).empty());

        manifest["platforms"].erase(platform);
        (void)os::fs::write(mcppls::base::join_path(payload, "payload.json"), manifest.dump());
        const auto refused = eng::resolve_payload(eng::PayloadRequest { payload, "", "", "clangd" });
        expect(refused.identityProblem.find(platform) != std::string::npos && refused.identityProblem.find(other) != std::string::npos) << refused.identityProblem;
        expect(!refused.clangd.starts_with(payload)) << "no engine of another platform is taken: " << refused.clangd;
        const auto issues = eng::verify_payload_integrity(refused, "");
        expect(!issues.empty() && issues.front().reason.find("carries no engine") != std::string::npos);
        std::filesystem::remove_all(payload, ignored);
    };

    "the kit of a target: its own, else the host's, else the openkal kit, and the kit that would serve it"_test = [] {
        using Kits = std::vector<mcppls::spec::Kit>;
        const auto openkal = kit_of("openkal", "x86_64-unknown-none-elf", { "x86_64-openkal", "aarch64-openkal" });
        const auto linuxKit = kit_of("linux", "x86_64-unknown-linux-gnu");
        const auto mingw = kit_of("mingw", "x86_64-w64-mingw32");

        // Nothing installed: this host's target reads with the openkal kit of its architecture, and says what serves it.
        const auto offline = eng::choose_kit(Kits { openkal }, false, "", "x86_64-linux-gnu");
        expect(offline.kit.has_value() && offline.kit->target == "x86_64-unknown-none-elf" && offline.missingTarget == "x86_64-linux-gnu");
        if (offline.kit) expect(offline.kit->systemIncludeDirectories.front().ends_with("x86_64-openkal") && offline.kit->variants.empty());
        const auto arm = eng::choose_kit(Kits { openkal }, false, "", "aarch64-unknown-linux-gnu");
        expect(arm.kit && arm.kit->target == "aarch64-unknown-none-elf" && arm.missingTarget == "aarch64-linux-gnu");
        const auto mac = eng::choose_kit(Kits { openkal }, false, "", "arm64-apple-darwin");
        expect(mac.kit && mac.kit->target == "aarch64-unknown-none-elf" && mac.missingTarget == "arm64-apple-darwin");

        // The target's own kit, once installed.
        const auto installed = eng::choose_kit(Kits { openkal, linuxKit }, false, "", "x86_64-linux-gnu");
        expect(installed.kit && installed.kit->name == "linux" && installed.missingTarget.empty());
        // An openkal target reads with its variant whatever the host has.
        const auto image = eng::choose_kit(Kits { openkal, linuxKit }, false, "aarch64-openkal", "x86_64-linux-gnu");
        expect(image.kit && image.kit->target == "aarch64-unknown-none-elf" && image.missingTarget.empty() && image.target == "aarch64-openkal");
        // Another system's target: its kit when installed, else the host's, naming the one to install.
        const auto windows = eng::choose_kit(Kits { openkal, linuxKit }, false, "x86_64-windows-gnu", "x86_64-linux-gnu");
        expect(windows.kit && windows.kit->name == "linux" && windows.missingTarget == "x86_64-w64-mingw32");
        const auto msvc = eng::choose_kit(Kits { openkal, mingw }, false, "x86_64-pc-windows-msvc", "x86_64-linux-gnu");
        expect(msvc.kit && msvc.kit->name == "mingw" && msvc.missingTarget.empty());
        // A target no kit is published for keeps the host's, and names none.
        const auto i686 = eng::choose_kit(Kits { openkal, linuxKit }, false, "i686-unknown-linux-gnu", "x86_64-linux-gnu");
        expect(i686.kit && i686.kit->name == "linux" && i686.missingTarget.empty());
        // --kit serves every target.
        const auto given = eng::choose_kit(Kits { mingw, openkal }, true, "x86_64-openkal", "x86_64-linux-gnu");
        expect(given.kit && given.kit->name == "mingw" && given.missingTarget.empty());
        // No kit at all.
        const auto none = eng::choose_kit(Kits {}, false, "", "x86_64-linux-gnu");
        expect(!none.kit && none.missingTarget == "x86_64-linux-gnu");
    };

    "targets are named as their kits name them"_test = [] {
        for (const auto& [triple, name] : std::vector<std::pair<std::string_view, std::string_view>> {
                 { "x86_64-unknown-linux-gnu", "x86_64-linux-gnu" }, { "x86_64-linux-gnu", "x86_64-linux-gnu" },
                 { "aarch64-linux-gnu", "aarch64-linux-gnu" }, { "arm64-unknown-linux-gnu", "aarch64-linux-gnu" },
                 { "x86_64-linux-musl", "x86_64-linux-musl" }, { "x86_64-pc-windows-msvc", "x86_64-w64-mingw32" },
                 { "x86_64-windows-gnu", "x86_64-w64-mingw32" }, { "x86_64-w64-mingw32", "x86_64-w64-mingw32" },
                 { "arm64-apple-darwin", "arm64-apple-darwin" }, { "aarch64-macos", "arm64-apple-darwin" },
                 { "arm64-apple-macos14.0", "arm64-apple-darwin" }, { "x86_64-openkal", "x86_64-openkal" },
                 { "aarch64-openkal", "aarch64-openkal" }, { "i686-unknown-linux-gnu", "i686-linux-gnu" }, { "wasm32-emscripten", "wasm32-emscripten" } }) {
            expect(eng::kit_target_name(triple) == name) << triple << " -> " << eng::kit_target_name(triple);
        }
        const auto published = eng::published_kit_targets();
        expect(std::ranges::find(published, eng::host_kit_target()) != published.end() || eng::host_kit_target().starts_with("x86_64-apple"))
            << "this host's target has a published kit: " << eng::host_kit_target();
    };

    "identified payloads validate their engine while explicit external engines inherit no capabilities"_test = [] {
        std::error_code ignored;
        const std::string payload { scratch("payload-v4") };
        const std::string relative { "clangd/bin/clangd" + std::string { os::conventions().exe_suffix } };
        (void)std::filesystem::create_directories(mcppls::base::join_path(payload, "clangd/bin"), ignored);
        (void)os::fs::write(mcppls::base::join_path(payload, relative), "fixture engine");
        write_kit(mcppls::base::join_path(payload, "kit"), "23.1.0");
        const std::string digest { mcppls::base::sha256_hex("fixture engine") };
        Json identity { { "engine-version", "23.1.0-mcppls.1" }, { "llvm-base-version", "23.1.0" },
                        { "llvm-commit", std::string(40, 'a') }, { "fork-commit", std::string(40, 'b') },
                        { "patch-series-sha256", std::string(64, 'c') }, { "platform", base::payload_platform() },
                        { "sha256", digest }, { "features", Json::array({ "semantic-tokens-range" }) } };
        Json manifest { { "payload-version", 4 }, { "platform", base::payload_platform() },
                        { "engines", Json { { "clangd", Json { { "version", "23.1.0-mcppls.1" }, { "path", relative }, { "identity", identity } } } } },
                        { "files", Json { { relative, Json { { "size", 14 }, { "sha256", digest } } } } } };
        const auto write = [&] { (void)os::fs::write(mcppls::base::join_path(payload, "payload.json"), manifest.dump()); };
        write();
        const auto matching = eng::resolve_payload(eng::PayloadRequest { payload, "", "", "clangd" });
        expect(matching.engineIdentity.has_value() && matching.identityProblem.empty());
        expect(matching.kit == mcppls::base::join_path(payload, "kit"));
        expect(eng::verify_payload_integrity(matching, mcppls::base::join_path(payload, "integrity-cache.json")).empty());
        manifest["engines"]["clangd"]["identity"]["sha256"] = std::string(64, 'e');
        write();
        const auto forged = eng::resolve_payload(eng::PayloadRequest { payload, "", "", "clangd" });
        expect(!forged.identityProblem.empty());
        expect(!eng::verify_payload_integrity(forged, "").empty());
        manifest["engines"]["clangd"].erase("identity");
        write();
        const auto missing = eng::resolve_payload(eng::PayloadRequest { payload, "", "", "clangd" });
        expect(!missing.identityProblem.empty());
        const auto external = eng::resolve_payload(eng::PayloadRequest { payload, mcppls::base::join_path(payload, "external-missing"), "", "clangd" });
        expect(!external.engineIdentity && external.identityProblem.empty());
        std::filesystem::remove_all(payload, ignored);
    };

    "a payload that declares no clangd still finds one in the conventional place"_test = [] {
        std::error_code ignored;
        const std::string payload { scratch("payload-conventional") };
        const std::string clangd { mcppls::base::join_path(payload, std::string { "clangd/bin/clangd" } + std::string { os::conventions().exe_suffix }) };
        (void)std::filesystem::create_directories(mcppls::base::join_path(payload, "clangd/bin"), ignored);
        (void)os::fs::write(clangd, "not really clangd");
        (void)os::fs::write(mcppls::base::join_path(payload, "payload.json"), Json { { "payload-version", 3 }, { "platform", base::payload_platform() } }.dump());
        const auto resolved = eng::resolve_payload(eng::PayloadRequest { payload, "", "", "none" });
        expect(resolved.clangd == clangd) << resolved.clangd;
        std::filesystem::remove_all(payload, ignored);
    };

    // `mcppls status` (S3 section 8): the instances still alive, each read from its own directory, and
    // the best state among them; one whose process is gone is not counted.
    "status names the live instances of a workspace and their best state"_test = [] {
        std::error_code ignored;
        const std::string cache { scratch("status") };
        const std::string root { base::normalize_path(base::join_path(cache, "project")) };
        const std::string workspace { base::join_path(cache, base::join_path("workspaces", mcppls::project::workspace_key(root))) };
        (void)std::filesystem::create_directories(base::join_path(workspace, "instances/dead"), ignored);
        const auto self = os::process::self();
        expect(fatal(self.has_value()));
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const Json alive { { "pid", static_cast<std::int64_t>(self->id) }, { "started", std::format("ok:{}", self->started_ns) }, { "at", now } };
        (void)os::fs::write(base::join_path(workspace, "instance.json"), alive.dump());
        const Json status { { "schema", 1 }, { "pid", static_cast<std::int64_t>(self->id) },
                            { "mcpp", Json { { "version", "2026.10.31.1" }, { "program", "/bin/mcpp" } } },
                            { "status", Json { { "state", "ready" }, { "engine", Json { { "name", "clangd" }, { "version", "23" } } },
                                               { "issues", Json::array({ Json { { "code", "kit-missing" }, { "message", "m" } } }) },
                                               { "about", Json { { "startedAt", "2026-10-31T00:00:00Z" }, { "editor", Json { { "name", "vscode" } } },
                                                                 { "engine", Json { { "restarts", 2 } } } } } } } };
        (void)os::fs::write(base::join_path(workspace, "status.json"), status.dump());
        // An instance whose heartbeat stopped long ago.
        const Json dead { { "pid", 999999 }, { "started", "ok:1" }, { "at", now - 3600000 } };
        (void)os::fs::write(base::join_path(workspace, "instances/dead/instance.json"), dead.dump());
        (void)os::fs::write(base::join_path(workspace, "instances/dead/status.json"),
                            Json { { "status", Json { { "state", "degraded" } } } }.dump());

        const Json document = mcppls::cli::status_document(root, cache);
        expect(document.value("schema", 0) == 1);
        expect(document["instances"].size() == 1u) << document.dump();
        expect(document["instances"][0].value("editor", std::string {}) == "vscode");
        const Json& w = document["workspace"];
        expect(w.value("state", std::string {}) == "ready") << document.dump();
        expect(w["engine"].value("restarts", 0) == 2);
        expect(w["mcpp"].value("version", std::string {}) == "2026.10.31.1");
        expect(w["problems"].size() == 1u && w["problems"][0].value("code", std::string {}) == "kit-missing");
        expect(document["actions"].is_array() && !document["actions"].empty());
        // The actions under each editor's id (S3-8-5, S3-8-7): an editor without an id has none.
        const auto action = [&](std::string_view id) -> Json {
            for (const auto& a : document["actions"]) if (a.value("id", std::string {}) == id) return a;
            return Json {};
        };
        expect(action("restart").value("jetbrains", std::string {}) == "community.mcpp.mcppls.RestartServer");
        expect(action("restart").value("nvim", std::string {}) == "McpplsRestart");
        expect(action("openLogDirectory")["vscodeArguments"] == Json::array({ "logs" }));
        expect(!action("restartEngine").contains("jetbrains"));
        expect(action("turnOffOtherCppFeatures").value("jetbrains", std::string {}) == "community.mcpp.mcppls.UseMcpplsOnly");
        // Installing the kit is offered exactly while the kit is missing, for its target.
        const Json kit = w.value("kit", Json::object());
        expect(action("installKit").is_null() == (kit.value("state", std::string {}) != "missing")) << document.dump();

        // Every instance gone: the workspace was served and is not now.
        std::filesystem::remove(base::join_path(workspace, "instance.json"), ignored);
        expect(mcppls::cli::status_document(root, cache)["workspace"].value("state", std::string {}) == "stopped");
        std::filesystem::remove_all(cache, ignored);
        expect(mcppls::cli::status_document(root, cache)["workspace"].value("state", std::string {}) == "none");
    };

    return report();
}
