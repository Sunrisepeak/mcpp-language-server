// mcppls cache: what this server's workspace caches hold, and removing one.
//   mcppls cache [--modules] [--format text|json]
//   mcppls cache --clean <workspace-prefix | all>
//
// It is the server's command rather than a developer tool's because the cache is the server's: where
// it lives (platform::dirs) and what a BMI file name means (engine::clangd::module_of_bmi) are this
// program's knowledge, and a second copy of either elsewhere is a copy that drifts. A user looking at
// a slow cold start is the first person who needs it, and they have the server, not the repository.
export module mcppls.cli.cache;

import std;
import mcpplibs.cmdline;

export namespace mcppls::cli {

mcpplibs::cmdline::App cache_command(bool& handled, int& status);

} // namespace mcppls::cli
