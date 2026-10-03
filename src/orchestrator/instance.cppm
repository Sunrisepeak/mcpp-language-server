// Instances sharing a workspace (overall design 6.3, first step). The first instance serving a
// workspace owns its cache directory under a lease it renews; an instance that finds a live lease
// (an editor's server while an agent starts its own) works in a private directory beside it,
// starting cold, instead of rewriting the owner's engine database and prepared modules. A lease
// whose owner stopped renewing it is taken over.
export module mcppls.orchestrator.instance;

import std;

export namespace mcppls::orchestrator {

inline constexpr std::chrono::seconds LEASE_RENEWAL { 10 };
inline constexpr std::chrono::seconds LEASE_EXPIRY { 30 };

class WorkspaceLease {
public:
    // `workspaceDirectory` is <cache>/workspaces/<key>; `now` is wall-clock time (a lease is read by
    // other processes, so steady clocks do not compare). `root` is the workspace root the instance
    // serves, recorded in its own `instance.json` (C-9) so a sweep's report can name what it sees.
    static WorkspaceLease acquire(std::string_view workspaceDirectory, std::chrono::system_clock::time_point now,
                                  std::string_view root = {});

    const std::string& directory() const { return directory_; }   // the cache directory this instance uses
    const std::string& token() const { return token_; }           // this instance's own; it marks its report entry (C-9)
    // The owner's cache directory, <cache>/workspaces/<key>: a guest may read what the owner wrote there (its
    // cached project model, P-1 plan 0.0.9), never write to it.
    const std::string& workspace_directory() const { return workspaceDirectory_; }
    bool shared() const { return shared_; }                        // another live instance owns the workspace directory
    void renew(std::chrono::system_clock::time_point now);         // the owner's lease heartbeat, and every instance's `instance.json` heartbeat
    void release();                                                // the owner drops its lease; a guest removes its directory

private:
    std::string workspaceDirectory_;
    std::string root_;
    std::string directory_;
    std::string token_;
    bool shared_ { false };
    bool released_ { false };
};

} // namespace mcppls::orchestrator
