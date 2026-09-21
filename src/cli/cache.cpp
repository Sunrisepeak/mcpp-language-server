module mcppls.cli.cache;

import std;
import nlohmann.json;
import mcpplibs.cmdline;
import mcppls.base.path;
import mcppls.platform.dirs;
import mcppls.platform.fs;
import mcppls.engine.clangd.bmi;

namespace mcppls::cli {
namespace {

namespace cmdline = mcpplibs::cmdline;
namespace fs = mcppls::platform::fs;
using Json = nlohmann::json;

// One workspace's module cache, counted the way it actually is rather than the way `find` reports it.
struct CacheReport {
    std::string name;
    std::string directory;
    std::size_t modules { 0 };       // distinct module names, not .pcm files
    std::size_t files { 0 };         // .pcm files, which is about twice `modules`
    std::uint64_t bytes { 0 };
    std::vector<std::pair<std::uint64_t, std::string>> largest;
};

// Walked by hand rather than with fs::list_files, which does not enter directories starting with
// '.' -- and every BMI lives under `<cdb>/.cache/clangd/modules/`.
void walk_pcm(const std::string& directory, const std::function<void(const std::string&)>& each) {
    for (const auto& entry : fs::list_directory(directory)) {
        if (fs::is_directory(entry)) walk_pcm(entry, each);
        else if (entry.ends_with(".pcm")) each(entry);
    }
}

std::uint64_t directory_bytes(const std::string& directory) {
    std::uint64_t total { 0 };
    for (const auto& entry : fs::list_directory(directory)) {
        if (fs::is_directory(entry)) total += directory_bytes(entry);
        else if (auto stamp = fs::stamp(entry)) total += stamp->size;
    }
    return total;
}

double megabytes(std::uint64_t bytes) { return static_cast<double>(bytes) / 1'000'000.0; }

int clean(const std::string& workspaces, const std::string& wanted) {
    std::size_t removed { 0 };
    std::uint64_t freed { 0 };
    for (const auto& entry : fs::list_directory(workspaces)) {
        const std::string name { base::file_name(entry) };
        if (wanted != "all" && !name.starts_with(wanted)) continue;
        const std::uint64_t bytes { directory_bytes(entry) };
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

int report(const std::string& workspaces, bool listModules, bool json) {
    std::vector<CacheReport> reports;
    for (const auto& entry : fs::list_directory(workspaces)) {
        if (!fs::is_directory(entry)) continue;
        CacheReport one { std::string { base::file_name(entry) }, entry, 0, 0, directory_bytes(entry), {} };
        std::set<std::string> names;
        walk_pcm(entry, [&](const std::string& path) {
            ++one.files;
            std::string module { engine::clangd::module_of_bmi(base::file_name(path)) };
            if (auto stamp = fs::stamp(path)) one.largest.emplace_back(stamp->size, module);
            names.insert(std::move(module));
        });
        one.modules = names.size();
        std::ranges::sort(one.largest, std::greater {});
        reports.push_back(std::move(one));
    }
    std::ranges::sort(reports, [](const CacheReport& a, const CacheReport& b) { return a.bytes > b.bytes; });

    if (json) {
        Json out = Json::array();
        for (const auto& one : reports) {
            Json largest = Json::array();
            for (const auto& [size, name] : one.largest | std::views::take(8)) {
                largest.push_back({ { "module", name }, { "bytes", size } });
            }
            out.push_back({ { "workspace", one.name }, { "directory", one.directory }, { "modules", one.modules },
                            { "files", one.files }, { "bytes", one.bytes }, { "largest", largest } });
        }
        std::println("{}", Json { { "root", workspaces }, { "workspaces", out } }.dump(2));
        return 0;
    }

    if (reports.empty()) {
        std::println("mcppls cache: no workspace cache under {}", workspaces);
        return 0;
    }
    std::uint64_t total { 0 };
    std::println("{:<44} {:>8} {:>8} {:>10}", "workspace", "modules", "files", "size");
    for (const auto& one : reports) {
        std::println("{:<44} {:>8} {:>8} {:>9.1f}M", one.name, one.modules, one.files, megabytes(one.bytes));
        total += one.bytes;
        if (listModules) {
            for (const auto& [size, name] : one.largest | std::views::take(8)) {
                std::println("    {:>9.1f}M  {}", megabytes(size), name);
            }
        }
    }
    std::println("{:<44} {:>27.1f}M", "total", megabytes(total));
    // Said once, because the doubled storage is the first thing anyone notices and the last thing
    // they guess: it is not a leak, it is the canonical copy beside clangd's stamped one.
    std::println("\nEach module may be stored twice (clangd's stamped BMI and the canonical copy), so `files` can be 2x `modules`.");
    return 0;
}

} // namespace

cmdline::App cache_command(bool& handled, int& status) {
    cmdline::App command { "cache" };
    (void)command.description("What the workspace caches hold: modules, size, and what a cold start would rebuild");
    (void)command.option("clean").takes_value().help("Remove one workspace's cache by name prefix, or `all`");
    (void)command.option("modules").help("List the largest cached modules of each workspace");
    (void)command.option("format").takes_value().help("text (default) | json");
    (void)command.action([&handled, &status](const cmdline::ParsedArgs& args) {
        handled = true;
        const std::string workspaces { base::join_path(platform::dirs::cache_directory(), "workspaces") };
        const bool json { args.value("format").value_or("text") == "json" };
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
        status = report(workspaces, args.is_flag_set("modules"), json);
    });
    return command;
}

} // namespace mcppls::cli
