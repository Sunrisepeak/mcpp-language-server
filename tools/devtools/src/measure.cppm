// The user-experience scenarios' numbers in directories of conformance --measure files: their
// distributions, for setting the scenarios' budgets, and a candidate's beside a baseline's.
//
//   mcppls-devtools measure budgets <dir>
//   mcppls-devtools measure compare <candidate> <baseline> [--fail-worse]
export module mcppls.devtools.measure;

import std;
import mcpplibs.cmdline;
import mcppls.base.error;

export namespace mcppls::devtools::measure {

// The user-experience scenarios' measurements over many runs (0.0.8 plan, U-*): every conformance --measure file
// under `directory`, at any depth (nightly downloads each round's files into a directory of its own), and for each
// check and each number its `measure` holds -- `completion.p95`, `diagnosticsRefresh`, `engineShare`, ... -- how many
// runs had it, its median, 95th percentile and largest value, and 1.3 times the 95th percentile: what a budget set
// from the measured distribution would be. A Markdown table, sorted by check and number.
base::Result<std::string> budgets(const std::string& directory);

// The same files of a candidate and of a baseline measured beside it (nightly's ux-baseline): for the numbers a release
// is compared on, each side's runs, median and largest value, as a Markdown table, and one sentence per number whose
// median is worse than the baseline's beyond their spread.
struct Comparison {
    std::string table;
    std::vector<std::string> worse;
};
base::Result<Comparison> compare(const std::string& candidate, const std::string& baseline);

} // namespace mcppls::devtools::measure

export namespace mcppls::devtools {

mcpplibs::cmdline::App measure_command(bool& handled, int& status);

} // namespace mcppls::devtools
