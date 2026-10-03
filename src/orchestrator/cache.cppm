// The cache mcppls owns and clangd uses (0.0.10 plan C-7, C-8, C-9, C-13.1): one place that knows
// what a workspace's cache holds, what a dead engine generation left in it, and what may be removed
// while no engine is using it. Every front end -- the engine's start hook, the startup task, the
// `mcppls cache` CLI and `mcppls.sweepCache` -- calls into here, so what one reports is what the
// others remove. The rules it keeps:
//
//   C-7   a copy-on-read copy a dead clangd left is swept before the next one starts (the 97.1% of
//         the 64 GiB case); the published `<module>.pcm` beside it is never touched;
//   C-8   the cache is under a budget; what still does not fit is reported, never deleted from;
//   C-9   an instance directory that stopped saying it is alive is renamed aside and removed.
//
// Nothing here stops or restarts an engine, and nothing here deletes a canonical BMI.
export module mcppls.orchestrator.cache;

import std;
import nlohmann.json;
import mcppls.base.path;
import mcppls.engine.clangd.bmi;
import mcppls.orchestrator.instance;
import mcppls.platform.fs;

export namespace mcppls::orchestrator::cache {

using Json = nlohmann::json;

inline constexpr std::uint64_t GIB { std::uint64_t { 1 } << 30 };

// What one sweep did: what it removed and what it had to leave (a lock, an antivirus scan). A
// sweeper reports its failures instead of failing silently, so a stuck cache is visible.
struct Sweep {
    std::uint64_t bytes { 0 };      // freed
    std::size_t files { 0 };        // copies removed
    std::size_t instances { 0 };    // instance directories removed
    std::size_t failed { 0 };       // entries that could not be removed
};

// C-8's limits, as the settings gave them ("unlimited" is the largest count there is).
struct Budget {
    std::uint64_t perWorkspace { 4 * GIB };
    std::uint64_t total { 16 * GIB };
};

// `bytes` against `limit`: how the status bar and the hub colour it. `near` starts at 70%.
std::string_view level_of(std::uint64_t bytes, std::uint64_t limit);

// Parses a budget value the way the settings write it: a plain byte count, a "G"/"M"/"K"-suffixed
// one, or "unlimited". nullopt for anything else (the caller keeps the default and says why).
std::optional<std::uint64_t> parse_bytes(std::string_view text);

// ---- C-7: the copy-on-read copies ---------------------------------------------------
//
// `before` is the sweep's upper bound on the file clock (fs::modified_bound of the moment nothing
// older may be removed): only copies written *before* it are deleted, so copies the generation
// that is starting right now is writing (mtime after the bound) are never touched. `before` is
// "now" on the start path, where no engine of this instance uses the tree yet, and the earliest
// live engine's start time for an interactive sweep (C-13: nothing mapped may be removed).

Sweep sweep_copies(std::string_view modulesRoot, std::int64_t before, bool dryRun = false);

// ---- C-9: instance directories ------------------------------------------------------
//
// Every instance keeps `instance.json` in the directory it works in (the owner's is the workspace
// directory itself, a guest's is `instances/<token>/`), with a heartbeat renewed with the lease.
// A directory whose heartbeat is stale belongs to a dead instance.

// The cheap part of the tick (plan C-9): stat a few `instance.json` files and rename the dead
// directories aside (`<token>.trash-<ownToken>`); the removal itself happens elsewhere, so the
// tick stays at milliseconds however large the dead directory is. Never touches a live one --
// a live guest is protected by its own heartbeat, not by the owner's lease.
std::size_t rename_dead_instances(std::string_view workspaceDirectory, std::chrono::milliseconds now, std::string_view ownToken);

// Removes what `rename_dead_instances` renamed aside and, in a full sweep (startup task, CLI
// `--prune`, the sweep command), judges every instance directory directly: heartbeat fresh ->
// alive; stale -> removed; no `instance.json` at all (a 0.0.8/0.0.9 leftover) -> removed when the
// newest file in it is older than `grace`.
Sweep sweep_instances(std::string_view workspaceDirectory, std::chrono::system_clock::time_point now,
                      std::chrono::seconds grace, bool dryRun = false);

// What the contexts moved aside (`trash`) costs, and (`dryRun` false) removes: renamed-aside junk no
// engine ever maps again, so this is safe even with a generation running.
Sweep sweep_trash(std::string_view cacheDirectory, bool dryRun = false);

// The context directories of one instance's cache (`contexts/*`), sorted.
std::vector<std::string> contexts_of(std::string_view cacheDirectory);

// `<context>/cdb/.cache/clangd/modules` -- where a context's copies live.
std::string modules_root(std::string_view context);

// The bytes of a tree, as the report counts them (a `--dry-run` needs the same number a removal
// would free).
std::uint64_t tree_bytes(std::string_view path);

// ---- C-8: the budget ----------------------------------------------------------------
//
// Steps, each only over things no engine is using: (1) the copies are already gone from the own
// tree on the start path; (2) workspaces nothing has open give up their copies, oldest-used first,
// until the global total fits. What still does not fit is reported (`level_of`), never taken from
// a canonical BMI. The own workspace is skipped -- its own server answers for it.
Sweep enforce_budget(std::string_view workspacesRoot, const Budget& budget, std::string_view ownKey,
                     std::chrono::system_clock::time_point now);

// ---- the classified report (C-10; `mcppls cache` and `cxxModules/cache` share it) ----
//
// One workspace's cache, classified: the published BMIs (`canonical`), the copies (`copies`, with
// the age of the oldest), what was moved aside (`trash`), the instance directories (`instances`,
// each with what its `instance.json` says), the largest modules. `ownToken` is only used to mark
// the caller's own entry in the instance list; the numbers do not depend on it.
Json report(std::string_view workspaceDirectory, const Budget& budget, std::chrono::system_clock::time_point now,
            std::string_view ownToken);

// Whether anything is working in a workspace directory: a lease heartbeat inside its expiry, or a
// live instance file. The CLI's `--prune` asks this instead of testing for the lease file's
// existence -- a server that crashed leaves the file behind, and the workspaces it held must stay
// reachable by a prune.
bool workspace_open(std::string_view workspaceDirectory, std::chrono::system_clock::time_point now);

// ---- the agent prompts (C-13.3, D19; the single source, server and CLI render the same) ----
//
// `facts`: mcppls/editor/os/root/cacheRoot/logDirectory/bundle/engines plus whatever the report
// already knows (copies, instances, trash). Anything absent renders as "(unknown)". The text is a
// read-only troubleshooting instruction: it tells the agent to look, never to delete.
std::string agent_prompt(const Json& facts);
std::string issue_prompt(const Json& facts);

} // namespace mcppls::orchestrator::cache
