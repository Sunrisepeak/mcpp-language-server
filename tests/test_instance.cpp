// Instances sharing a workspace (overall design 6.3) and choosing the kit that matches the core engine (5.6).
import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.os;
import mcppls.base.path;
import mcppls.platform.dirs;
import mcppls.platform.fs;
import mcppls.engine.payload;
import mcppls.orchestrator.instance;

using Json = nlohmann::json;
namespace fs = mcppls::platform::fs;
namespace orch = mcppls::orchestrator;
namespace eng = mcppls::engine;

namespace {

std::string scratch(std::string_view name) {
    const std::string root { mcppls::base::join_path(mcppls::platform::dirs::temp_directory(),
        std::format("mcppls-test-{}-{}", name, std::chrono::steady_clock::now().time_since_epoch().count())) };
    (void)fs::create_directories(root);
    return root;
}

void write_kit(std::string_view root, std::string_view version) {
    (void)fs::create_directories(root);
    const Json kit { { "kit-version", 1 }, { "name", std::format("mcppls-kit-libcxx-{}", version) }, { "target", "x86_64-unknown-linux-gnu" },
                     { "stdlib", Json { { "name", "libc++" }, { "version", std::string { version } }, { "module-metadata", "lib/libc++.modules.json" } } },
                     { "system-include-directories", Json::array({ "include/c++/v1" }) }, { "licenses", Json::array() } };
    (void)fs::write_file(mcppls::base::join_path(root, "kit.json"), kit.dump());
}

} // namespace

