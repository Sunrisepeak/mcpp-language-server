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
};

std::optional<Lease> read_lease(std::string_view workspaceDirectory) {
    auto text = platform::fs::read_file(lease_path(workspaceDirectory));
    if (!text) return std::nullopt;
    const Json document = Json::parse(*text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) return std::nullopt;
    return Lease { document.value("token", std::string {}), document.value("heartbeat", std::int64_t { 0 }) };
}

void write_lease(std::string_view workspaceDirectory, std::string_view token, std::chrono::system_clock::time_point now) {
    const Json document { { "token", std::string { token } }, { "heartbeat", milliseconds(now) }, { "version", std::string { base::VERSION } } };
    (void)platform::fs::write_file_atomic(lease_path(workspaceDirectory), document.dump());
}

} // namespace

WorkspaceLease WorkspaceLease::acquire(std::string_view workspaceDirectory, std::chrono::system_clock::time_point now) {
    WorkspaceLease lease;
    lease.workspaceDirectory_ = std::string { workspaceDirectory };
    lease.token_ = new_token();
    (void)platform::fs::create_directories(workspaceDirectory);
    const auto current = read_lease(workspaceDirectory);
    const bool live { current && !current->token.empty() && milliseconds(now) - current->heartbeat < std::chrono::milliseconds { LEASE_EXPIRY }.count() };
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
