module mcppls.devtools.bench.validate;

import std;
import mcpplibs.cmdline;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.devtools.common;
import mcppls.devtools.bench.tasks;
import mcppls.devtools.bench.patch;

namespace mcppls::devtools::bench {
namespace {

namespace fs = mcppls::platform::fs;
namespace cmdline = mcpplibs::cmdline;

constexpr int DEFAULT_CHECK_TIMEOUT_S { 300 };
constexpr std::string_view DEFAULT_TASKS_SUBDIR { "tools/bench/tasks" };

} // namespace

ValidateOutcome validate_task(const Task& task, const std::string& scratchRoot, int timeoutSeconds) {
    const std::string baseline { base::join_path(scratchRoot, task.id + "-baseline") };
    const std::string patched { base::join_path(scratchRoot, task.id + "-patched") };
    fs::remove_all(baseline);
    fs::remove_all(patched);

    if (auto copied = copy_project(task.projectDir, baseline); !copied) {
        return { false, copied.error().message };
    }
    const auto baselineOutcome = run_checks(task.checks, baseline, timeoutSeconds);
    fs::remove_all(baseline);
    if (baselineOutcome.ok) {
        return { false, "baseline already satisfies every check (it must fail at least one)" };
    }

    if (auto copied = copy_project(task.projectDir, patched); !copied) {
        return { false, copied.error().message };
    }
    auto applied = apply_patch(task.referencePath, patched);
    if (!applied) {
        const std::string detail { std::format("reference patch did not apply: {}", applied.error().message) };
        fs::remove_all(patched);
        return { false, detail };
    }
    const auto patchedOutcome = run_checks(task.checks, patched, timeoutSeconds);
    fs::remove_all(patched);
    if (!patchedOutcome.ok) {
        return { false, std::format("reference patch applied but checks still fail: {}", patchedOutcome.detail) };
    }
    return { true, "baseline fails, patched passes" };
}

int command_validate(const cmdline::ParsedArgs& arguments) {
    const std::string root { repository_root() };
    const std::string tasksDir { arguments.value("tasks").value_or(base::join_path(root, std::string { DEFAULT_TASKS_SUBDIR })) };
    if (!mcppls::platform::env::find_executable("mcpp")) {
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
    fs::remove_all(scratchRoot);
    (void) fs::create_directories(scratchRoot);

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
    fs::remove_all(scratchRoot);

    const std::size_t total { tasks->size() };
    std::println("");
    std::println("{}/{} tasks validated", total - static_cast<std::size_t>(failures), total);
    return failures ? 1 : 0;
}

} // namespace mcppls::devtools::bench
