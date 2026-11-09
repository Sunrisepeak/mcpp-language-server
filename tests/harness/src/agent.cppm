// The agent task benchmark's runs (tests/bench/README.md, "Agent runs"): a real coding agent CLI
// pointed at a task, headless, and whether the task's checks pass afterwards. A run spends API
// credits or model usage: the Bench that makes runs (tests/bench/tasks.cpp) is skipped unless
// MCPPLS_BENCH_AGENT names an agent, and CI names none.
export module mcppls.harness.agent;

import std;
import mcppls.harness.tasks;

export namespace mcppls::harness {

// The agents and the arms a run names.
inline constexpr std::array<std::string_view, 2> AGENTS { "claude-code", "copilot-cli" };
inline constexpr std::array<std::string_view, 4> ARMS { "grep", "clangd-lsp", "mcppls-lsp", "mcppls-mcp" };

// The argv a headless agent CLI is started with for one task/arm, before the project copy's
// path and prompt are substituted in (both builders write files into `projectCopy` for the
// arms that need one, e.g. `.github/lsp.json`, an MCP config).
std::vector<std::string> claude_code_command(const Task& task, const std::string& repositoryRoot,
                                             const std::string& projectCopy, const std::string& arm);
std::vector<std::string> copilot_cli_command(const Task& task, const std::string& projectCopy, const std::string& arm);

struct RunRecord {
    std::string task;
    std::string shape;
    std::string agent;
    std::string arm;
    int repeat { 0 };
    bool success { false };
    double wallTimeSeconds { 0.0 };
    std::optional<int> agentExitCode;
    std::optional<int> turns;
    std::optional<long long> inputTokens;
    std::optional<long long> outputTokens;
    std::optional<double> costUsd;
    std::vector<std::string> filesChangedOutsideScope;
    std::string checkDetail;
    std::string agentStderrTail;
};

// Every file under `root`, its (size, modified-time) stamp, skipping target/ and .git/.
std::map<std::string, std::pair<std::uint64_t, std::int64_t>> agent_run_snapshot(const std::string& root);

// Changed paths (added, removed, or modified) between two snapshots, outside src/ and tests/.
std::vector<std::string> agent_run_changed_outside_scope(
    const std::map<std::string, std::pair<std::uint64_t, std::int64_t>>& before,
    const std::map<std::string, std::pair<std::uint64_t, std::int64_t>>& after);

// One run of `agent` on `task` with `arm`, in a copy of its project under `scratchRoot`; `root` is
// the repository, whose editors/ the arms that load a plugin name.
RunRecord run_one_agent_task(const std::string& agent, const std::string& arm, const Task& task,
                             int repeatIndex, int timeoutSeconds, const std::string& scratchRoot,
                             const std::string& root);

} // namespace mcppls::harness
