// The agent task benchmark and the review fixtures (design doc §10.2, §10.3, work items A2,
// RV5), ported from tools/bench/{run,review}.py -- see tools/bench/README.md.
//
//   mcppls-devtools bench tasks validate [--tasks DIR] [--only ID] [--timeout SECONDS]
//   mcppls-devtools bench tasks run --agent ... --arm ... --repeat N --out FILE ...
//   mcppls-devtools bench review --server PATH [--payload DIR] [--mock-model PATH]
//       [--fixture ID ...] [--timeout SECONDS] [--keep] [--report FILE]
//
// `mcpplibs::cmdline::App::run` dispatches only one level of subcommand (it calls a matched
// subcommand's own `action`, but does not recurse into that subcommand's `run`), and "tasks
// validate"/"tasks run"/"review" are two and three levels deep. So `bench`'s own App carries the
// action, set here, and walks `parsed.subcommand()` by hand down to "validate", "run" or
// "review" before calling the corresponding command function -- the "tasks"/"validate"/"run"
// sub-Apps exist only to parse their own options and print their own --help.
export module mcppls.devtools.bench;

import std;
import mcpplibs.cmdline;

export namespace mcppls::devtools {

mcpplibs::cmdline::App bench_command(bool& handled, int& status);

} // namespace mcppls::devtools
