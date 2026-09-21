module mcppls.devtools.bench.agent;

import std;
import nlohmann.json;
import mcpplibs.cmdline;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.platform.process;
import mcppls.platform.toolrun;
import mcppls.devtools.common;
import mcppls.devtools.bench.tasks;

namespace mcppls::devtools::bench {
namespace {

namespace fs = mcppls::platform::fs;
namespace toolrun = mcppls::platform::toolrun;
namespace cmdline = mcpplibs::cmdline;

constexpr std::array<std::string_view, 2> VALID_AGENTS { "claude-code", "copilot-cli" };
constexpr std::array<std::string_view, 4> VALID_ARMS { "grep", "clangd-lsp", "mcppls-lsp", "mcppls-mcp" };
constexpr std::string_view DEFAULT_TASKS_SUBDIR { "tools/bench/tasks" };

bool one_of(std::string_view value, std::span<const std::string_view> valid) {
    return std::ranges::any_of(valid, [&](std::string_view v) { return v == value; });
}

// A copilot-cli lsp.json server entry (design 8.2: mcppls-lsp vs. clangd-lsp), trimmed to the
// common fields (see editors/copilot-cli/lsp.json for the documented-fields-only full list).
nlohmann::json copilot_server_entry(const std::string& command, const std::vector<std::string>& args) {
    nlohmann::json entry = nlohmann::json::object();
    entry["command"] = command;
    entry["args"] = args;
    nlohmann::json extensions = nlohmann::json::object();
    extensions[".cpp"] = "cpp";
    extensions[".cppm"] = "cpp";
    extensions[".h"] = "cpp";
    extensions[".c"] = "c";
    entry["fileExtensions"] = std::move(extensions);
    return entry;
}

} // namespace

std::vector<std::string> claude_code_command(const Task& task, const std::string& repositoryRoot,
                                             const std::string& projectCopy, const std::string& arm) {
    std::vector<std::string> cmd { "claude", "-p", task.prompt, "--output-format", "json",
                                   "--permission-mode", "acceptEdits", "--permission-prompts", "none" };
    if (arm == "grep") {
        cmd.push_back("--bare");
    } else if (arm == "mcppls-lsp") {
        const std::string pluginDir { base::join_path(repositoryRoot, "editors/claude-code/mcppls-lsp") };
        cmd.push_back("--bare");
        cmd.push_back("--plugin-dir");
        cmd.push_back(pluginDir);
    } else if (arm == "mcppls-mcp") {
        const std::string config { base::join_path(base::parent_path(projectCopy), "mcp-config.json") };
        nlohmann::json server = nlohmann::json::object();
        server["command"] = "mcppls";
        server["args"] = std::vector<std::string> { "mcp" };
        nlohmann::json contents = nlohmann::json::object();
        contents["mcpServers"]["mcppls"] = std::move(server);
        (void) fs::write_file(config, contents.dump(2));
        cmd.push_back("--bare");
        cmd.push_back("--mcp-config");
        cmd.push_back(config);
        cmd.push_back("--strict-mcp-config");
        cmd.push_back("--allowedTools");
        cmd.push_back("mcp__mcppls");
    } else if (arm == "clangd-lsp") {
        // No local plugin source to point --bare --plugin-dir at (unlike mcppls-lsp): relies on
        // the operator having run `claude plugin install clangd-lsp@claude-plugins-official` at
        // user scope, and on clangd being on PATH (bench/README.md "Unverified").
    } else {
        throw std::invalid_argument { std::format("unsupported arm for claude-code: {}", arm) };
    }
    return cmd;
}

std::vector<std::string> copilot_cli_command(const Task& task, const std::string& projectCopy, const std::string& arm) {
    if (arm == "mcppls-mcp") {
        const std::string config { base::join_path(base::parent_path(projectCopy), "mcp-config.json") };
        nlohmann::json server = nlohmann::json::object();
        server["type"] = "local";
        server["command"] = "mcppls";
        server["args"] = std::vector<std::string> { "mcp" };
        server["tools"] = std::vector<std::string> { "*" };
        nlohmann::json contents = nlohmann::json::object();
        contents["mcpServers"]["mcppls"] = std::move(server);
        (void) fs::write_file(config, contents.dump(2));
        return { "copilot", "-p", task.prompt, "--allow-all-tools", "--additional-mcp-config", "@" + config };
    }
    if (arm == "grep") {
        // no .github/lsp.json written for this arm: no LSP tooling at all
    } else if (arm == "mcppls-lsp" || arm == "clangd-lsp") {
        const auto [name, command, args] = arm == "mcppls-lsp"
            ? std::tuple<std::string, std::string, std::vector<std::string>> { "mcppls", "mcppls", { "serve" } }
            : std::tuple<std::string, std::string, std::vector<std::string>> { "clangd", "clangd", { "--background-index" } };
        nlohmann::json contents = nlohmann::json::object();
        contents["lspServers"][name] = copilot_server_entry(command, args);
        const std::string githubDir { base::join_path(projectCopy, ".github") };
        (void) fs::create_directories(githubDir);
        (void) fs::write_file(base::join_path(githubDir, "lsp.json"), contents.dump(2));
    } else {
        throw std::invalid_argument { std::format("unsupported arm for copilot-cli: {}", arm) };
    }
    return { "copilot", "-p", task.prompt, "--allow-all-tools" };
}

std::map<std::string, std::pair<std::uint64_t, std::int64_t>> agent_run_snapshot(const std::string& root) {
    std::map<std::string, std::pair<std::uint64_t, std::int64_t>> out;
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(root, error);
        !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (!it->is_regular_file()) continue;
        const auto relative = std::filesystem::relative(it->path(), root);
        if (!relative.empty()) {
            const std::string first { relative.begin()->string() };
            if (first == "target" || first == ".git") continue;
        }
        std::error_code statusError;
        const auto size = std::filesystem::file_size(it->path(), statusError);
        const auto modified = std::filesystem::last_write_time(it->path(), statusError);
        out[relative.generic_string()] = { statusError ? 0 : size,
                                           modified.time_since_epoch().count() };
    }
    return out;
}

