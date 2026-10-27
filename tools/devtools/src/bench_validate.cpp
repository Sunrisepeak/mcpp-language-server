module mcppls.devtools.bench.validate;

import std;
import mcpplibs.cmdline;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.devtools.common;
import mcppls.devtools.bench.tasks;
import mcppls.devtools.bench.patch;
import mcppls.toolrun;

namespace mcppls::devtools::bench {
namespace {
namespace cmdline = mcpplibs::cmdline;

constexpr int DEFAULT_CHECK_TIMEOUT_S { 300 };
constexpr std::string_view DEFAULT_TASKS_SUBDIR { "tools/bench/tasks" };

} // namespace

ValidateOutcome validate_task(const Task& task, const std::string& scratchRoot, int timeoutSeconds) {
    std::error_code ignored;
    const std::string baseline { base::join_path(scratchRoot, task.id + "-baseline") };
    const std::string patched { base::join_path(scratchRoot, task.id + "-patched") };
    std::filesystem::remove_all(baseline, ignored);
    std::filesystem::remove_all(patched, ignored);

    if (auto copied = copy_project(task.projectDir, baseline); !copied) {
        return { false, copied.error().message };
    }
    const auto baselineOutcome = run_checks(task.checks, baseline, timeoutSeconds);
    std::filesystem::remove_all(baseline, ignored);
    if (baselineOutcome.ok) {
        return { false, "baseline already satisfies every check (it must fail at least one)" };
    }

    if (auto copied = copy_project(task.projectDir, patched); !copied) {
        return { false, copied.error().message };
    }
    auto applied = apply_patch(task.referencePath, patched);
    if (!applied) {
        const std::string detail { std::format("reference patch did not apply: {}", applied.error().message) };
        std::filesystem::remove_all(patched, ignored);
        return { false, detail };
    }
    const auto patchedOutcome = run_checks(task.checks, patched, timeoutSeconds);
    std::filesystem::remove_all(patched, ignored);
    if (!patchedOutcome.ok) {
        return { false, std::format("reference patch applied but checks still fail: {}", patchedOutcome.detail) };
    }
    return { true, "baseline fails, patched passes" };
}

int command_validate(const cmdline::ParsedArgs& arguments) {
    std::error_code ignored;
    const std::string root { repository_root() };
    const std::string tasksDir { arguments.value("tasks").value_or(base::join_path(root, std::string { DEFAULT_TASKS_SUBDIR })) };
    if (!toolrun::find_program("mcpp")) {
        std::println(std::cerr, "error: `mcpp` is not on PATH (see tools/bench/README.md for setup)");
        return 2;
    }
    std::optional<std::string> only;
    if (auto value = arguments.value("only"); value && !value->empty()) only = *value;
    int timeoutSeconds { DEFAULT_CHECK_TIMEOUT_S };
    if (auto value = arguments.value("timeout"); value && !value->empty()) timeoutSeconds = std::stoi(*value);

    auto tasks = discover_tasks(tasksDir, only);
    if (!tasks) {
        std::println(std::cerr, "error: {}", tasks.error().message);
        return 2;
    }
    if (tasks->empty()) {
        std::println(std::cerr, "error: no tasks found under {}", tasksDir);
        return 2;
    }

    const std::string scratchRoot { base::join_path(root, "target/bench-validate") };
    std::filesystem::remove_all(scratchRoot, ignored);
    (void)std::filesystem::create_directories(scratchRoot, ignored);

    int failures { 0 };
    for (const auto& task : *tasks) {
        const auto started = std::chrono::steady_clock::now();
        ValidateOutcome outcome;
        try {
            outcome = validate_task(task, scratchRoot, timeoutSeconds);
        } catch (const std::exception& error) {
            outcome = { false, std::format("unexpected error: {}", error.what()) };
        }
        const auto elapsed = std::chrono::duration<double> { std::chrono::steady_clock::now() - started }.count();
        std::println("{} {} ({}, {:.1f}s)", outcome.ok ? "PASS" : "FAIL", task.id, task.shape, elapsed);
        if (!outcome.ok) {
            ++failures;
            for (const auto line : base::split(outcome.detail, '\n')) std::println("     {}", line);
        }
    }
    std::filesystem::remove_all(scratchRoot, ignored);

    const std::size_t total { tasks->size() };
    std::println("");
    std::println("{}/{} tasks validated", total - static_cast<std::size_t>(failures), total);
    return failures ? 1 : 0;
}

} // namespace mcppls::devtools::bench
