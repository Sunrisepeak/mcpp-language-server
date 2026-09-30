module mcppls.orchestrator.instance;

import std;
import nlohmann.json;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.sha256;
import mcppls.base.version;
import mcppls.platform.fs;

namespace mcppls::orchestrator {

namespace {

using Json = nlohmann::json;

std::string lease_path(std::string_view workspaceDirectory) { return base::join_path(workspaceDirectory, "owner.lease"); }

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
    std::int64_t pid { 0 };           // the owner's process, where it could say (C-5)
    std::string started;              // and when that process started, so a reused pid is not taken for it
};

// C-5 (plan 2026-09-30): a process as /proc knows it -- its id and its start time (field 22 of stat, in clock ticks
// since boot). Nothing where there is no /proc: the heartbeat alone decides there, as before.
struct ProcessIdentity {
    std::int64_t pid { 0 };
    std::string started;
};

std::optional<ProcessIdentity> identity_of(std::string_view statPath) {
    const auto stat = platform::fs::read_file(statPath);
    if (!stat) return std::nullopt;
    const auto open = stat->find('(');
    const auto close = stat->rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) return std::nullopt;
    ProcessIdentity identity;
    const std::string_view head { std::string_view { *stat }.substr(0, open) };
    const std::string_view digits { head.substr(0, head.find(' ')) };
    if (std::from_chars(digits.data(), digits.data() + digits.size(), identity.pid).ec != std::errc {}) return std::nullopt;
    // After ") ": state is field 3, starttime field 22, so the 20th of what follows.
    std::size_t field { 0 };
    std::size_t at { close + 2 };
    while (at < stat->size()) {
        const auto end = stat->find(' ', at);
        const std::string_view value { std::string_view { *stat }.substr(at, end == std::string::npos ? std::string::npos : end - at) };
        if (++field == 20) {
            identity.started = std::string { value };
            return identity;
        }
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return std::nullopt;
}

std::optional<ProcessIdentity> this_process() { return identity_of("/proc/self/stat"); }

// Whether the process a lease names is gone: false when it cannot be told (no pid recorded, no /proc).
bool owner_gone(const Lease& lease) {
    if (lease.pid <= 0 || lease.started.empty() || !platform::fs::exists("/proc/self/stat")) return false;
    const auto owner = identity_of(std::format("/proc/{}/stat", lease.pid));
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
    if (const auto self = this_process()) {
        document["pid"] = self->pid;
        document["started"] = self->started;
    }
    (void)platform::fs::write_file_atomic(lease_path(workspaceDirectory), document.dump());
}

} // namespace

WorkspaceLease WorkspaceLease::acquire(std::string_view workspaceDirectory, std::chrono::system_clock::time_point now) {
    WorkspaceLease lease;
    lease.workspaceDirectory_ = std::string { workspaceDirectory };
    lease.token_ = new_token();
    (void)platform::fs::create_directories(workspaceDirectory);
    const auto current = read_lease(workspaceDirectory);
    // C-5: a server that crashed left its lease fresh for up to LEASE_EXPIRY, and the editor restarts a crashed server at
    // once -- so the restarted one took itself for a second instance and started cold in a private directory (66 s
    // instead of 3.6 s on mcpp). A lease whose owner is gone, or whose pid another process has since, is not live.
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
    return lease;
}

void WorkspaceLease::renew(std::chrono::system_clock::time_point now) {
    if (shared_ || released_) return;
    write_lease(workspaceDirectory_, token_, now);
}

void WorkspaceLease::release() {
    if (released_) return;
    released_ = true;
    if (shared_) {
        platform::fs::remove_all(directory_);
        return;
    }
    // Only a lease that is still this instance's own is removed.
    if (const auto current = read_lease(workspaceDirectory_); current && current->token == token_) platform::fs::remove_all(lease_path(workspaceDirectory_));
}

} // namespace mcppls::orchestrator
