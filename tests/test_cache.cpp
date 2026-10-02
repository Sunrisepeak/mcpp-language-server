// The cache sweeper, the budget and the report (0.0.10 plan C-7, C-8, C-9; §6).
import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.os;
import mcppls.base.path;
import mcppls.platform.dirs;
import mcppls.platform.fs;
import mcppls.engine.clangd.bmi;
import mcppls.orchestrator.cache;
import mcppls.orchestrator.instance;

using Json = nlohmann::json;
namespace fs = mcppls::platform::fs;
namespace cache = mcppls::orchestrator::cache;
namespace bmi = mcppls::engine::clangd;

namespace {

std::string scratch(std::string_view name) {
    const std::string root { mcppls::base::join_path(mcppls::platform::dirs::temp_directory(),
        std::format("mcppls-test-{}-{}", name, std::chrono::steady_clock::now().time_since_epoch().count())) };
    (void)fs::create_directories(root);
    return root;
}

// One module's shape as clangd leaves it: the published `<module>.pcm` beside the copies it made
// from it, in a command directory (`<...>/modules/<unit>-<hash>/<command-hash>/`).
std::string command_directory(const std::string& workspace, std::string_view unit) {
    const std::string directory { mcppls::base::join_path(workspace,
        mcppls::base::join_path("contexts/default/cdb/.cache/clangd/modules", std::format("{}-0123456789abcdef", unit))) };
    (void)fs::create_directories(mcppls::base::join_path(directory, "11111111111111111111111111111111"));
    return mcppls::base::join_path(directory, "11111111111111111111111111111111");
}

void write_copy(const std::string& directory, std::string_view module, std::string_view serial, std::size_t bytes, bool old = true) {
    const std::string file { mcppls::base::join_path(directory, std::format("{}-{}.pcm", module, serial)) };
    (void)fs::write_file(file, std::string(bytes, 'x'));
    // Stamps compare on the file clock; an old copy needs an old mtime, so one is written back.
    if (old) {
        const auto target { std::chrono::file_clock::now() - std::chrono::hours { 48 } };
        std::filesystem::last_write_time(file, target);
    }
}

void write_canonical(const std::string& directory, std::string_view module, std::size_t bytes) {
    (void)fs::write_file(mcppls::base::join_path(directory, std::format("{}.pcm", module)), std::string(bytes, 'x'));
}

void write_instance(const std::string& directory, std::int64_t at, std::string_view token) {
    (void)fs::create_directories(directory);
    const Json document { { "token", std::string { token } }, { "version", "0.0.10" }, { "root", "/somewhere" },
                          { "at", at }, { "shared", true } };
    (void)fs::write_file(mcppls::base::join_path(directory, "instance.json"), document.dump());
}

std::size_t count_pcm(const std::string& directory) {
    std::size_t count { 0 };
    for (const auto& entry : fs::list_files(directory, std::array<std::string_view, 1> { ".pcm" }, {})) {
        (void)entry;
        ++count;
    }
    return count;
}

} // namespace

