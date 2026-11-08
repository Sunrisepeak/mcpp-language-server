// The measurements as one program, `mcppls-bench` (README.md): the cases are the objects of
// startup.cpp, tokens.cpp, review.cpp and tasks.cpp. They run against what the environment of the
// conformance cases names (MCPPLS_CTS_SERVER, MCPPLS_CTS_PAYLOAD, MCPPLS_CTS_RUNNER,
// MCPPLS_CTS_FIXTURES; the head of mcppls.harness), from the repository's root, and further:
//   MCPPLS_BENCH_MOCK_MODEL  mcppls-mock-model, for the review fixtures that script a model
//                            (default: beside this program)
//   MCPPLS_BENCH_AGENT       claude-code or copilot-cli: the agent the agent runs use (none: skipped)
//   MCPPLS_BENCH_ARM         grep, clangd-lsp, mcppls-lsp or mcppls-mcp (default mcppls-lsp)
//   MCPPLS_BENCH_REPEAT      runs of each task (default 5)
//   MCPPLS_BENCH_RUNS        the file the agent's runs are recorded in (default target/bench/agent-runs.json)
import mcpplibs.mtest;

int main(int argc, char** argv) {
    return mcpplibs::mtest::run(argc, argv);
}