std::vector<std::string> agent_run_changed_outside_scope(
    const std::map<std::string, std::pair<std::uint64_t, std::int64_t>>& before,
    const std::map<std::string, std::pair<std::uint64_t, std::int64_t>>& after) {
    std::set<std::string> changed;
    for (const auto& [path, _] : before) if (!after.contains(path)) changed.insert(path);
    for (const auto& [path, _] : after) if (!before.contains(path)) changed.insert(path);
    for (const auto& [path, stamp] : before) {
        if (auto it = after.find(path); it != after.end() && it->second != stamp) changed.insert(path);
    }
    std::vector<std::string> out;
    for (const auto& path : changed) {
        if (!path.starts_with("src/") && !path.starts_with("tests/")) out.push_back(path);
    }
    return out;
}

RunRecord run_one_agent_task(const std::string& agent, const std::string& arm, const Task& task,
                             int repeatIndex, int timeoutSeconds, const std::string& scratchRoot) {
    const std::string root { repository_root() };
    const std::string runDir { base::join_path(scratchRoot, std::format("{}-run-{}", task.id, repeatIndex)) };
    fs::remove_all(runDir);
    const std::string projectCopy { base::join_path(runDir, "project") };
    RunRecord record { .task = task.id, .shape = task.shape, .agent = agent, .arm = arm, .repeat = repeatIndex };
    if (auto copied = copy_project(task.projectDir, projectCopy); !copied) {
        record.checkDetail = copied.error().message;
        return record;
    }
    const auto before = agent_run_snapshot(projectCopy);

    std::vector<std::string> command;
    try {
        command = agent == "claude-code" ? claude_code_command(task, root, projectCopy, arm)
                                          : copilot_cli_command(task, projectCopy, arm);
    } catch (const std::exception& error) {
        record.checkDetail = error.what();
        return record;
    }

    const auto started = std::chrono::steady_clock::now();
    auto result = toolrun::run({
        .program = command.front(),
        .arguments = { command.begin() + 1, command.end() },
        .workDirectory = projectCopy,
        .purpose = "bench",
        .network = toolrun::Network::allowed,
        .bounds = mcppls::platform::RunBounds { .hard = std::chrono::seconds { timeoutSeconds } },
    });
    record.wallTimeSeconds = std::chrono::duration<double> { std::chrono::steady_clock::now() - started }.count();

    std::string stdoutText, stderrText;
    if (result) {
        record.agentExitCode = result->timedOut ? std::optional<int> {} : std::optional<int> { result->exitCode };
        stdoutText = result->output;
        stderrText = result->error;
        if (result->timedOut) stderrText += std::format("\n[agent timed out after {}s]", timeoutSeconds);
    } else {
        stderrText = result.error().message;
    }

    const auto trimmedStdout = base::trim(stdoutText);
    if (agent == "claude-code" && !trimmedStdout.empty()) {
        const auto lines = base::split_lines(trimmedStdout);
        try {
            if (!lines.empty()) {
                nlohmann::json payload = nlohmann::json::parse(lines.back());
                if (payload.contains("num_turns")) record.turns = payload.at("num_turns").get<int>();
                const auto usage = payload.value("usage", nlohmann::json::object());
                if (usage.contains("input_tokens")) record.inputTokens = usage.at("input_tokens").get<long long>();
                if (usage.contains("output_tokens")) record.outputTokens = usage.at("output_tokens").get<long long>();
                if (payload.contains("total_cost_usd")) record.costUsd = payload.at("total_cost_usd").get<double>();
            }
        } catch (const std::exception&) {
            // copilot-cli/claude-code: no parseable JSON result line -- fields stay unset,
            // matching run.py's `except (json.JSONDecodeError, IndexError): pass`.
        }
    }

    const auto after = agent_run_snapshot(projectCopy);
    record.filesChangedOutsideScope = agent_run_changed_outside_scope(before, after);

    const auto checkOutcome = run_checks(task.checks, projectCopy, 300);
    record.success = checkOutcome.ok;
    record.checkDetail = checkOutcome.ok ? "all checks passed" : checkOutcome.detail;
    const auto stderrLines = base::split_lines(stderrText);
    const std::size_t tailCount { std::min<std::size_t>(20, stderrLines.size()) };
    std::vector<std::string> tail { stderrLines.end() - tailCount, stderrLines.end() };
    record.agentStderrTail = base::join(tail, "\n");
    return record;
}

