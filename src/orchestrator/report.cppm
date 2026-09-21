// What a bug report needs (robustness design O2, O3): a diagnostic report of the server and every
// root it serves, and a log file for this process that outlives the editor's output.
export module mcppls.orchestrator.report;

import std;
import nlohmann.json;
import mcppls.engine.payload;

export namespace mcppls::orchestrator {

// The report around the roots' own (Workspace::report): when, which server and client, which payload,
// and the latest lines of the log.
nlohmann::json make_report(nlohmann::json roots, nlohmann::json client, std::string_view engine, const engine::PayloadPaths& payload,
                           bool payloadCorrupt, std::chrono::steady_clock::duration uptime);

// Opens `<cache>/logs/<kind>-<time>-<tag>.log` for this process's log, keeping the newest `keep` logs of
// that kind, and returns its path; empty when it cannot be opened.
std::string open_log_file(std::string_view kind, std::size_t keep = 10);

} // namespace mcppls::orchestrator
