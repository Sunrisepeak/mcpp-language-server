// `mcppls-devtools bench tokens` (mcpp design 2026-10-28 §11.1): how soon a large file's semantic tokens are the
// compiler's, beside how soon its first completion is, and how many bytes an edit costs -- conformance's
// tokens-large-file fixture, its cold and warm stage on one cache, run `--rounds` times, the medians in one table.
//
//   mcppls-devtools bench tokens --server PATH --payload DIR [--rounds N] [--out FILE] [--compare FILE] [--keep]
//
// `--out` writes every round and the medians; `--compare` reads such a file (another server's, measured the same way)
// and prints its medians beside these. The exit status is 1 when a check of the fixture failed in any round -- its
// budgets are the design's criteria, and CI's bench-validate job gates on them.
export module mcppls.devtools.bench.tokens;

import std;
import nlohmann.json;
import mcpplibs.cmdline;

export namespace mcppls::devtools::bench {

// The numbers of one stage's tokens-latency measure, by name, as the table shows them.
inline constexpr std::array<std::string_view, 10> TOKEN_METRICS {
    "firstCompletion", "firstTokens", "tokensAfterCompletion", "fullBytes", "editFirstAnswer", "editUpdated", "editBytes", "refreshes", "polls",
    "missedRefreshes",
};

// The medians of each metric over `rounds` (each a {"cold": measure, "warm": measure} object), by stage then metric.
nlohmann::json token_medians(const nlohmann::json& rounds);

// The Markdown table of `medians`, with `baseline`'s medians beside them when it is an object.
std::string token_table(const nlohmann::json& medians, const nlohmann::json& baseline);

int command_tokens(const mcpplibs::cmdline::ParsedArgs& args);

} // namespace mcppls::devtools::bench
