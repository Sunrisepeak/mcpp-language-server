// Medians of the startup timeline in a directory of conformance --measure files. Ported from
// .github/scripts/timing_summary.py.
//
//   mcppls-devtools measure summary <dir> [--max-cold S] [--max-warm S]
export module mcppls.devtools.measure;

import std;
import mcpplibs.cmdline;
import mcppls.base.error;

export namespace mcppls::devtools::measure {

// The Markdown table timing_summary.py prints: files named cold-<round>.json / warm-<round>.json
// (nightly.yml's naming), medians of `ready`, `first-diagnostics` and `first-navigation` over the
// rounds, with every value, and the summed `failures`. A `directory` with neither kind present is
// not an error -- it renders the header alone, the same as the Python script.
base::Result<std::string> summarize(const std::string& directory);

// What a pre-release gate needs from the same files: the median `first-navigation` of each kind
// against a ceiling in seconds, and every run's `failures` at zero. One sentence per breach; an
// empty answer passes. A budget for a kind with no files is a breach too: a gate that measured
// nothing has not passed.
base::Result<std::vector<std::string>> check_budgets(const std::string& directory, std::optional<double> coldSeconds,
                                                      std::optional<double> warmSeconds);

} // namespace mcppls::devtools::measure

export namespace mcppls::devtools {

mcpplibs::cmdline::App measure_command(bool& handled, int& status);

} // namespace mcppls::devtools
