// Instances sharing a workspace (overall design 6.3) and choosing the kit that matches the core engine (5.6).
import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.base.path;
import mcppls.base.sha256;
import mcppls.engine.payload;
import mcppls.orchestrator.instance;
import mcpplibs.os;
import mcppls.base.payload;

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

void write_kit(std::string_view root, std::string_view version) {
    std::error_code ignored;
    (void)std::filesystem::create_directories(root, ignored);
    const Json kit { { "kit-version", 1 }, { "name", std::format("mcppls-kit-libcxx-{}", version) }, { "target", "x86_64-unknown-linux-gnu" },
                     { "stdlib", Json { { "name", "libc++" }, { "version", std::string { version } }, { "module-metadata", "lib/libc++.modules.json" } } },
                     { "system-include-directories", Json::array({ "include/c++/v1" }) }, { "licenses", Json::array() } };
    (void)os::fs::write(mcppls::base::join_path(root, "kit.json"), kit.dump());
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

    "a payload's kit is taken only when its libc++ is the core engine's version"_test = [] {
        std::error_code ignored;
        const std::string payload { scratch("payload-v3") };
        (void)std::filesystem::create_directories(mcppls::base::join_path(payload, "clangd/bin"), ignored);
        (void)os::fs::write(mcppls::base::join_path(payload, "clangd/bin/clangd"), "not really clangd");
        write_kit(mcppls::base::join_path(payload, "kit"), "23.1.0");
        const Json manifest { { "payload-version", 3 }, { "platform", "linux-x64" },
                              { "engines", Json { { "clangd", Json { { "version", "23.1.0" }, { "path", "clangd/bin/clangd" },
                                                                     { "kit", Json { { "name", "mcppls-kit-libcxx-23.1.0" }, { "path", "kit" } } } } } } } };
        (void)os::fs::write(mcppls::base::join_path(payload, "payload.json"), manifest.dump());

        const auto matching = eng::resolve_payload(eng::PayloadRequest { payload, "", "", "clangd" });
        expect(matching.clangd == mcppls::base::join_path(payload, "clangd/bin/clangd") && matching.clangdVersion == "23.1.0") << matching.clangd;
        expect(matching.kit == mcppls::base::join_path(payload, "kit") && matching.kitNotice.empty()) << matching.kit;

        // Retain the full maintained-engine identity while selecting its base kit.
        Json forkManifest = manifest;
        forkManifest["engines"]["clangd"]["version"] = "23.1.0-mcppls.1";
        (void)os::fs::write(mcppls::base::join_path(payload, "payload.json"), forkManifest.dump());
        const auto fork = eng::resolve_payload(eng::PayloadRequest { payload, "", "", "clangd" });
        expect(fork.clangdVersion == "23.1.0-mcppls.1");
        expect(fork.kit == mcppls::base::join_path(payload, "kit") && fork.kitNotice.empty());
        (void)os::fs::write(mcppls::base::join_path(payload, "payload.json"), manifest.dump());

        // A kit built for another clangd is not used, and the status says why (S4-4-5).
        write_kit(mcppls::base::join_path(payload, "kit"), "22.1.8");
        const auto mismatched = eng::resolve_payload(eng::PayloadRequest { payload, "", "", "clangd" });
        expect(mismatched.kit.empty()) << mismatched.kit;
        expect(mismatched.kitNotice.find("22.1.8") != std::string::npos && mismatched.kitNotice.find("23.1.0") != std::string::npos) << mismatched.kitNotice;
        // Without a core engine any kit does, and an explicit --kit is taken as given.
        expect(eng::resolve_payload(eng::PayloadRequest { payload, "", "", "none" }).kit == mcppls::base::join_path(payload, "kit"));
        expect(eng::resolve_payload(eng::PayloadRequest { payload, "", mcppls::base::join_path(payload, "kit"), "clangd" }).kit == mcppls::base::join_path(payload, "kit"));
        std::filesystem::remove_all(payload, ignored);
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
        (void)os::fs::write(mcppls::base::join_path(payload, "payload.json"), Json { { "payload-version", 3 }, { "platform", "linux-x64" } }.dump());
        const auto resolved = eng::resolve_payload(eng::PayloadRequest { payload, "", "", "none" });
        expect(resolved.clangd == clangd) << resolved.clangd;
        std::filesystem::remove_all(payload, ignored);
    };

    return report();
}