int command_run(const cmdline::ParsedArgs& arguments) {
    if (!arguments.is_flag_set(SAFETY_FLAG)) {
        std::println(std::cerr,
            "error: refusing to run -- pass --{} to confirm you understand\n"
            "this invokes a real coding agent, which spends API credits or model\n"
            "usage on every task/arm/repeat. Nothing in this repository's own\n"
            "tooling or CI passes this flag; run it yourself, deliberately.", SAFETY_FLAG);
        return 2;
    }
    const auto agentOpt = arguments.value("agent");
    const auto armOpt = arguments.value("arm");
    if (!agentOpt || !one_of(*agentOpt, VALID_AGENTS)) {
        std::println(std::cerr, "error: --agent must be one of claude-code, copilot-cli");
        return 2;
    }
    if (!armOpt || !one_of(*armOpt, VALID_ARMS)) {
        std::println(std::cerr, "error: --arm must be one of grep, clangd-lsp, mcppls-lsp, mcppls-mcp");
        return 2;
    }
    const std::string agent { *agentOpt };
    const std::string arm { *armOpt };
    const int repeat { arguments.value("repeat") ? std::stoi(*arguments.value("repeat")) : 5 };
    const auto outOpt = arguments.value("out");
    if (!outOpt || outOpt->empty()) {
        std::println(std::cerr, "error: --out FILE is required");
        return 2;
    }
    const int timeoutSeconds { arguments.value("timeout") ? std::stoi(*arguments.value("timeout")) : 900 };

    const std::string root { repository_root() };
    const std::string tasksDir { arguments.value("tasks").value_or(base::join_path(root, std::string { DEFAULT_TASKS_SUBDIR })) };
    std::optional<std::string> only;
    if (auto value = arguments.value("only"); value && !value->empty()) only = *value;
    auto tasks = discover_tasks(tasksDir, only);
    if (!tasks) {
        std::println(std::cerr, "error: {}", tasks.error().message);
        return 2;
    }

    const std::string agentBinaryName { agent == "claude-code" ? "claude" : "copilot" };
    if (!mcppls::platform::env::find_executable(agentBinaryName)) {
        std::println(std::cerr, "error: `{}` is not on PATH", agentBinaryName);
        return 2;
    }

    const std::string scratchRoot { base::join_path(root, "target/bench-run") };
    fs::remove_all(scratchRoot);
    (void) fs::create_directories(scratchRoot);

    std::vector<RunRecord> runs;
    for (const auto& task : *tasks) {
        for (int repeatIndex { 1 }; repeatIndex <= repeat; ++repeatIndex) {
            std::println(std::cerr, "running {} arm={} agent={} repeat={}/{}...", task.id, arm, agent, repeatIndex, repeat);
            auto record = run_one_agent_task(agent, arm, task, repeatIndex, timeoutSeconds, scratchRoot);
            std::println(std::cerr, "  -> {} in {:.1f}s", record.success ? "PASS" : "FAIL", record.wallTimeSeconds);
            runs.push_back(std::move(record));
        }
    }
    fs::remove_all(scratchRoot);

    const std::size_t successes { static_cast<std::size_t>(std::ranges::count_if(runs, &RunRecord::success)) };
    nlohmann::ordered_json out;
    out["agent"] = agent;
    out["arm"] = arm;
    {
        const auto now = std::chrono::system_clock::now();
        out["generated_at"] = std::format("{:%Y-%m-%dT%H:%M:%SZ}", std::chrono::floor<std::chrono::seconds>(now));
    }
    out["run_id"] = [] {
        std::random_device rd;
        return std::format("{:016x}{:016x}", static_cast<std::uint64_t>(rd()) << 32 | rd(),
                           static_cast<std::uint64_t>(rd()) << 32 | rd());
    }();
    out["tasks_dir"] = tasksDir;
    out["repeat"] = repeat;
    out["summary"] = { { "runs", runs.size() }, { "successes", successes },
                       { "success_rate", runs.empty() ? 0.0 : static_cast<double>(successes) / runs.size() } };
    out["runs"] = nlohmann::json::array();
    for (const auto& r : runs) {
        nlohmann::ordered_json entry;
        entry["task"] = r.task;
        entry["shape"] = r.shape;
        entry["agent"] = r.agent;
        entry["arm"] = r.arm;
        entry["repeat"] = r.repeat;
        entry["success"] = r.success;
        entry["wall_time_s"] = r.wallTimeSeconds;
        entry["agent_exit_code"] = r.agentExitCode ? nlohmann::json(*r.agentExitCode) : nlohmann::json();
        entry["turns"] = r.turns ? nlohmann::json(*r.turns) : nlohmann::json();
        entry["input_tokens"] = r.inputTokens ? nlohmann::json(*r.inputTokens) : nlohmann::json();
        entry["output_tokens"] = r.outputTokens ? nlohmann::json(*r.outputTokens) : nlohmann::json();
        entry["cost_usd"] = r.costUsd ? nlohmann::json(*r.costUsd) : nlohmann::json();
        entry["files_changed_outside_scope"] = r.filesChangedOutsideScope;
        entry["check_detail"] = r.checkDetail;
        entry["agent_stderr_tail"] = r.agentStderrTail;
        out["runs"].push_back(std::move(entry));
    }
    (void) fs::write_file(*outOpt, out.dump(2));
    std::println("wrote {}: {}/{} runs succeeded", *outOpt, successes, runs.size());
    return 0;
}

} // namespace mcppls::devtools::bench
