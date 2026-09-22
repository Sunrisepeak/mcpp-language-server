// Real-project stress testing (design 2026-09-22, Workstream A): runs mcppls-conformance's
// `stress` check over a matrix of fixtures and client profiles, and prints one summary table.
//
//   mcpp run -p devtools -- stress --payload DIR [--fixture NAME]... [--project xlings|mcpp|self]...
//                                   [--client vscode|neovim|zed|plain]... [--seed N]
//                                   [--measure-dir DIR] [--compare BASE.json]
export module mcppls.devtools.stress;

import std;
import nlohmann.json;
import mcpplibs.cmdline;
import mcppls.base.error;

export namespace mcppls::devtools::stress {

struct RunSpec {
    std::string fixture;
    std::string client;   // "none": this runner's own default profile (mcppls-conformance --client is omitted)
};

// The matrix's rows: `fixtures` and `projects` (mapped to the fixture that exercises each real
// project, when one exists in this checkout) become the fixture axis, in the order named and with
// duplicates dropped; `clients` the client axis, "none" alone when none is given. The fixture axis
// is not validated against the filesystem here — orchestration reports a fixture that does not
// exist, so this stays pure and testable without one.
base::Result<std::vector<RunSpec>> matrix(const std::vector<std::string>& fixtures, const std::vector<std::string>& projects,
                                          const std::vector<std::string>& clients);

struct StressRow {
    std::string fixture;
    std::string client;
    bool ok { true };
    int answered { 0 };
    int empty { 0 };
    int timeout { 0 };
    int error { 0 };
    double p90 { 0.0 };
    double maxStallSeconds { 0.0 };
    std::optional<double> cpuSeconds;
    std::optional<double> rssMB;
    std::string note;   // why there is no row worth trusting: a fixture that does not exist, a
                        // run mcppls-conformance itself could not finish, one with no stress check
};

// The stress check inside one fixture's `--measure` JSON (conformance/README.md): the first check
// of kind "stress", whose "detail" is the summary this program's own conformance runner dumped as
// JSON text (src/bin/conformance.cpp). A measure document with no stress check is a failure here:
// a fixture named in the matrix but whose scenario carries no stress check is a mistake to report,
// not a row to print silently as if it stressed nothing.
base::Result<StressRow> extract_row(const std::string& fixture, const std::string& client, const nlohmann::json& measured);

nlohmann::json to_json(const StressRow& row);
base::Result<std::vector<StressRow>> rows_from_json(const nlohmann::json& value);

// One aligned table: project/scenario/client x answered/empty/timeout/error, p90, max stall, CPU,
// RSS. A row with `note` set prints its note instead of numbers it does not have.
std::string render_table(const std::vector<StressRow>& rows);

// `current` against `baseline`, matched by (fixture, client): a delta for p90, max stall, CPU/min
// and RSS, and which rows are new or missing on either side.
std::string render_comparison(const std::vector<StressRow>& baseline, const std::vector<StressRow>& current);

} // namespace mcppls::devtools::stress

export namespace mcppls::devtools {

mcpplibs::cmdline::App stress_command(bool& handled, int& status);

} // namespace mcppls::devtools
