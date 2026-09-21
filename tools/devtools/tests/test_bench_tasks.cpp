// Task loading, the declarative content check, and the project copy -- the pure logic behind
// `bench tasks validate` (tools/bench/README.md "Checks").
import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.base.path;
import mcppls.platform.fs;
import mcppls.devtools.bench.tasks;

namespace bench = mcppls::devtools::bench;
namespace fs = mcppls::platform::fs;
namespace base = mcppls::base;

namespace {

std::string temp_dir(std::string_view name) {
    const auto root = std::filesystem::current_path() / ".test-scratch"
                      / std::format("bench-tasks-{}-{}", name, std::random_device {}());
    std::filesystem::create_directories(root);
    return root.string();
}

} // namespace

int main() {
    using namespace mcppls::testing;

    "a command check parses as an argv list"_test = [] {
        auto check = bench::parse_check(nlohmann::json::parse(R"(["mcpp", "build"])"));
        expect(check.has_value());
        if (!check) return;
        expect(check->command.size() == 2);
        expect(check->command[0] == "mcpp");
        expect(!check->content.has_value());
    };

    "an empty command list is rejected"_test = [] {
        auto check = bench::parse_check(nlohmann::json::parse(R"([])"));
        expect(!check.has_value());
    };

    "file-contains parses files/all/none"_test = [] {
        auto check = bench::parse_check(nlohmann::json::parse(
            R"({"file-contains": {"files": ["a", "b"], "all": ["x"], "none": ["y"]}})"));
        expect(check.has_value());
        if (!check) return;
        expect(!check->content.has_value() == false);
        expect(check->content->files.size() == 2);
        expect(check->content->all == std::vector<std::string> { "x" });
        expect(check->content->none == std::vector<std::string> { "y" });
    };

    "file-contains needs at least one of all/none"_test = [] {
        auto check = bench::parse_check(nlohmann::json::parse(R"({"file-contains": {"files": ["a"]}})"));
        expect(!check.has_value());
    };

    "file-contains needs a non-empty files list"_test = [] {
        auto check = bench::parse_check(nlohmann::json::parse(R"({"file-contains": {"files": [], "all": ["x"]}})"));
        expect(!check.has_value());
    };

    "an unrecognized check shape is rejected"_test = [] {
        auto check = bench::parse_check(nlohmann::json::parse(R"({"unknown": {}})"));
        expect(!check.has_value());
    };

    "file-contains.all: every listed file's content is concatenated in order"_test = [&] {
        const std::string dir { temp_dir("all") };
        (void) fs::write_file(base::join_path(dir, "one.txt"), "hello ");
        (void) fs::write_file(base::join_path(dir, "two.txt"), "world");
        bench::Check check { .command = {}, .content = bench::ContentCheck { .files = { "one.txt", "two.txt" }, .all = { "hello world" } } };
        const auto outcome = bench::run_checks({ check }, dir, 10);
        expect(outcome.ok) << outcome.detail;
        std::filesystem::remove_all(dir);
    };

    "file-contains.all: a missing substring fails the check"_test = [&] {
        const std::string dir { temp_dir("all-missing") };
        (void) fs::write_file(base::join_path(dir, "one.txt"), "hello");
        bench::Check check { .command = {}, .content = bench::ContentCheck { .files = { "one.txt" }, .all = { "salute" } } };
        const auto outcome = bench::run_checks({ check }, dir, 10);
        expect(!outcome.ok);
        std::filesystem::remove_all(dir);
    };

    "file-contains.none: a present-but-forbidden substring fails the check"_test = [&] {
        const std::string dir { temp_dir("none") };
        (void) fs::write_file(base::join_path(dir, "main.cpp"), "import hello.greet:format;\n");
        bench::Check check { .command = {}, .content = bench::ContentCheck { .files = { "main.cpp" }, .none = { ":format" } } };
        const auto outcome = bench::run_checks({ check }, dir, 10);
        expect(!outcome.ok);
        std::filesystem::remove_all(dir);
    };

    "file-contains.none: absence of the forbidden substring passes"_test = [&] {
        const std::string dir { temp_dir("none-ok") };
        (void) fs::write_file(base::join_path(dir, "main.cpp"), "import hello.greet;\n");
        bench::Check check { .command = {}, .content = bench::ContentCheck { .files = { "main.cpp" }, .none = { ":format" } } };
        const auto outcome = bench::run_checks({ check }, dir, 10);
        expect(outcome.ok) << outcome.detail;
        std::filesystem::remove_all(dir);
    };

    "run_checks stops at the first failing check, like a chain"_test = [&] {
        const std::string dir { temp_dir("chain") };
        (void) fs::write_file(base::join_path(dir, "f.txt"), "abc");
        bench::Check failing { .command = {}, .content = bench::ContentCheck { .files = { "f.txt" }, .all = { "zzz" } } };
        bench::Check wouldPass { .command = {}, .content = bench::ContentCheck { .files = { "f.txt" }, .all = { "abc" } } };
        const auto outcome = bench::run_checks({ failing, wouldPass }, dir, 10);
        expect(!outcome.ok);
        std::filesystem::remove_all(dir);
    };

    "copy_project copies sources and drops target/ and compile_commands.json"_test = [&] {
        const std::string src { temp_dir("copy-src") };
        (void) fs::create_directories(base::join_path(src, "target/pack"));
        (void) fs::write_file(base::join_path(src, "target/pack/stale"), "stale");
        (void) fs::write_file(base::join_path(src, "compile_commands.json"), "[]");
        (void) fs::create_directories(base::join_path(src, "src"));
        (void) fs::write_file(base::join_path(src, "src/main.cpp"), "int main(){}");
        const std::string dest { temp_dir("copy-dest") };
        std::filesystem::remove_all(dest);
        auto copied = bench::copy_project(src, dest);
        expect(copied.has_value());
        expect(fs::is_regular_file(base::join_path(dest, "src/main.cpp")));
        expect(!fs::exists(base::join_path(dest, "target")));
        expect(!fs::exists(base::join_path(dest, "compile_commands.json")));
        std::filesystem::remove_all(src);
        std::filesystem::remove_all(dest);
    };

    "copy_tree with a skip predicate leaves out a whole subtree by name"_test = [&] {
        const std::string src { temp_dir("skip-src") };
        (void) fs::write_file(base::join_path(src, "keep.txt"), "keep");
        (void) fs::create_directories(base::join_path(src, "drop"));
        (void) fs::write_file(base::join_path(src, "drop/inside.txt"), "dropped");
        const std::string dest { temp_dir("skip-dest") };
        std::filesystem::remove_all(dest);
        auto copied = bench::copy_tree(src, dest, [](std::string_view name) { return name == "drop"; });
        expect(copied.has_value());
        expect(fs::is_regular_file(base::join_path(dest, "keep.txt")));
        expect(!fs::exists(base::join_path(dest, "drop")));
        std::filesystem::remove_all(src);
        std::filesystem::remove_all(dest);
    };

    "load_task rejects an id that does not match its directory name"_test = [&] {
        const std::string dir { temp_dir("badid") };
        const std::string taskDir { base::join_path(dir, "my-task") };
        (void) fs::create_directories(base::join_path(taskDir, "project"));
        (void) fs::write_file(base::join_path(taskDir, "project/mcpp.toml"), "[package]\n");
        (void) fs::write_file(base::join_path(taskDir, "reference.patch"), "");
        (void) fs::write_file(base::join_path(taskDir, "task.json"), R"({
            "id": "not-my-task", "title": "t", "prompt": "p", "project": "project",
            "check": [["mcpp", "build"]], "reference": "reference.patch", "shape": "split"
        })");
        auto task = bench::load_task(taskDir);
        expect(!task.has_value());
        std::filesystem::remove_all(dir);
    };

    "load_task accepts a well-formed task with a declarative check"_test = [&] {
        const std::string dir { temp_dir("goodtask") };
        const std::string taskDir { base::join_path(dir, "my-task") };
        (void) fs::create_directories(base::join_path(taskDir, "project"));
        (void) fs::write_file(base::join_path(taskDir, "project/mcpp.toml"), "[package]\n");
        (void) fs::write_file(base::join_path(taskDir, "reference.patch"), "");
        (void) fs::write_file(base::join_path(taskDir, "task.json"), R"({
            "id": "my-task", "title": "t", "prompt": "p", "project": "project",
            "check": [["mcpp", "build"], {"file-contains": {"files": ["src/x.cppm"], "all": ["y"]}}],
            "reference": "reference.patch", "shape": "all-cppm", "toolchain": "llvm@22.1.8"
        })");
        auto task = bench::load_task(taskDir);
        expect(task.has_value());
        if (!task) return;
        expect(task->checks.size() == 2);
        expect(task->checks[1].content.has_value());
        expect(task->toolchain.has_value() && *task->toolchain == "llvm@22.1.8");
        std::filesystem::remove_all(dir);
    };

    return report();
}
