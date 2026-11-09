module mcppls.harness.agent;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.harness.tasks;
import mcpplibs.os;
import mcppls.toolrun;

namespace os = mcpplibs::os;

namespace mcppls::harness {
namespace {
namespace toolrun = mcppls::toolrun;

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
        (void) os::fs::write(config, contents.dump(2));
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
    std::error_code ignored;
    if (arm == "mcppls-mcp") {
        const std::string config { base::join_path(base::parent_path(projectCopy), "mcp-config.json") };
        nlohmann::json server = nlohmann::json::object();
        server["type"] = "local";
        server["command"] = "mcppls";
        server["args"] = std::vector<std::string> { "mcp" };
        server["tools"] = std::vector<std::string> { "*" };
        nlohmann::json contents = nlohmann::json::object();
        contents["mcpServers"]["mcppls"] = std::move(server);
        (void) os::fs::write(config, contents.dump(2));
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
        (void)std::filesystem::create_directories(githubDir, ignored);
        (void) os::fs::write(base::join_path(githubDir, "lsp.json"), contents.dump(2));
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
        const auto relative = it->path().lexically_relative(root);
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
                             int repeatIndex, int timeoutSeconds, const std::string& scratchRoot,
                             const std::string& root) {
    std::error_code ignored;
    const std::string runDir { base::join_path(scratchRoot, std::format("{}-run-{}", task.id, repeatIndex)) };
    std::filesystem::remove_all(runDir, ignored);
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
        .bounds = mcpplibs::os::process::bounds { .hard = std::chrono::seconds { timeoutSeconds } },
    });
    record.wallTimeSeconds = std::chrono::duration<double> { std::chrono::steady_clock::now() - started }.count();

    std::string stdoutText, stderrText;
    if (result) {
        record.agentExitCode = result->timed_out ? std::optional<int> {} : std::optional<int> { toolrun::exit_code(result->status) };
        stdoutText = result->out;
        stderrText = result->err;
        if (result->timed_out) stderrText += std::format("\n[agent timed out after {}s]", timeoutSeconds);
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
            // No JSON result line: the counts stay unset.
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
} // namespace mcppls::harness
