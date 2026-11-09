// The agent task benchmark (design §10.2; README.md): each task of tasks/ is a small project, a
// prompt for a coding agent, the checks that tell whether the agent did it, and the reference
// patch that does it. The benchmark's data holds when a task's baseline fails a check and its
// reference makes every check pass, which needs mcpp and no agent; an agent's runs of the tasks
// spend API credits or model usage and run only when the environment names the agent.
import std;
import nlohmann.json;
import mcpplibs.mtest;
import mcpplibs.os;
import mcppls.harness;
import mcppls.harness.tasks;
import mcppls.harness.agent;
import mcppls.toolrun;

using namespace mcpplibs::mtest;
using mcppls::harness::on;
namespace harness = mcppls::harness;
namespace os = mcpplibs::os;

namespace {

std::vector<harness::Task> tasks_here() {
    if (!std::filesystem::exists(os::fs::from_utf8("tests/bench/tasks")))
        skip("tests/bench/tasks is not in the working directory");
    auto found = harness::discover_tasks("tests/bench/tasks", std::nullopt);
    expect(fatal(found.has_value())) << (found ? std::string{} : found.error().message);
    expect(fatal(!found->empty())) << "tests/bench/tasks holds no task";
    return std::move(*found);
}

// A case for each task: its baseline fails at least one check, its reference passes every check.
UTest task_data{"every task's baseline fails and its reference passes", on{"linux-x64"}, [] {
    if (!mcppls::toolrun::find_program("mcpp"))
        skip("mcpp is not on PATH");
    for (const auto& task : tasks_here())
        test(task.id) = [&] {
            const auto outcome = harness::validate_task(task, os::fs::utf8(temp_dir()), 300);
            expect(outcome.ok) << outcome.detail;
        };
}};

// The agent MCPPLS_BENCH_AGENT names, with the arm MCPPLS_BENCH_ARM names, on every task,
// MCPPLS_BENCH_REPEAT times; each task's success rate and the time, turns, tokens and cost of its
// runs are metrics, and every run is a record of MCPPLS_BENCH_RUNS (produced).
// Every task, MCPPLS_BENCH_REPEAT times, each run up to 15 minutes: a measurement of hours, run by hand.
Bench agent{"agent", timeout{std::chrono::hours{12}}, [] {
    const auto agent = harness::env_or("MCPPLS_BENCH_AGENT", "");
    if (agent.empty())
        skip("MCPPLS_BENCH_AGENT names no agent: a run spends API credits or model usage");
    const auto arm = harness::env_or("MCPPLS_BENCH_ARM", "mcppls-lsp");
    expect(fatal(std::ranges::contains(harness::AGENTS, agent))) << agent << " is not claude-code or copilot-cli";
    expect(fatal(std::ranges::contains(harness::ARMS, arm))) << arm << " is not grep, clangd-lsp, mcppls-lsp or mcppls-mcp";
    expect(fatal(mcppls::toolrun::find_program(agent == "claude-code" ? "claude" : "copilot").has_value()))
        << "the agent is not on PATH";
    const int repeat = std::max(1, std::stoi(harness::env_or("MCPPLS_BENCH_REPEAT", "5")));
    const auto root = os::fs::utf8(std::filesystem::current_path());
    auto records = nlohmann::ordered_json::array();
    for (const auto& task : tasks_here())
        test(task.id) = [&] {
            int successes = 0;
            for (int run = 1; run <= repeat; ++run) {
                const auto r = harness::run_one_agent_task(agent, arm, task, run, 900, os::fs::utf8(temp_dir()), root);
                successes += r.success ? 1 : 0;
                metric(std::format("run{}.seconds", run), r.wallTimeSeconds, "s");
                if (r.turns) metric(std::format("run{}.turns", run), *r.turns, "1");
                if (r.inputTokens) metric(std::format("run{}.input-tokens", run), static_cast<double>(*r.inputTokens), "1");
                if (r.outputTokens) metric(std::format("run{}.output-tokens", run), static_cast<double>(*r.outputTokens), "1");
                if (r.costUsd) metric(std::format("run{}.cost", run), *r.costUsd, "USD");
                records.push_back({{"task", r.task}, {"shape", r.shape}, {"agent", r.agent}, {"arm", r.arm},
                                   {"repeat", r.repeat}, {"success", r.success}, {"wall_time_s", r.wallTimeSeconds},
                                   {"files_changed_outside_scope", r.filesChangedOutsideScope},
                                   {"check_detail", r.checkDetail}, {"agent_stderr_tail", r.agentStderrTail}});
            }
            metric("success-rate", static_cast<double>(successes) / repeat, "1", {}, "higher");
        };
    "every run's record"_test = [&] {
        const auto file = os::fs::from_utf8(harness::env_or("MCPPLS_BENCH_RUNS", "target/bench/agent-runs.json"));
        std::filesystem::create_directories(file.parent_path());
        expect(fatal(os::fs::write(file, records.dump(2)).has_value()));
        produced(file);
    };
}};

}  // namespace
