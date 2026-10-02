module mcppls.orchestrator.cache;

import std;
import nlohmann.json;
import mcppls.base.path;
import mcppls.engine.clangd.bmi;
import mcppls.orchestrator.instance;
import mcppls.platform.fs;

namespace mcppls::orchestrator::cache {

using Json = nlohmann::json;

namespace fs = mcppls::platform::fs;
namespace bmi = mcppls::engine::clangd;

namespace {

std::uint64_t directory_bytes(const std::string& directory) {
    std::uint64_t total { 0 };
    for (const auto& entry : fs::list_directory(directory)) {
        if (fs::is_directory(entry)) total += directory_bytes(entry);
        else if (const auto stamp = fs::stamp(entry)) total += stamp->size;
    }
    return total;
}

// The newest file mtime in a tree, on the file clock: the only liveness signal a 0.0.8/0.0.9
// instance left behind (C-9: it wrote no `instance.json`).
std::int64_t newest_modified(const std::string& directory) {
    std::int64_t newest { std::numeric_limits<std::int64_t>::min() };
    for (const auto& entry : fs::list_directory(directory)) {
        if (fs::is_directory(entry)) newest = std::max(newest, newest_modified(entry));
        else if (const auto stamp = fs::stamp(entry)) newest = std::max(newest, stamp->modified);
    }
    return newest;
}

void sweep_into(const std::string& directory, std::int64_t before, bool dryRun, Sweep& sweep) {
    for (const auto& entry : fs::list_directory(directory)) {
        if (fs::is_directory(entry)) {
            sweep_into(entry, before, dryRun, sweep);
            continue;
        }
        const std::string_view name { base::file_name(entry) };
        if (!name.ends_with(".pcm")) continue;
        const auto stamp = fs::stamp(entry);
        // Written after the bound: a generation that is alive right now may hold it mapped.
        if (!stamp || stamp->modified >= before) continue;
        if (!bmi::is_versioned_copy(name, directory)) continue;
        if (dryRun) {
            sweep.bytes += stamp->size;
            ++sweep.files;
        } else if (fs::remove(entry)) {
            sweep.bytes += stamp->size;
            ++sweep.files;
        } else {
            ++sweep.failed;
        }
    }
}

// What an instance said about itself most recently, `at` in milliseconds since the epoch.
struct InstanceFile {
    std::string token;
    std::string version;
    std::string root;
    std::int64_t at { 0 };
    bool shared { false };
};

std::optional<InstanceFile> instance_file(const std::string& directory) {
    const auto text = fs::read_file(base::join_path(directory, "instance.json"));
    if (!text) return std::nullopt;
    const Json document = Json::parse(*text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) return std::nullopt;
    return InstanceFile { document.value("token", std::string {}), document.value("version", std::string {}),
                          document.value("root", std::string {}), document.value("at", std::int64_t { 0 }),
                          document.value("shared", false) };
}

// Whether an instance (its own directory's heartbeat) says it is alive: within twice the lease
// expiry, the same window the lease takeover already uses, and wider than a renewal interval.
bool instance_alive(const std::optional<InstanceFile>& described, std::chrono::system_clock::time_point now) {
    if (!described || described->at <= 0) return false;
    const auto age { std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() - described->at };
    return age >= 0 && age < std::chrono::duration_cast<std::chrono::milliseconds>(LEASE_EXPIRY * 2).count();
}

// Whether anything is working in a workspace directory this process does not own: a fresh lease, a
// fresh own `instance.json`, or any fresh guest directory. Used by the global budget, which must
// not touch what another instance serves.
bool workspace_live(const std::string& directory, std::chrono::system_clock::time_point now) {
    if (const auto text = fs::read_file(base::join_path(directory, "owner.lease"))) {
        const Json lease = Json::parse(*text, nullptr, false);
        if (!lease.is_discarded() && lease.is_object()) {
            const auto heartbeat = lease.value("heartbeat", std::int64_t { 0 });
            const auto age { std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() - heartbeat };
            if (heartbeat > 0 && age >= 0 && age < std::chrono::duration_cast<std::chrono::milliseconds>(LEASE_EXPIRY).count()) return true;
        }
    }
    if (instance_alive(instance_file(directory), now)) return true;
    const std::string instances { base::join_path(directory, "instances") };
    if (fs::is_directory(instances)) {
        for (const auto& entry : fs::list_directory(instances)) {
            if (fs::is_directory(entry) && instance_alive(instance_file(entry), now)) return true;
        }
    }
    return false;
}

std::string interpolated(std::string_view value) { return value.empty() ? std::string { "(unknown)" } : std::string { value }; }

} // namespace

bool workspace_open(std::string_view workspaceDirectory, std::chrono::system_clock::time_point now) {
    return workspace_live(std::string { workspaceDirectory }, now);
}

std::string_view level_of(std::uint64_t bytes, std::uint64_t limit) {
    if (limit == 0 || bytes > limit) return "over";
    if (bytes * 10 >= limit * 7) return "near";
    return "ok";
}

std::optional<std::uint64_t> parse_bytes(std::string_view text) {
    if (text.empty()) return std::nullopt;
    if (text == "unlimited") return std::numeric_limits<std::uint64_t>::max();
    std::uint64_t scale { 1 };
    if (text.back() == 'G' || text.back() == 'g') {
        scale = GIB;
        text.remove_suffix(1);
    } else if (text.back() == 'M' || text.back() == 'm') {
        scale = std::uint64_t { 1 } << 20;
        text.remove_suffix(1);
    } else if (text.back() == 'K' || text.back() == 'k') {
        scale = std::uint64_t { 1 } << 10;
        text.remove_suffix(1);
    }
    const std::uint64_t count { [&] {
        std::uint64_t value { 0 };
        const auto [_, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        return error == std::errc {} ? value : std::uint64_t { 0 };
    }() };
    if (count == 0 && text != "0") return std::nullopt;
    if (count > std::numeric_limits<std::uint64_t>::max() / scale) return std::numeric_limits<std::uint64_t>::max();
    return count * scale;
}

Sweep sweep_copies(std::string_view modulesRoot, std::int64_t before, bool dryRun) {
    Sweep sweep;
    if (fs::is_directory(modulesRoot)) sweep_into(std::string { modulesRoot }, before, dryRun, sweep);
    return sweep;
}

std::size_t rename_dead_instances(std::string_view workspaceDirectory, std::chrono::milliseconds now, std::string_view ownToken) {
    const std::string instances { base::join_path(workspaceDirectory, "instances") };
    if (!fs::is_directory(instances)) return 0;
    std::size_t renamed { 0 };
    const auto nowPoint { std::chrono::system_clock::time_point { std::chrono::duration_cast<std::chrono::system_clock::duration>(now) } };
    for (const auto& entry : fs::list_directory(instances)) {
        const std::string name { base::file_name(entry) };
        if (!fs::is_directory(entry) || name.find(".trash-") != std::string::npos) continue;
        const auto described = instance_file(entry);
        if (instance_alive(described, nowPoint)) continue;
        // Renamed aside in one cheap metadata operation; the removal (seconds to minutes on a
        // full directory) happens in the background, and a failed rename waits for the next tick.
        if (fs::rename(entry, std::format("{}.trash-{}", entry, ownToken))) ++renamed;
    }
    return renamed;
}

Sweep sweep_instances(std::string_view workspaceDirectory, std::chrono::system_clock::time_point now,
                      std::chrono::seconds grace, bool dryRun) {
    Sweep sweep;
    const std::string instances { base::join_path(workspaceDirectory, "instances") };
    if (!fs::is_directory(instances)) return sweep;
    // The file clock is linear: its reading of "now minus the grace" is its reading of now, less
    // the grace in nanoseconds.
    const std::int64_t staleBefore { fs::modified_now() - std::chrono::duration_cast<std::chrono::nanoseconds>(grace).count() };
    for (const auto& entry : fs::list_directory(instances)) {
        if (!fs::is_directory(entry)) continue;
        const std::string name { base::file_name(entry) };
        if (name.find(".trash-") == std::string::npos) {
            const auto described = instance_file(entry);
            if (instance_alive(described, now)) continue;   // a live guest keeps its directory
            if (!described) {
                // No self-description at all (a version before 0.0.10): the only signal is whether
                // anything here was written recently. Older than the grace, it is a leftover.
                if (newest_modified(entry) >= staleBefore) continue;
            }
        }
        if (dryRun) {
            sweep.bytes += directory_bytes(entry);
            ++sweep.instances;
            continue;
        }
        const fs::Removal removed { fs::tree_remove(entry) };
        sweep.bytes += removed.bytes;
        sweep.failed += removed.failed;
        if (removed.failed == 0) ++sweep.instances;
    }
    return sweep;
}

Sweep sweep_trash(std::string_view cacheDirectory, bool dryRun) {
    Sweep sweep;
    for (const auto& context : contexts_of(cacheDirectory)) {
        const std::string trash { base::join_path(context, "trash") };
        if (!fs::is_directory(trash)) continue;
        const std::uint64_t bytes { directory_bytes(trash) };
        if (dryRun) {
            sweep.bytes += bytes;
        } else {
            const fs::Removal removed { fs::tree_remove(trash) };
            sweep.bytes += removed.bytes;
            sweep.failed += removed.failed;
        }
    }
    return sweep;
}

std::vector<std::string> contexts_of(std::string_view cacheDirectory) {
    std::vector<std::string> contexts;
    const std::string root { base::join_path(cacheDirectory, "contexts") };
    if (fs::is_directory(root)) {
        for (const auto& entry : fs::list_directory(root)) {
            if (fs::is_directory(entry)) contexts.push_back(entry);
        }
    }
    return contexts;
}

std::string modules_root(std::string_view context) {
    return base::join_path(base::join_path(context, "cdb"), base::join_path(".cache", base::join_path("clangd", "modules")));
}

std::uint64_t tree_bytes(std::string_view path) {
    return fs::is_directory(path) ? directory_bytes(std::string { path }) : 0;
}

Sweep enforce_budget(std::string_view workspacesRoot, const Budget& budget, std::string_view ownKey,
                     std::chrono::system_clock::time_point now) {
    Sweep sweep;
    if (!fs::is_directory(workspacesRoot) || budget.total == std::numeric_limits<std::uint64_t>::max()) return sweep;
    struct Candidate {
        std::string path;
        std::int64_t lastUse;
    };
    std::vector<Candidate> candidates;
    std::uint64_t total { 0 };
    for (const auto& entry : fs::list_directory(workspacesRoot)) {
        if (!fs::is_directory(entry)) continue;
        const std::string name { base::file_name(entry) };
        const std::uint64_t bytes { directory_bytes(entry) };
        total += bytes;
        if (name == ownKey || workspace_live(entry, now)) continue;
        // Only dead workspaces are ranked, so the wall-clock heartbeat of the instance file is
        // stale by definition; the tree's newest file mtime is the ranking that is left.
        candidates.push_back({ entry, newest_modified(entry) });
    }
    if (total <= budget.total) return sweep;
    // Oldest-used first: the workspace nobody has touched for the longest gives up its copies.
    std::ranges::sort(candidates, {}, &Candidate::lastUse);
    for (const auto& candidate : candidates) {
        if (total <= budget.total) break;
        const std::string contexts { base::join_path(candidate.path, "contexts") };
        if (!fs::is_directory(contexts)) continue;
        for (const auto& context : fs::list_directory(contexts)) {
            if (!fs::is_directory(context)) continue;
            const Sweep one { sweep_copies(modules_root(context), fs::modified_now()) };
            sweep.bytes += one.bytes;
            sweep.files += one.files;
            sweep.failed += one.failed;
            total = total > one.bytes ? total - one.bytes : 0;
        }
    }
    return sweep;
}

Json report(std::string_view workspaceDirectory, const Budget& budget, std::chrono::system_clock::time_point now,
            std::string_view ownToken) {
    const std::string workspace { std::string { workspaceDirectory } };
    Json contexts = Json::array();
    std::uint64_t canonicalBytes { 0 };
    std::size_t canonicalFiles { 0 };
    std::uint64_t copiesBytes { 0 };
    std::size_t copiesFiles { 0 };
    std::int64_t oldestCopy { std::numeric_limits<std::int64_t>::max() };
    std::map<std::string, std::pair<std::uint64_t, std::size_t>> modules;   // name -> {bytes, copies}
    std::uint64_t trashBytes { 0 };
    const std::string contextsRoot { base::join_path(workspace, "contexts") };
    if (fs::is_directory(contextsRoot)) {
        for (const auto& context : fs::list_directory(contextsRoot)) {
            if (!fs::is_directory(context)) continue;
            Json one { { "name", std::string { base::file_name(context) } } };
            std::uint64_t contextCanonicalBytes { 0 };
            std::size_t contextCanonicalFiles { 0 };
            std::uint64_t contextCopiesBytes { 0 };
            std::size_t contextCopiesFiles { 0 };
            const std::string modulesRoot { modules_root(context) };
            if (fs::is_directory(modulesRoot)) {
                for (const auto& entry : fs::list_files(modulesRoot, std::array<std::string_view, 1> { ".pcm" }, {})) {
                    const auto stamp = fs::stamp(entry);
                    if (!stamp) continue;
                    const std::string name { base::file_name(entry) };
                    const std::string module { bmi::module_of_bmi(name) };
                    if (bmi::is_versioned_copy(name, base::parent_path(entry))) {
                        ++contextCopiesFiles;
                        contextCopiesBytes += stamp->size;
                        ++modules[module].second;
                        modules[module].first += stamp->size;
                        oldestCopy = std::min(oldestCopy, stamp->modified);
                    } else {
                        ++contextCanonicalFiles;
                        contextCanonicalBytes += stamp->size;
                    }
                }
            }
            canonicalBytes += contextCanonicalBytes;
            canonicalFiles += contextCanonicalFiles;
            copiesBytes += contextCopiesBytes;
            copiesFiles += contextCopiesFiles;
            one["canonical"] = Json { { "files", contextCanonicalFiles }, { "bytes", contextCanonicalBytes } };
            one["copies"] = Json { { "files", contextCopiesFiles }, { "bytes", contextCopiesBytes } };
            const std::string trash { base::join_path(context, "trash") };
            const std::uint64_t contextTrash { fs::is_directory(trash) ? directory_bytes(trash) : 0 };
            trashBytes += contextTrash;
            one["trash"] = Json { { "bytes", contextTrash } };
            contexts.push_back(std::move(one));
        }
    }
    std::size_t instanceCount { 0 };
    std::uint64_t instanceBytes { 0 };
    Json instanceList = Json::array();
    const std::string instances { base::join_path(workspace, "instances") };
    if (fs::is_directory(instances)) {
        for (const auto& entry : fs::list_directory(instances)) {
            if (!fs::is_directory(entry)) continue;
            const auto described = instance_file(entry);
            const std::uint64_t bytes { directory_bytes(entry) };
            ++instanceCount;
            instanceBytes += bytes;
            instanceList.push_back(Json { { "token", described ? described->token : std::string { base::file_name(entry) } },
                                          { "version", described ? described->version : std::string {} },
                                          { "root", described ? described->root : std::string {} },
                                          { "at", Json { described ? described->at : std::int64_t { 0 } } },
                                          { "bytes", bytes },
                                          { "shared", described ? described->shared : true },
                                          { "alive", instance_alive(described, now) },
                                          { "own", described && described->token == ownToken } });
        }
    }
    Json largest = Json::array();
    std::vector<std::pair<std::string, std::pair<std::uint64_t, std::size_t>>> sorted { modules.begin(), modules.end() };
    std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
    for (const auto& [name, weight] : sorted | std::views::take(20)) {
        largest.push_back(Json { { "module", name }, { "bytes", weight.first }, { "copies", weight.second } });
    }
    const std::uint64_t total { directory_bytes(workspace) };
    const std::int64_t oldestAgeSeconds {
        oldestCopy == std::numeric_limits<std::int64_t>::max()
            ? 0
            : std::max<std::int64_t>(0, (fs::modified_now() - oldestCopy) / std::int64_t { 1'000'000'000 })
    };
    return Json { { "bytes", total },
                  { "modules", modules.size() },
                  { "canonical", Json { { "files", canonicalFiles }, { "bytes", canonicalBytes } } },
                  { "copies", Json { { "files", copiesFiles }, { "bytes", copiesBytes }, { "oldestSeconds", oldestAgeSeconds } } },
                  { "trash", Json { { "bytes", trashBytes } } },
                  { "instances", Json { { "count", instanceCount }, { "bytes", instanceBytes }, { "list", std::move(instanceList) } } },
                  { "largest", std::move(largest) },
                  { "limits", Json { { "perWorkspace", budget.perWorkspace }, { "total", budget.total },
                                     { "over", total > budget.perWorkspace } } },
                  { "level", std::string { level_of(total, budget.perWorkspace) } },
                  { "contexts", std::move(contexts) } };
}

std::string agent_prompt(const Json& facts) {
    const auto text = [&](std::string_view key) { return interpolated(facts.value(key, std::string {})); };
    std::string engines;
    if (const auto list = facts.find("engines"); list != facts.end() && list->is_array()) {
        for (const auto& engine : *list) {
            engines.append(std::format("\n- engine: {} {}", interpolated(engine.value("name", std::string {})),
                                       interpolated(engine.value("version", std::string {}))));
        }
    }
    return std::format(R"(You are helping debug a cache problem of mcppls (mcpp-language-server), the C++ modules language server.

READ ONLY. Do not delete any file. Do not run `mcppls cache --clean` or any `-CleanAll`. Do not change any configuration. Do not send any log or report anywhere. If something needs to be deleted or published, stop and ask the person first.

Environment facts:
- mcppls {}, editor {} {}, {}/{}
- workspace root: {}
- cache root: {}
- log directory: {}
- diagnostic bundle or report, if one was made: {}{}
- the server's own view: `mcppls report`

Read-only commands to look at:
- `mcppls cache --format json` -- the classified report: canonical BMIs vs copies vs instances vs trash
- `mcppls cache --modules` -- the largest cached modules
- `mcppls cache --prune --dry-run` -- what a prune would remove (it removes nothing)
- the tail of the newest files matching {}/server-*.log* -- especially `clangd exited unexpectedly` lines
- `incidents/` under the cache root -- what the server already recorded by itself

What to check, most likely first:
1. copies vs canonical: the copies' share of the bytes. 90%+ copies is clangd's copy-on-read leftover from engines that died; each crash leaks one copy per read module (about 305 MB each in the known case).
2. instances/: orphan instance directories (a guest that died). Count, size, and whether any `instance.json` inside still has a fresh heartbeat `at`.
3. trash directories: what a sweep could not finish removing (locked files).
4. how often and how clustered `clangd exited unexpectedly` appears in the logs.
5. `mcppls cache --prune` freed nearly nothing although the cache is large -- true for mcppls older than 0.0.10, which does not reach copies or instances.
6. whether MCPPLS_CACHE_DIR is set: the cache is where it says, not the default location.
7. free disk space on the volume the cache root is on.

Answer in five sentences: is this a bug; which of the above it is; the evidence; what can be done locally without deleting anything; whether it needs an issue filed.)",
                       text("version"), text("editor"), text("editorVersion"), text("os"), text("arch"), text("root"), text("cacheRoot"),
                       text("logDirectory"), text("bundle"), engines, text("logDirectory"));
}

std::string issue_prompt(const Json& facts) {
    const auto text = [&](std::string_view key) { return interpolated(facts.value(key, std::string {})); };
    return std::format(R"(Turn the cache analysis below into a GitHub issue draft for https://github.com/Sunrisepeak/mcpp-language-server, using the `bug_report.yml` template fields. Write it for a person to read: the conclusion first, then the evidence. Show the draft to the person and wait for their agreement before anything is submitted anywhere; attach nothing without their say-so.

Fields to fill:
- version: {}
- editor: {} {}
- os: {}/{}
- build-system: {} (leave the template's default if the analysis did not say)
- what-happened: the cache grew without bound; the conclusion of the analysis in one paragraph, with the numbers
- expected: the cache stays under the configured budget (mcppls.cache.maxBytes, default 4G per workspace)
- bundle / report: attach only if the person agrees; name the path you would attach: {} (bundle) / {} (report)
- steps: the shortest sequence that reproduces it, ending with `mcppls cache --format json` output (redacted)

Rules: redact home-directory paths, user names and host names; do not invent numbers the analysis did not produce; say explicitly when a number is unknown.)",
                       text("version"), text("editor"), text("editorVersion"), text("os"), text("arch"), text("buildSystem"), text("bundle"),
                       text("report"));
}

} // namespace mcppls::orchestrator::cache
