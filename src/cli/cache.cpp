// What the module caches hold, what is dead weight in them, and what to do about it -- through the
// one implementation the server also uses (0.0.10 plan C-10, C-11): `orchestrator::cache` decides,
// this command only renders and takes options. The old hint that "each module may be stored twice"
// is gone with the classified report, because the report says what each class actually is.
module mcppls.cli.cache;

import std;
import nlohmann.json;
import mcpplibs.cmdline;
import mcppls.arch;
import mcppls.base.path;
import mcppls.base.version;
import mcppls.bundle.writer;
import mcppls.engine.clangd.process;
import mcppls.orchestrator.cache;
import mcppls.os;
import mcppls.platform.dirs;
import mcppls.platform.fs;

namespace mcppls::cli {

namespace {

namespace cmdline = mcpplibs::cmdline;
namespace fs = mcppls::platform::fs;
namespace cache = mcppls::orchestrator::cache;
using Json = nlohmann::json;

// The defaults the server also starts from (settings: `cache.instanceGrace`); the CLI reads no
// settings registry of its own.
constexpr std::chrono::seconds DEFAULT_GRACE { 86'400 };

double gigabytes(std::uint64_t bytes) { return static_cast<double>(bytes) / 1'000'000'000.0; }
double megabytes(std::uint64_t bytes) { return static_cast<double>(bytes) / 1'000'000.0; }

// `7d`, `12h`, `30m`, `600s` or plain seconds.
std::optional<std::chrono::seconds> parse_duration(std::string_view text) {
    if (text.empty()) return std::nullopt;
    std::uint64_t count { 0 };
    std::string_view unit { text };
    std::uint64_t scale { 1 };
    if (!unit.empty() && (unit.back() == 'd' || unit.back() == 'D')) {
        scale = 86'400;
        unit.remove_suffix(1);
    } else if (!unit.empty() && (unit.back() == 'h' || unit.back() == 'H')) {
        scale = 3'600;
        unit.remove_suffix(1);
    } else if (!unit.empty() && (unit.back() == 'm' || unit.back() == 'M')) {
        scale = 60;
        unit.remove_suffix(1);
    } else if (!unit.empty() && (unit.back() == 's' || unit.back() == 'S')) {
        unit.remove_suffix(1);
    }
    if (unit.empty() || !std::ranges::all_of(unit, [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
    std::from_chars(unit.data(), unit.data() + unit.size(), count);
    return std::chrono::seconds { count * scale };
}

int clean(const std::string& workspaces, const std::string& wanted) {
    std::size_t removed { 0 };
    std::uint64_t freed { 0 };
    for (const auto& entry : fs::list_directory(workspaces)) {
        const std::string name { base::file_name(entry) };
        if (wanted != "all" && !name.starts_with(wanted)) continue;
        const std::uint64_t bytes { cache::tree_bytes(entry) };
        fs::remove_all(entry);
        std::println("  removed {} ({:.1f} MB)", name, megabytes(bytes));
        ++removed;
        freed += bytes;
    }
    if (removed == 0) {
        std::println(std::cerr, "mcppls cache: no workspace cache matches {}", wanted);
        return 1;
    }
    std::println("mcppls cache: {} workspace(s), {:.1f} MB freed", removed, megabytes(freed));
    return 0;
}

// C-11: the umbrella prune, for every workspace nothing has open -- and "nothing has open" is now
// answered by the same liveness rule the server uses (a crashed server's stale lease does not
// protect its workspaces any more; a live guest's own heartbeat does).
struct PruneOptions {
    bool instancesOnly { false };
    std::chrono::seconds olderThan { 0 };       // 0: no age limit on the copies
    std::optional<std::uint64_t> maxBytes;
    bool dryRun { false };
};

int prune(const std::string& workspaces, const PruneOptions& options, cache::Budget budget) {
    const auto now { std::chrono::system_clock::now() };
    const std::int64_t bound { options.olderThan > std::chrono::seconds { 0 }
                                   ? fs::modified_now() - std::chrono::duration_cast<std::chrono::nanoseconds>(options.olderThan).count()
                                   : fs::modified_now() };
    const std::chrono::seconds grace { options.olderThan > std::chrono::seconds { 0 } ? options.olderThan : DEFAULT_GRACE };
    std::uint64_t freed { 0 };
    std::size_t instances { 0 }, failed { 0 }, skipped { 0 };
    for (const auto& workspace : fs::list_directory(workspaces)) {
        if (!fs::is_directory(workspace)) continue;
        if (cache::workspace_open(workspace, now)) {
            std::println("  {} is in use by a running instance; left as it is", base::file_name(workspace));
            ++skipped;
            continue;
        }
        if (!options.instancesOnly) {
            for (const auto& context : cache::contexts_of(workspace)) {
                const auto copies { cache::sweep_copies(cache::modules_root(context), bound, options.dryRun) };
                freed += copies.bytes;
                failed += copies.failed;
                for (const auto& build : engine::clangd::stale_module_builds(base::join_path(context, "cdb"), 2)) {
                    const std::uint64_t bytes { cache::tree_bytes(build) };
                    if (!options.dryRun) fs::remove_all(build);
                    freed += bytes;
                }
                const auto trash { cache::sweep_trash(context, options.dryRun) };
                freed += trash.bytes;
                failed += trash.failed;
            }
        }
        const auto dead { cache::sweep_instances(workspace, now, grace, options.dryRun) };
        freed += dead.bytes;
        instances += dead.instances;
        failed += dead.failed;
    }
    if (options.maxBytes) {
        // `--max-size`: this run's eviction target, in the workspaces nothing has open.
        const auto over { cache::enforce_budget(workspaces, cache::Budget { *options.maxBytes, *options.maxBytes }, {}, now) };
        freed += over.bytes;
        failed += over.failed;
    }
    const std::string_view what { options.dryRun ? "would free" : "freed" };
    std::println("mcppls cache: {:.1f} MB {}{}{}", megabytes(freed),
                 instances > 0 ? std::format(", {} instance director{}", instances, instances == 1 ? "y" : "ies") : "",
                 failed > 0 ? std::format(", {} could not be removed", failed) : "",
                 skipped > 0 ? std::format("; {} workspace(s) in use skipped", skipped) : std::string {});
    return 0;
}

// C-10: the classified report -- what is published, what is a copy, what an instance keeps, what
// the trash holds -- straight from `orchestrator::cache::report`, the numbers the server shows too.
int report(const std::string& workspaces, bool instancesOnly, bool listModules, bool json) {
    const auto now { std::chrono::system_clock::now() };
    const cache::Budget budget;
    if (json) {
        Json out = Json::array();
        for (const auto& entry : fs::list_directory(workspaces)) {
            if (!fs::is_directory(entry)) continue;
            const Json numbers = cache::report(entry, budget, now, {});
            Json one { { "workspace", std::string { base::file_name(entry) } },
                       { "directory", entry },
                       { "modules", numbers.value("modules", std::size_t { 0 }) },
                       { "bytes", numbers.value("bytes", std::uint64_t { 0 }) },
                       { "canonical", numbers.value("canonical", Json::object()) },
                       { "copies", numbers.value("copies", Json::object()) },
                       { "instances", numbers.value("instances", Json::object()) },
                       { "trash", numbers.value("trash", Json::object()) },
                       { "limits", numbers.value("limits", Json::object()) },
                       { "level", numbers.value("level", std::string { "ok" }) } };
            if (!instancesOnly) one["largest"] = numbers.value("largest", Json::array());
            out.push_back(std::move(one));
        }
        std::println("{}", Json { { "root", workspaces },
                                  { "logDirectory", base::join_path(platform::dirs::cache_directory(), "logs") },
                                  { "bundlesDirectory", bundle::default_directory() },
                                  { "workspaces", std::move(out) } }.dump(2));
        return 0;
    }
    struct Row {
        std::string name;
        std::size_t modules;
        std::uint64_t canonical, copies, instances, trash, bytes;
        std::size_t instanceCount;
        std::string level;
        Json detail;
    };
    std::vector<Row> rows;
    for (const auto& entry : fs::list_directory(workspaces)) {
        if (!fs::is_directory(entry)) continue;
        const Json numbers = cache::report(entry, budget, now, {});
        rows.push_back({ std::string { base::file_name(entry) },
                         numbers.value("modules", std::size_t { 0 }),
                         numbers["canonical"].value("bytes", std::uint64_t { 0 }),
                         numbers["copies"].value("bytes", std::uint64_t { 0 }),
                         numbers["instances"].value("bytes", std::uint64_t { 0 }),
                         numbers.value("trash", Json::object()).value("bytes", std::uint64_t { 0 }),
                         numbers.value("bytes", std::uint64_t { 0 }),
                         numbers["instances"].value("count", std::size_t { 0 }),
                         numbers.value("level", std::string { "ok" }),
                         numbers });
    }
    if (rows.empty()) {
        std::println("mcppls cache: no workspace cache under {}", workspaces);
        return 0;
    }
    std::ranges::sort(rows, {}, &Row::bytes);
    std::ranges::reverse(rows);
    if (instancesOnly) {
        std::println("{:<44} {:>8} {:>10} {}", "workspace", "instances", "bytes", "level");
        for (const auto& row : rows) std::println("{:<44} {:>8} {:>8.1f}M {}", row.name, row.instanceCount, megabytes(row.instances), row.level);
        return 0;
    }
    std::println("{:<44} {:>8} {:>8} {:>8} {:>8} {:>9}", "workspace", "modules", "canonical", "copies", "instances", "total");
    std::uint64_t total { 0 };
    for (const auto& row : rows) {
        std::println("{:<44} {:>8} {:>7.1f}M {:>7.1f}M {:>7.1f}M {:>7.1f}M {}", row.name, row.modules, megabytes(row.canonical),
                     megabytes(row.copies), megabytes(row.instances), megabytes(row.bytes), row.level);
        total += row.bytes;
        if (listModules) {
            for (const auto& module : row.detail.value("largest", Json::array())) {
                std::println("    {:>9.1f}M  {} ({} copies)", megabytes(module.value("bytes", std::uint64_t { 0 })),
                             module.value("module", std::string {}), module.value("copies", std::size_t { 0 }));
            }
        }
    }
    std::println("{:<44} {:>49.1f}M", "total", megabytes(total));
    std::println("\nThe budget is 4.0G a workspace ({}G all workspaces together) and counts everything; a cache over it after a sweep is reported, never taken from the published BMIs.", gigabytes(cache::Budget {}.total));
    return 0;
}

// D19: the same prompt the server renders, for the agents that live in a terminal. The CLI knows no
// editor and no server, so those facts render as "(unknown)" and the prompt says to look.
Json prompt_facts() {
    return Json { { "version", std::string { base::VERSION } },
                  { "os", std::string { mcppls::os::FAMILY_NAME } },
                  { "arch", std::string_view { mcppls::arch::ARCH == mcppls::arch::Arch::aarch64 ? "arm64" : "x64" } },
                  { "cacheRoot", platform::dirs::cache_directory() },
                  { "logDirectory", base::join_path(platform::dirs::cache_directory(), "logs") },
                  { "bundlesDirectory", bundle::default_directory() } };
}

int prompt(std::string_view kind) {
    if (kind == "agent") {
        std::println("{}", cache::agent_prompt(prompt_facts()));
        return 0;
    }
    if (kind == "issue") {
        std::println("{}", cache::issue_prompt(prompt_facts()));
        return 0;
    }
    std::println(std::cerr, "mcppls cache --prompt: agent or issue, not {}", kind);
    return 1;
}

} // namespace

cmdline::App cache_command(bool& handled, int& status) {
    cmdline::App command { "cache" };
    (void)command.description("What the workspace caches hold: modules, size, and what a cold start would rebuild");
    (void)command.option("clean").takes_value().help("Remove one workspace's cache by name prefix, or `all`");
    (void)command.option("prune").help("Remove what no engine holds in every workspace no instance has open: copies, stale command directories, trash, dead instance directories, and what a --max-size asks for");
    (void)command.option("instances").help("With a report: only the instance directories. With --prune: only those are removed");
    (void)command.option("older-than").takes_value().help("With --prune: only copies and instance directories older than this (600s, 30m, 7d)");
    (void)command.option("max-size").takes_value().help("With --prune: keep removing copies until the workspaces fit this size (4G, 512M, bytes)");
    (void)command.option("dry-run").help("With --prune: report what would be removed, remove nothing");
    (void)command.option("prompt").takes_value().help("Print the troubleshooting prompt for a local agent: agent | issue");
    (void)command.option("modules").help("List the largest cached modules of each workspace");
    (void)command.option("format").takes_value().help("text (default) | json");
    (void)command.action([&handled, &status](const cmdline::ParsedArgs& args) {
        handled = true;
        const std::string workspaces { base::join_path(platform::dirs::cache_directory(), "workspaces") };
        const bool json { args.value("format").value_or("text") == "json" };
        if (const auto kind = args.value("prompt"); kind && !kind->empty()) {
            status = prompt(*kind);
            return;
        }
        if (!fs::is_directory(workspaces)) {
            if (json) std::println("{}", Json { { "root", workspaces }, { "workspaces", Json::array() } }.dump(2));
            else std::println("mcppls cache: no workspace cache at {}", workspaces);
            status = 0;
            return;
        }
        if (const auto wanted = args.value("clean"); wanted && !wanted->empty()) {
            status = clean(workspaces, *wanted);
            return;
        }
        if (args.is_flag_set("prune")) {
            PruneOptions options;
            options.instancesOnly = args.is_flag_set("instances");
            options.dryRun = args.is_flag_set("dry-run");
            if (const auto given = args.value("older-than"); given && !given->empty()) {
                const auto duration = parse_duration(*given);
                if (!duration) {
                    std::println(std::cerr, "mcppls cache: --older-than does not read {}", *given);
                    status = 1;
                    return;
                }
                options.olderThan = *duration;
            }
            if (const auto given = args.value("max-size"); given && !given->empty()) {
                const auto bytes = cache::parse_bytes(*given);
                if (!bytes) {
                    std::println(std::cerr, "mcppls cache: --max-size does not read {}", *given);
                    status = 1;
                    return;
                }
                options.maxBytes = *bytes;
            }
            status = prune(workspaces, options, cache::Budget {});
            return;
        }
        status = report(workspaces, args.is_flag_set("instances"), args.is_flag_set("modules"), json);
    });
    return command;
}

} // namespace mcppls::cli