int main() {
    using namespace mcppls::testing;

    "a copy is a stamped name whose canonical BMI sits beside it; a module named like a stamp is not a copy"_test = [] {
        const std::string directory { scratch("bmi") };
        write_canonical(directory, "pybind11", 16);
        write_copy(directory, "pybind11", "20261002-194715-990444", 16);
        write_canonical(directory, "my-module", 16);
        write_copy(directory, "my-module", "20261002-194715-000001", 16);
        // `foo-20260101-120000-1` with no `foo.pcm` beside it is a module of its own (C-7's guard).
        write_copy(directory, "foo", "20260101-120000-1", 16);

        expect(bmi::is_versioned_copy("pybind11-20261002-194715-990444.pcm", directory));
        expect(bmi::is_versioned_copy("my-module-20261002-194715-000001.pcm", directory)) << "a module name with a dash does not defeat the shape";
        expect(!bmi::is_versioned_copy("pybind11.pcm", directory));
        expect(!bmi::is_versioned_copy("std.pcm", directory));
        const bool fooCopy { bmi::is_versioned_copy("foo-20260101-120000-1.pcm", directory) };
        expect(!fooCopy) << std::format("predicate={} module_of_bmi={} foo.pcm exists={} joined={}",
                                        fooCopy, bmi::module_of_bmi("foo-20260101-120000-1.pcm"),
                                        mcppls::platform::fs::exists(mcppls::base::join_path(directory, "foo.pcm")),
                                        mcppls::base::join_path(directory, std::format("{}.pcm", bmi::module_of_bmi("foo-20260101-120000-1.pcm"))));
        fs::remove_all(directory);
    };

    "a sweep takes the copies and leaves every canonical BMI, and the report says the same numbers"_test = [] {
        const std::string workspace { scratch("sweep") };
        const std::string directory { command_directory(workspace, "main") };
        write_canonical(directory, "pybind11", 1'000'000);
        for (int index = 0; index < 127; ++index) {
            write_copy(directory, "pybind11", std::format("20261002-194715-{:06d}", index), 100'000);
        }
        expect(count_pcm(directory) == 128);

        const auto before { fs::modified_now() + 1 };
        const cache::Sweep sweep { cache::sweep_copies(mcppls::base::join_path(workspace, "contexts/default/cdb/.cache/clangd/modules"), before) };
        expect(sweep.files == 127) << "all 127 copies";
        expect(sweep.bytes > 12'000'000) << "the bytes are the copies' own";
        expect(sweep.failed == 0);
        expect(count_pcm(directory) == 1) << "the canonical BMI stays";

        // The report's classified numbers agree with what the sweep removed.
        Json numbers;
        try {
            numbers = cache::report(workspace, cache::Budget {}, std::chrono::system_clock::now(), {});
        } catch (const Json::exception& error) {
            expect(false) << std::format("report threw: {}", error.what());
        }
        expect(numbers.value("copies", Json::object()).value("files", std::size_t { 0 }) == 0);
        expect(numbers.value("canonical", Json::object()).value("files", std::size_t { 0 }) == 1);
        expect(numbers.value("canonical", Json::object()).value("bytes", std::uint64_t { 0 }) == 1'000'000);
        fs::remove_all(workspace);
    };

    "a sweep honours its bound: what a live generation wrote stays"_test = [] {
        const std::string workspace { scratch("bound") };
        const std::string directory { command_directory(workspace, "main") };
        write_canonical(directory, "greet", 10);
        write_copy(directory, "greet", "20261002-194715-000001", 10);
        const auto afterTheOldCopy { fs::modified_now() };
        // A copy written "just now" (fresh mtime): the bound stops the sweep one step before it.
        write_copy(directory, "greet", "20261002-194715-000002", 10, false);

        const cache::Sweep sweep { cache::sweep_copies(mcppls::base::join_path(workspace, "contexts/default/cdb/.cache/clangd/modules"), afterTheOldCopy) };
        expect(sweep.files == 1) << "one copy within the bound";
        expect(count_pcm(directory) == 2) << "the fresh copy and the canonical stay";
        fs::remove_all(workspace);
    };

    "a dry run counts and removes nothing"_test = [] {
        const std::string workspace { scratch("dry") };
        const std::string directory { command_directory(workspace, "main") };
        write_canonical(directory, "greet", 10);
        write_copy(directory, "greet", "20261002-194715-000001", 100);
        const cache::Sweep sweep { cache::sweep_copies(mcppls::base::join_path(workspace, "contexts/default/cdb/.cache/clangd/modules"), fs::modified_now() + 1, true) };
        expect(sweep.files == 1 && sweep.bytes == 100);
        expect(count_pcm(directory) == 2);
        fs::remove_all(workspace);
    };

    "instance directories: a fresh heartbeat is alive, a stale one is reaped, and a leftover waits for its grace"_test = [] {
        const std::string workspace { scratch("instances") };
        const auto now { std::chrono::system_clock::now() };
        const std::int64_t nowMs { std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() };
        const std::string instances { mcppls::base::join_path(workspace, "instances") };

        write_instance(mcppls::base::join_path(instances, "aaaaaaaaaaaaaaaa"), nowMs, "aaaaaaaaaaaaaaaa");
        write_instance(mcppls::base::join_path(instances, "bbbbbbbbbbbbbbbb"), nowMs - 120'000, "bbbbbbbbbbbbbbbb");   // 2 min stale
        write_instance(mcppls::base::join_path(instances, "cccccccccccccccc"), 0, "cccccccccccccccc");
        (void)fs::remove(mcppls::base::join_path(instances, "cccccccccccccccc/instance.json"));
        // `cccc` says nothing (a 0.0.9 leftover); its tree is fresh, so the grace keeps it.
        (void)fs::write_file(mcppls::base::join_path(instances, "cccccccccccccccc/model.a.json"), "{}");

        const cache::Sweep sweep { cache::sweep_instances(workspace, now, std::chrono::hours { 24 }) };
        expect(sweep.instances == 1) << sweep.instances;
        expect(fs::is_directory(mcppls::base::join_path(instances, "aaaaaaaaaaaaaaaa"))) << "the live one stays";
        expect(!fs::exists(mcppls::base::join_path(instances, "bbbbbbbbbbbbbbbb")));
        expect(fs::is_directory(mcppls::base::join_path(instances, "cccccccccccccccc"))) << "fresh without a self-description: the grace holds";

        // The tick's cheap half: rename only, never remove.
        const std::size_t renamed { cache::rename_dead_instances(workspace, std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()), "dddddddddddddddd") };
        expect(renamed == 1) << "the stale one is renamed aside";
        expect(fs::is_directory(mcppls::base::join_path(instances, "cccccccccccccccc.trash-dddddddddddddddd")));
        // And a sweep removes what the tick renamed aside.
        const cache::Sweep taken { cache::sweep_instances(workspace, now, std::chrono::hours { 24 }) };
        expect(taken.instances == 1) << "the renamed-aside directory is taken";
        expect(fs::is_directory(mcppls::base::join_path(instances, "aaaaaaaaaaaaaaaa"))) << "the live one survived the second sweep";
        expect(!fs::exists(mcppls::base::join_path(instances, "cccccccccccccccc.trash-dddddddddddddddd")));
        fs::remove_all(workspace);
    };

    "a leftover older than its grace goes, and a budget evicts copies before anything published"_test = [] {
        const std::string workspace { scratch("grace") };
        const std::string instances { mcppls::base::join_path(workspace, "instances") };
        // A leftover with nothing to say for itself, whose newest file is two days old: past the
        // 24-hour grace, so the sweep takes it.
        const std::string leftover { mcppls::base::join_path(instances, "eeeeeeeeeeeeeeee") };
        (void)fs::create_directories(leftover);
        const std::string oldModel { mcppls::base::join_path(leftover, "model.old.json") };
        (void)fs::write_file(oldModel, "{}");
        std::filesystem::last_write_time(oldModel, std::chrono::file_clock::now() - std::chrono::hours { 48 });
        const std::string directory { command_directory(workspace, "main") };
        write_canonical(directory, "pybind11", 2'000'000);
        write_copy(directory, "pybind11", "20261002-194715-000001", 1'000'000);
        const cache::Sweep sweep { cache::sweep_instances(workspace, std::chrono::system_clock::now(), std::chrono::hours { 24 }) };
        expect(sweep.instances == 1) << "the leftover is past its grace";
        expect(!fs::exists(leftover));

        // The budget, directly: a 1'000'000-byte limit evicts the copy, keeps the canonical.
        const Json before = cache::report(workspace, cache::Budget { 1'000'000, 1'000'000 }, std::chrono::system_clock::now(), {});
        expect(before.value("limits", Json::object()).value("over", false));
        const std::string workspaces { scratch("workspaces") };
        const std::string moved { mcppls::base::join_path(workspaces, "someproject-abcdef") };
        std::filesystem::rename(workspace, moved);
        const cache::Sweep evicted { cache::enforce_budget(workspaces, cache::Budget { 1'000'000, 1'000'000 }, {}, std::chrono::system_clock::now()) };
        expect(evicted.files == 1) << evicted.files;
        expect(cache::tree_bytes(mcppls::base::join_path(moved, "contexts/default/cdb/.cache/clangd/modules")) >= 2'000'000) << "the canonical BMI stays";
        fs::remove_all(workspaces);
    };

    "parse_bytes reads what the settings write, and level_of colours the fill"_test = [] {
        expect(cache::parse_bytes("4G") == std::uint64_t { 4 } << 30);
        expect(cache::parse_bytes("512M") == std::uint64_t { 512 } << 20);
        expect(cache::parse_bytes("100K") == std::uint64_t { 100 } << 10);
        expect(cache::parse_bytes("4096") == std::uint64_t { 4'096 });
        expect(cache::parse_bytes("unlimited") == std::numeric_limits<std::uint64_t>::max());
        expect(!cache::parse_bytes("four").has_value());
        expect(cache::level_of(1'000, 4'000) == "ok");
        expect(cache::level_of(3'000, 4'000) == "near");
        expect(cache::level_of(4'001, 4'000) == "over");
    };

    "the agent prompt is the read-only instruction the plan wrote down (D19)"_test = [] {
        const Json facts { { "version", "0.0.10" }, { "editor", "VS Code" }, { "editorVersion", "1.95" },
                           { "os", "linux" }, { "root", "/project" }, { "cacheRoot", "/cache" },
                           { "logDirectory", "/cache/log" },
                           { "engines", Json::array({ Json { { "name", "clangd" }, { "version", "23.1.0" } } }) } };
        const std::string prompt { cache::agent_prompt(facts) };
        expect(prompt.contains("READ ONLY"));
        expect(prompt.contains("/cache")) << "the cache root's real value";
        expect(prompt.contains("/cache/log"));
        expect(prompt.contains("clangd")) << "support facts are not hidden (D17)";
        expect(prompt.contains("Do not delete any file"));
        expect(prompt.contains("mcppls cache --format json"));
        expect(prompt.contains("five sentences"));
        expect(!prompt.contains("{}")) << "nothing left unrendered";
        const std::string issue { cache::issue_prompt(facts) };
        expect(issue.contains("bug_report.yml"));
        expect(issue.contains("Show the draft to the person"));
    };

    return report();
}
