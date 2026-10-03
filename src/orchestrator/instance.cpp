module mcppls.orchestrator.instance;

import std;
import nlohmann.json;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.sha256;
import mcppls.base.version;
import mcppls.platform.fs;
import mcppls.platform.process;

namespace mcppls::orchestrator {

using Json = nlohmann::json;

namespace {

std::string lease_path(std::string_view workspaceDirectory) { return base::join_path(workspaceDirectory, "owner.lease"); }

// C-9 (plan 2026-10-03): every instance describes itself in the directory it works in -- the owner
// in the workspace directory itself, a guest beside the others under `instances/<token>/`. The
// heartbeat `at` is what the reapers read (a live guest is protected by its own file, not by the
// owner's lease); `pid`/`started` are what X-6 needs to tell a dead owner from a reused pid, and
// are simply absent where the platform cannot say (an older file stays as readable as ever).
std::string instance_path(bool shared, std::string_view workspaceDirectory, std::string_view directory) {
    // The guest's file lives inside its own directory; the owner's beside the lease it renews.
    return base::join_path(shared ? directory : workspaceDirectory, "instance.json");
}

std::int64_t milliseconds(std::chrono::system_clock::time_point at) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
}

// Unique enough to tell two instances apart: two clocks, this object's address and the thread.
std::string new_token() {
    const auto steady = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto wall = std::chrono::system_clock::now().time_since_epoch().count();
    int local { 0 };
    const std::string seed { std::format("{}:{}:{}:{}", steady, wall, static_cast<void*>(&local), std::hash<std::thread::id> {}(std::this_thread::get_id())) };
    return base::sha256_hex(seed).substr(0, 16);
}

struct Lease {
    std::string token;
    std::int64_t heartbeat { 0 };
    std::int64_t pid { 0 };           // the owner's process, where it could say (C-5, X-6)
    std::string started;              // and when that process started, so a reused pid is not taken for it
};

// Whether the process a lease names is gone: false when it cannot be told (no pid recorded, or the
// platform cannot answer -- the heartbeat alone decides there, as before). X-6 makes the check real
// on Windows, where before it could never say anything.
bool owner_gone(const Lease& lease) {
    if (lease.pid <= 0 || lease.started.empty()) return false;
    const auto owner = platform::process_identity(lease.pid);
    return !owner || owner->started != lease.started;
}

std::optional<Lease> read_lease(std::string_view workspaceDirectory) {
    auto text = platform::fs::read_file(lease_path(workspaceDirectory));
    if (!text) return std::nullopt;
    const Json document = Json::parse(*text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) return std::nullopt;
    return Lease { document.value("token", std::string {}), document.value("heartbeat", std::int64_t { 0 }),
                   document.value("pid", std::int64_t { 0 }), document.value("started", std::string {}) };
}

void write_lease(std::string_view workspaceDirectory, std::string_view token, std::chrono::system_clock::time_point now) {
    Json document { { "token", std::string { token } }, { "heartbeat", milliseconds(now) }, { "version", std::string { base::VERSION } } };
    if (const auto self = platform::process_self()) {
        document["pid"] = self->pid;
        document["started"] = self->started;
    }
    (void)platform::fs::write_file_atomic(lease_path(workspaceDirectory), document.dump());
}

void write_instance(bool shared, std::string_view workspaceDirectory, std::string_view directory, std::string_view token,
                    std::string_view root, std::chrono::system_clock::time_point now) {
    Json document { { "token", std::string { token } },
                    { "version", std::string { base::VERSION } },
                    { "root", std::string { root } },
                    { "at", milliseconds(now) },
                    { "shared", shared } };
    if (const auto self = platform::process_self()) {
        document["pid"] = self->pid;
        document["started"] = self->started;
    }
    (void)platform::fs::write_file_atomic(instance_path(shared, workspaceDirectory, directory), document.dump());
}

} // namespace

WorkspaceLease WorkspaceLease::acquire(std::string_view workspaceDirectory, std::chrono::system_clock::time_point now,
                                       std::string_view root) {
    WorkspaceLease lease;
    lease.workspaceDirectory_ = std::string { workspaceDirectory };
    lease.root_ = std::string { root };
    lease.token_ = new_token();
    (void)platform::fs::create_directories(workspaceDirectory);
    const auto current = read_lease(workspaceDirectory);
    // C-5: a server that crashed left its lease fresh for up to LEASE_EXPIRY, and the editor restarts a crashed server at
    // once -- so the restarted one took itself for a second instance and started cold in a private directory (66 s
    // instead of 3.6 s on mcpp). A lease whose owner is gone, or whose pid another process has since, is not live. On
    // Windows the same check finally answers (X-6): before it, a crash there always read as a second instance.
    const bool live { current && !current->token.empty() && milliseconds(now) - current->heartbeat < std::chrono::milliseconds { LEASE_EXPIRY }.count()
                      && !owner_gone(*current) };
    if (!live) {
        write_lease(workspaceDirectory, lease.token_, now);
        // Two instances starting at the same moment both write; the one whose token stayed owns it.
        const auto confirmed = read_lease(workspaceDirectory);
        lease.shared_ = !confirmed || confirmed->token != lease.token_;
    } else {
        lease.shared_ = true;
    }
    if (lease.shared_) {
        lease.directory_ = base::join_path(workspaceDirectory, base::join_path("instances", lease.token_));
        (void)platform::fs::create_directories(lease.directory_);
        base::log::info("another instance serves {}; this one uses {}", workspaceDirectory, lease.directory_);
    } else {
        lease.directory_ = std::string { workspaceDirectory };
    }
    write_instance(lease.shared_, workspaceDirectory, lease.directory_, lease.token_, root, now);
    return lease;
}

void WorkspaceLease::renew(std::chrono::system_clock::time_point now) {
    if (released_) return;
    // The lease is the owner's alone; the instance file is everybody's heartbeat, and the reapers
    // (C-9) read it to tell a live guest from a dead one.
    if (!shared_) write_lease(workspaceDirectory_, token_, now);
    write_instance(shared_, workspaceDirectory_, directory_, token_, root_, now);
}

void WorkspaceLease::release() {
    if (released_) return;
    released_ = true;
    if (shared_) {
        platform::fs::remove_all(directory_);
        return;
    }
    // Only a lease that is still this instance's own is removed, and the self-description with it.
    if (const auto current = read_lease(workspaceDirectory_); current && current->token == token_) {
        platform::fs::remove_all(lease_path(workspaceDirectory_));
        (void)platform::fs::remove(instance_path(shared_, workspaceDirectory_, directory_));
    }
}

} // namespace mcppls::orchestrator
