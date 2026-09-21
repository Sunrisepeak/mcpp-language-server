// `mcppls-devtools bench tasks run`: points a real coding agent CLI at every task, headless,
// and records whether the checks pass afterward. Ported from tools/bench/run.py's `run` command
// (see its module docstring and bench/README.md "run" for what is verified vs. assumed).
//
// Spends API credits / model usage on every invocation; refuses to run without
// --i-understand-this-uses-api-credits. Nothing in this repository's own tooling, CI included,
// passes that flag.
export module mcppls.devtools.bench.agent;

import std;
import mcpplibs.cmdline;
import mcppls.devtools.bench.tasks;

export namespace mcppls::devtools::bench {

inline constexpr std::string_view SAFETY_FLAG { "i-understand-this-uses-api-credits" };

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

RunRecord run_one_agent_task(const std::string& agent, const std::string& arm, const Task& task,
                             int repeatIndex, int timeoutSeconds, const std::string& scratchRoot);

// `mcppls-devtools bench tasks run --agent ... --arm ... --repeat N --out FILE
//     [--tasks DIR] [--only ID] [--timeout SECONDS] --i-understand-this-uses-api-credits`.
int command_run(const mcpplibs::cmdline::ParsedArgs& arguments);

} // namespace mcppls::devtools::bench