int main() {
    using namespace mcppls::testing;

    "the first instance owns a workspace and a second works in a private directory"_test = [] {
        const std::string workspace { scratch("lease") };
        const auto now = std::chrono::system_clock::now();
        auto owner = orch::WorkspaceLease::acquire(workspace, now);
        expect(!owner.shared() && owner.directory() == workspace);
        auto guest = orch::WorkspaceLease::acquire(workspace, now + std::chrono::seconds { 5 });
        expect(guest.shared()) << "the fresh lease of a live owner is shared";
        expect(guest.directory() != workspace) << guest.directory();
        expect(fs::is_directory(guest.directory())) << guest.directory();
        const std::string guestDirectory { guest.directory() };
        guest.release();
        expect(!fs::exists(guestDirectory)) << "a guest removes its private directory";

        // An owner that stopped renewing its lease is taken over; one that renews keeps it.
        owner.renew(now + std::chrono::seconds { 20 });
        expect(orch::WorkspaceLease::acquire(workspace, now + std::chrono::seconds { 45 }).shared()) << "renewed 25 seconds ago";
        auto successor = orch::WorkspaceLease::acquire(workspace, now + std::chrono::seconds { 60 });
        expect(!successor.shared()) << "40 seconds without renewal";
        owner.release();
        expect(fs::exists(mcppls::base::join_path(workspace, "owner.lease"))) << "a stale owner does not remove its successor's lease";
        successor.release();
        expect(!fs::exists(mcppls::base::join_path(workspace, "owner.lease")));
        fs::remove_all(workspace);
    };

    // C-5 (plan 2026-09-30): a server that crashed left its lease fresh, the editor restarted it at once, and the new one
    // took itself for a second instance and started cold in a private directory. A lease names its process now.
    "a fresh lease whose process is gone, or whose pid another process has, is taken over at once"_test = [] {
        if constexpr (mcppls::os::FAMILY != mcppls::os::Family::linux) return;   // the heartbeat alone decides elsewhere
        const std::string workspace { scratch("lease-owner") };
        const auto now = std::chrono::system_clock::now();
        const auto heartbeat = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        const std::string lease { mcppls::base::join_path(workspace, "owner.lease") };
        (void)fs::write_file(lease, Json { { "token", "crashed" }, { "heartbeat", heartbeat }, { "pid", 999999999 }, { "started", "12345" } }.dump());
        auto restarted = orch::WorkspaceLease::acquire(workspace, now + std::chrono::seconds { 2 });
        expect(!restarted.shared() && restarted.directory() == workspace) << "the owner's process is gone";
        restarted.release();

        const auto self = fs::read_file("/proc/self/stat");
        expect(fatal(self.has_value()));
        const std::int64_t pid { std::stoll(self->substr(0, self->find(' '))) };
        (void)fs::write_file(lease, Json { { "token", "reused" }, { "heartbeat", heartbeat }, { "pid", pid }, { "started", "1" } }.dump());
        auto reused = orch::WorkspaceLease::acquire(workspace, now + std::chrono::seconds { 2 });
        expect(!reused.shared()) << "the pid is another process's now";
        // The live owner itself: a second instance still works in a private directory.
        auto guest = orch::WorkspaceLease::acquire(workspace, now + std::chrono::seconds { 3 });
        expect(guest.shared()) << "the owner is this very process, alive";
        guest.release();
        reused.release();
        fs::remove_all(workspace);
    };

    "a payload's kit is taken only when its libc++ is the core engine's version"_test = [] {
        const std::string payload { scratch("payload-v3") };
        (void)fs::create_directories(mcppls::base::join_path(payload, "clangd/bin"));
        (void)fs::write_file(mcppls::base::join_path(payload, "clangd/bin/clangd"), "not really clangd");
        write_kit(mcppls::base::join_path(payload, "kit"), "23.1.0");
        const Json manifest { { "payload-version", 3 }, { "platform", "linux-x64" },
                              { "engines", Json { { "clangd", Json { { "version", "23.1.0" }, { "path", "clangd/bin/clangd" },
                                                                     { "kit", Json { { "name", "mcppls-kit-libcxx-23.1.0" }, { "path", "kit" } } } } } } } };
        (void)fs::write_file(mcppls::base::join_path(payload, "payload.json"), manifest.dump());

        const auto matching = eng::resolve_payload(eng::PayloadRequest { payload, "", "", "clangd" });
        expect(matching.clangd == mcppls::base::join_path(payload, "clangd/bin/clangd") && matching.clangdVersion == "23.1.0") << matching.clangd;
        expect(matching.kit == mcppls::base::join_path(payload, "kit") && matching.kitNotice.empty()) << matching.kit;

        // Retain the full maintained-engine identity while selecting its base kit.
        Json forkManifest = manifest;
        forkManifest["engines"]["clangd"]["version"] = "23.1.0-mcppls.1";
        (void)fs::write_file(mcppls::base::join_path(payload, "payload.json"), forkManifest.dump());
        const auto fork = eng::resolve_payload(eng::PayloadRequest { payload, "", "", "clangd" });
        expect(fork.clangdVersion == "23.1.0-mcppls.1");
        expect(fork.kit == mcppls::base::join_path(payload, "kit") && fork.kitNotice.empty());
        (void)fs::write_file(mcppls::base::join_path(payload, "payload.json"), manifest.dump());

        // A kit built for another clangd is not used, and the status says why (S4-4-5).
        write_kit(mcppls::base::join_path(payload, "kit"), "22.1.8");
        const auto mismatched = eng::resolve_payload(eng::PayloadRequest { payload, "", "", "clangd" });
        expect(mismatched.kit.empty()) << mismatched.kit;
        expect(mismatched.kitNotice.find("22.1.8") != std::string::npos && mismatched.kitNotice.find("23.1.0") != std::string::npos) << mismatched.kitNotice;
        // Without a core engine any kit does, and an explicit --kit is taken as given.
        expect(eng::resolve_payload(eng::PayloadRequest { payload, "", "", "none" }).kit == mcppls::base::join_path(payload, "kit"));
        expect(eng::resolve_payload(eng::PayloadRequest { payload, "", mcppls::base::join_path(payload, "kit"), "clangd" }).kit == mcppls::base::join_path(payload, "kit"));
        fs::remove_all(payload);
    };

    "a payload that declares no clangd still finds one in the conventional place"_test = [] {
        const std::string payload { scratch("payload-conventional") };
        const std::string clangd { mcppls::base::join_path(payload, std::string { "clangd/bin/clangd" } + std::string { mcppls::os::EXECUTABLE_SUFFIX }) };
        (void)fs::create_directories(mcppls::base::join_path(payload, "clangd/bin"));
        (void)fs::write_file(clangd, "not really clangd");
        (void)fs::write_file(mcppls::base::join_path(payload, "payload.json"), Json { { "payload-version", 3 }, { "platform", "linux-x64" } }.dump());
        const auto resolved = eng::resolve_payload(eng::PayloadRequest { payload, "", "", "none" });
        expect(resolved.clangd == clangd) << resolved.clangd;
        fs::remove_all(payload);
    };

    return report();
}
