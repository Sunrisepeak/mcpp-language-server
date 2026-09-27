// mcppls settings [--format markdown|json] [--lang en|zh-CN]
//
// Prints the configuration registry (0.0.6 plan §9 T1): the same rows `docs/30-settings.md` and its
// zh-CN mirror embed between their `<!-- settings:begin -->` / `<!-- settings:end -->` markers
// (`--format markdown`, the default, `--lang` choosing which of a row's two summaries to print), or
// every field of every row for a machine reader (`--format json`).
export module mcppls.cli.settings;

import std;
import mcpplibs.cmdline;

export namespace mcppls::cli {

mcpplibs::cmdline::App settings_command(bool& handled, int& status);

} // namespace mcppls::cli
