// `mcppls-devtools bench tasks validate`: for every task, its baseline must fail at least one
// check and its reference-patched copy must pass every check. Ported from
// tools/bench/run.py's `validate_task()`/`cmd_validate()`; see bench/README.md.
export module mcppls.devtools.bench.validate;

import std;
import mcpplibs.cmdline;
import mcppls.devtools.bench.tasks;

export namespace mcppls::devtools::bench {

struct ValidateOutcome {
    bool ok { false };
    std::string detail;
};

// Runs one task's baseline/reference check, entirely under `scratchRoot` (two copies of the
// task's project: "<id>-baseline" and "<id>-patched", removed again before returning).
ValidateOutcome validate_task(const Task& task, const std::string& scratchRoot, int timeoutSeconds);

// `mcppls-devtools bench tasks validate [--tasks DIR] [--only ID] [--timeout SECONDS]`.
int command_validate(const mcpplibs::cmdline::ParsedArgs& arguments);

} // namespace mcppls::devtools::bench
