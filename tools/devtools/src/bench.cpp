module mcppls.devtools.bench;

import std;
import mcpplibs.cmdline;
import mcppls.devtools.bench.validate;
import mcppls.devtools.bench.agent;
import mcppls.devtools.bench.review;

namespace mcppls::devtools {
namespace {

namespace cmdline = mcpplibs::cmdline;

int dispatch_tasks(const cmdline::ParsedArgs& tasksArgs) {
    if (!tasksArgs.has_subcommand()) {
        std::println(std::cerr, "mcppls-devtools: say `bench tasks validate` or `bench tasks run`");
        return 2;
    }
    const auto inner = tasksArgs.subcommand();
    if (!inner) {
        std::println(std::cerr, "mcppls-devtools: say `bench tasks validate` or `bench tasks run`");
        return 2;
    }
    if (tasksArgs.subcommand_name() == "validate") return bench::command_validate(inner->get());
    if (tasksArgs.subcommand_name() == "run") return bench::command_run(inner->get());
    std::println(std::cerr, "mcppls-devtools: unknown `bench tasks` subcommand '{}'", tasksArgs.subcommand_name());
    return 2;
}

int dispatch_bench(const cmdline::ParsedArgs& benchArgs) {
    if (!benchArgs.has_subcommand()) {
        std::println(std::cerr, "mcppls-devtools: say `bench tasks ...` or `bench review ...`");
        return 2;
    }
    const auto inner = benchArgs.subcommand();
    if (!inner) {
        std::println(std::cerr, "mcppls-devtools: say `bench tasks ...` or `bench review ...`");
        return 2;
    }
    if (benchArgs.subcommand_name() == "tasks") return dispatch_tasks(inner->get());
    if (benchArgs.subcommand_name() == "review") return bench::command_review(inner->get());
    std::println(std::cerr, "mcppls-devtools: unknown `bench` subcommand '{}'", benchArgs.subcommand_name());
    return 2;
}

} // namespace

cmdline::App bench_command(bool& handled, int& status) {
    cmdline::App validateCmd { "validate" };
    (void) validateCmd.description("Check every task's baseline/reference pair (no agent, no network beyond mcpp build/test)");
    (void) validateCmd.option("tasks").takes_value().help("Tasks directory (default tools/bench/tasks)");
    (void) validateCmd.option("only").takes_value().help("Validate a single task id");
    (void) validateCmd.option("timeout").takes_value().help("Seconds allowed per check command (default 300)");

    cmdline::App runCmd { "run" };
    (void) runCmd.description("Run a coding agent against every task -- spends API credits, never run by CI");
    (void) runCmd.option("agent").takes_value().help("claude-code | copilot-cli");
    (void) runCmd.option("arm").takes_value().help("grep | clangd-lsp | mcppls-lsp | mcppls-mcp");
    (void) runCmd.option("repeat").takes_value().help("Repeats per task (default 5)");
    (void) runCmd.option("out").takes_value().help("Path to write the results JSON to");
    (void) runCmd.option("tasks").takes_value().help("Tasks directory (default tools/bench/tasks)");
    (void) runCmd.option("only").takes_value().help("Run a single task id");
    (void) runCmd.option("timeout").takes_value().help("Seconds allowed per agent invocation (default 900)");
    (void) runCmd.option("i-understand-this-uses-api-credits")
        .help("Required: confirms you understand this spends API credits / model usage");

    cmdline::App tasksCmd { "tasks" };
    (void) tasksCmd.description("The task benchmark's baseline/reference pairs");
    (void) tasksCmd.subcommand(std::move(validateCmd));
    (void) tasksCmd.subcommand(std::move(runCmd));

    cmdline::App reviewCmd { "review" };
    (void) reviewCmd.description("Whether `mcppls review` finds what a change breaks, and nothing a clean one does");
    (void) reviewCmd.option("server").takes_value().help("The mcppls binary to review with (required)");
    (void) reviewCmd.option("payload").takes_value().help("A payload directory (for fixtures that need the semantic kit)");
    (void) reviewCmd.option("mock-model").takes_value().help("mcppls-mock-model, for fixtures whose review.json scripts a model");
    (void) reviewCmd.option("fixture").takes_value().multiple().help("Restrict to this fixture id (repeatable)");
    (void) reviewCmd.option("timeout").takes_value().help("Seconds allowed per fixture's review (default 300)");
    (void) reviewCmd.option("keep").help("Keep the scratch workspaces afterward");
    (void) reviewCmd.option("report").takes_value().help("Write a JSON report of every fixture and rule to this path");

    cmdline::App command { "bench" };
    (void) command.description("The agent task benchmark and the review fixtures (tools/bench/README.md)");
    (void) command.subcommand(std::move(tasksCmd));
    (void) command.subcommand(std::move(reviewCmd));
    (void) command.action([&handled, &status](const cmdline::ParsedArgs& arguments) {
        handled = true;
        status = dispatch_bench(arguments);
    });
    return command;
}

} // namespace mcppls::devtools
