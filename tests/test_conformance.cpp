// The real conformance runner writes externally supplied details as bounded UTF-8 JSON.
// This test is also its fake LSP server: no Python, shell, payload or compiler is needed.
import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.os;
import mcppls.base.path;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.platform.dirs;
import mcppls.platform.process;
import mcppls.platform.stdio;
import mcppls.lsp.jsonrpc;

using Json = nlohmann::json;
namespace base = mcppls::base;
namespace platform = mcppls::platform;
namespace fs = platform::fs;
namespace lsp = mcppls::lsp;

namespace {
std::string self_path() {
    const auto arguments = platform::env::arguments();
    const std::string path { arguments.front() };
    return base::normalize_path(base::is_absolute_path(path) ? path : base::join_path(fs::current_directory(), path));
}

// A test build and a regular build use sibling fingerprint buckets (test_model.cpp).
std::optional<std::string> runner_path() {
    if (const auto configured = platform::env::get("MCPPLS_CONFORMANCE"); configured && !configured->empty()) return *configured;
    const std::string triple { base::parent_path(base::parent_path(base::parent_path(self_path()))) };
    for (const auto& bucket : fs::list_directory(triple)) {
        const std::string path { base::join_path(base::join_path(bucket, "bin"), "mcppls-conformance" + std::string { mcppls::os::EXECUTABLE_SUFFIX }) };
        if (fs::is_regular_file(path)) return path;
    }
    return std::nullopt;
}

std::string pcm_bytes(const std::string& source) {
    return source + "\n" + std::string(256, 'x');
}

int serve() {
    const auto arguments = platform::env::arguments();
    std::string cacheMode;
    for (const auto& argument : arguments) if (argument.starts_with("--cache-")) cacheMode = argument;
    const std::string cache { platform::env::get("MCPPLS_CACHE_DIR").value_or("") };
    const std::string cacheSource { base::join_path(cache, "std.cppm") };
    const std::string readCopy { base::join_path(cache, "modules/.owned-payload-v1/generation-1-2/payload.pcm") };
    const bool drain { !cacheMode.empty() || std::ranges::find(arguments, "--drain-ready") != arguments.end()
        || std::ranges::find(arguments, "--drain-never") != arguments.end() || std::ranges::find(arguments, "--drain-missing") != arguments.end()
        || std::ranges::find(arguments, "--drain-restarting") != arguments.end() };
    const bool never { std::ranges::find(arguments, "--drain-never") != arguments.end() };
    const bool missing { std::ranges::find(arguments, "--drain-missing") != arguments.end() };
    const bool restarting { std::ranges::find(arguments, "--drain-restarting") != arguments.end() };
    const bool timelineStates { std::ranges::find(arguments, "--timeline-states") != arguments.end() };
    const bool completionTrace { std::ranges::find(arguments, "--completion-trace") != arguments.end() };
    const bool completionOverflow { std::ranges::find(arguments, "--completion-trace-overflow") != arguments.end() };
    int traceReports { 0 };
    std::optional<Json> lateCompletion;
    int reports { 0 }, changes { 0 }, completions { 0 };
    const auto send = [](const Json& message) { return platform::stdio::write_output(lsp::encode_frame(message)); };
    const auto status = [&](std::string state) { return send(lsp::make_notification("cxxModules/status", Json { { "state", std::move(state) } })); };
    lsp::FrameReader frames;
    while (true) {
        auto bytes = platform::stdio::read_input();
        if (!bytes || bytes->empty()) return 0;
        frames.feed(*bytes);
        while (auto frame = frames.next()) {
            if (!*frame) return 2;
            const Json& request { **frame };
            const std::string method { request.value("method", std::string {}) };
            if (method == "exit") return 0;
            if (timelineStates && method == "initialized") {
                const auto sample = [&](std::string state, int done, int total, std::string issue = {}) {
                    Json issues = Json::array();
                    if (!issue.empty()) issues.push_back(Json { { "code", issue }, { "message", "private-status-text" } });
                    return send(lsp::make_notification("cxxModules/status", Json {
                        { "state", std::move(state) }, { "progress", Json { { "done", done }, { "total", total } } }, { "issues", std::move(issues) }
                    }));
                };
                (void)sample("loading", 0, 3);
                (void)sample("loading", 1, 3); // Progress changes are not state transitions.
                (void)sample("preparing", 1, 3, "preparation-stalled");
                (void)sample("preparing", 2, 3);
                (void)sample("ready", 3, 3);
                (void)sample("error", 3, 3, "mock-state-error");
                (void)sample("ready", 3, 3);
                // Exercise the exact sentinel recorded by Client::reset without a slow OS kill.
                (void)sample("restart", 0, 0);
                (void)sample("ready", 0, 2);
                (void)sample("loading", 0, 2);
                (void)sample("ready", 2, 2);
                (void)sample("ready", 2, 2);
            }
            if (drain && method == "initialized") {
                if (!cacheMode.empty()) {
                    (void)fs::create_directories(base::parent_path(readCopy));
                    (void)fs::write_file(readCopy, cacheSource + "\n" + std::string(32, 'x'));
                }
                (void)status("ready");
                (void)send(lsp::make_request("drain-watch", "client/registerCapability", Json { { "registrations", Json::array({ Json {
                    { "id", "inputs" }, { "method", "workspace/didChangeWatchedFiles" },
                    { "registerOptions", Json { { "watchers", Json::array({ Json { { "globPattern", "**/mcpp.toml" } } }) } } }
                } }) } }));
            }
            if (drain && method == "workspace/didChangeWatchedFiles") {
                ++changes;
                reports = 0;
                (void)status("loading");
                (void)status("ready");
            }
            if (!request.contains("method")) continue;
            if (!request.contains("id")) continue;
            Json result = nullptr;
            if (method == "initialize") result = Json { { "capabilities", Json { { "experimental", Json { { "cxxModules", Json::object() } } }, { "hoverProvider", true } } } };
            if (method == "textDocument/completion") {
                ++completions;
                if (completionTrace && completions == 1) std::this_thread::sleep_for(std::chrono::milliseconds { 30 });
                if (completionOverflow && completions == 270) std::this_thread::sleep_for(std::chrono::milliseconds { 500 });
                if (completionTrace && completions == 2) {
                    (void)send(lsp::make_error(request["id"], -32001, "private-completion-error"));
                    continue;
                }
                if (completionTrace && completions == 3) {
                    lateCompletion = request["id"];
                    continue; // It arrives only with the post-sampling report, after statistics are frozen.
                }
                result = Json { { "isIncomplete", completions == 1 },
                    { "items", Json::array({ Json { { "label", "private-completion-text" } } }) } };
                if (completions == 2) result["items"].push_back(Json { { "label", "second-private-item" } });
            }
            if (method == "textDocument/hover") result = Json { { "contents", Json { { "kind", "plaintext" }, { "value", std::string(159, 'a') + "中文😀" } } } };
            if (method == "cxxModules/report") result = Json {
                { "note", "中文😀" },
                { "roots", Json::array({ Json { { "state", "preparing" }, { "engines", Json::array({ Json {
                    { "name", "clangd" }, { "state", "preparing" },
                    { "details", Json { { "preparation", Json { { "runningUnits", Json::array({ Json { { "module", "sample" }, { "seconds", 12 } } }) } } } } }
                } }) } } }) }
            };
            if ((completionTrace || completionOverflow) && method == "cxxModules/report") {
                ++traceReports;
                if (traceReports == 2 && lateCompletion) {
                    (void)send(lsp::make_result(*lateCompletion, Json { { "items", Json::array({ Json { { "label", "private-late-item" } } }) } }));
                    lateCompletion.reset();
                }
                result["traceObservation"] = Json { { "completionRequests", completions }, { "reportRequests", traceReports } };
                if (completionTrace) {
                    result["roots"][0]["requests"] = Json { { "textDocument/completion", Json {
                        { "count", 10 + completions }, { "empty", 0 }, { "errors", completions > 0 ? 1 : 0 }, { "cancelled", 0 },
                        { "p50Ms", 12 }, { "p95Ms", 34 }, { "maxMs", 8765 }, { "engineAnswered", 7 },
                        { "engineP50Ms", 10 }, { "engineP95Ms", 30 }, { "overheadP50Ms", 2 }, { "overheadP95Ms", 4 },
                        { "answeredBy", Json { { "clangd", 7 } } }, { "lastAt", "fake-at" }, { "file", "private-method-file" }
                    } } };
                    result["roots"][0]["eventTotals"] = Json { { "slow-request", traceReports == 1 ? 5 : 8 } };
                    Json slowEvents = Json::array();
                    for (int index { 0 }; index < 4; ++index) slowEvents.push_back(Json {
                        { "at", std::format("fake-at-{}", index) }, { "kind", "slow-request" },
                        { "detail", Json { { "method", "textDocument/completion" }, { "ms", 8765 + index },
                            { "outcome", "result" }, { "answeredBy", "clangd" }, { "file", "private-slow-request-file" } } }
                    });
                    result["roots"][0]["events"] = std::move(slowEvents);
                }
            }
            if (drain && method == "cxxModules/report") {
                const bool busy { !cacheMode.empty() ? (++reports < 2 || cacheMode == "--cache-never")
                                                   : !restarting && (never || ++reports < 3) };
                if (!cacheMode.empty() && !busy) {
                    (void)fs::write_file(readCopy, pcm_bytes(cacheSource) + (cacheMode == "--cache-new" ? "new-build" : ""));
                }
                const bool preparationBusy { cacheMode.empty() && busy };
                Json details {
                    { "generation", changes + 1 },
                    { "handshakeDone", !restarting }, { "accepting", !restarting }, { "restartScheduled", restarting },
                    { "preparation", Json { { "done", preparationBusy ? 0 : 140 }, { "wanted", 140 }, { "running", preparationBusy ? 1 : 0 },
                        { "completedModuleBuilds", changes == 0 ? 5 : 10 } } },
                    { "implementationIndex", Json { { "sweepComplete", true }, { "sweepScheduled", false }, { "queued", 0 }, { "running", 0 } } },
                    { "backgroundIndex", Json { { "active", false } } },
                    { "pendingRequests", 0 }, { "filesAwaitingDiagnostics", busy ? 11 : 0 }, { "deferredRequests", 0 }, { "heldRequests", 0 }
                };
                if (missing || cacheMode == "--cache-missing") details.erase("backgroundIndex");
                result = Json { { "roots", Json::array({ Json { { "state", "ready" },
                    { "engines", Json::array({ Json { { "name", "clangd" }, { "state", restarting ? "starting" : "ready" }, { "details", std::move(details) } } }) }
                } }) } };
            }
            if (!platform::stdio::write_output(lsp::encode_frame(lsp::make_result(request["id"], result)))) return 2;
        }
    }
}
} // namespace

int main() {
    const auto arguments = platform::env::arguments();
    if (arguments.size() > 1 && arguments[1] == "serve") return serve();
    if (arguments.size() > 1 && arguments[1] == "--invalid-output") {
        (void)platform::stdio::write_output("external-\xFF-output");
        return 0;
    }
    using namespace mcppls::testing;
    const auto runner = runner_path();
    if (!runner) {
        if (platform::env::get("CI").value_or("") == "true") {
            expect(false) << "CI requires a built mcppls-conformance runner for the artifact regression";
        } else {
            std::println("note: build mcppls-conformance before mcpp test to exercise its UTF-8 artifact regression");
        }
        return report();
    }
    const std::string root { base::join_path(platform::dirs::temp_directory(),
        std::format("mcppls-test-conformance-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
    const std::string fixture { base::join_path(root, "fixture") };
    expect(fs::create_directories(fixture).has_value());
    const Json scenario {
        { "name", "timing" },
        { "checks", Json::array({
            Json { { "id", "UTF8" }, { "kind", "hover-contains" }, { "file", "main.cpp" }, { "at", Json::array({ 0, 0 }) }, { "expect", "中文" } },
            Json { { "id", "EXTERNAL" }, { "kind", "cli" }, { "args", Json::array({ "--invalid-output" }) }, { "optional", true } },
            Json { { "id", "TIMELINE" }, { "kind", "timeline" } }
        }) }
    };
    expect(fs::write_file(base::join_path(fixture, "scenario.json"), scenario.dump()).has_value());
    expect(fs::write_file(base::join_path(fixture, "main.cpp"), "int main() {}\n").has_value());
    const auto invoke = [&](const std::string& measure, const std::string& evidence, const std::string& cache = std::string {}, bool warm = false) {
        platform::SpawnOptions spawn;
        spawn.program = *runner;
        spawn.arguments = { "run", "--server", self_path(), "--fixture", fixture, "--measure", measure, "--timing-evidence", evidence, "--timeout", "5" };
        if (!cache.empty()) spawn.arguments.insert(spawn.arguments.end(), { "--cache-dir", cache });
        if (warm) spawn.arguments.push_back("--expect-warm");
        return platform::run(std::move(spawn), std::chrono::seconds { 30 });
    };
    const std::string measure { base::join_path(root, "measure.json") };
    const std::string evidence { base::join_path(root, "evidence.json") };
    "UTF-8 clipping and malformed external bytes produce valid measurement and evidence JSON"_test = [&] {
        const auto result = invoke(measure, evidence);
        expect(result.has_value());
        if (!result) return;
        expect(result->exitCode == 0) << result->output << result->error;
        const auto text = fs::read_file(measure);
        expect(text.has_value());
        if (!text) return;
        const Json summary = Json::parse(*text, nullptr, false);
        expect(!summary.is_discarded());
        if (summary.is_discarded()) return;
        expect(summary["failures"] == 0);
        expect(summary["checks"][0]["detail"] == std::string(159, 'a'));
        expect(summary["checks"][0]["ok"] == true);
        expect(summary["checks"][1]["detail"].get<std::string>().find("external-\xEF\xBF\xBD-output") != std::string::npos);
        const Json& timeline { summary["checks"][2]["measure"] };
        expect(timeline["longestStallToSeconds"].get<double>() >= timeline["longestStallFromSeconds"].get<double>());
        expect(timeline["longestStallActivity"]["events"].is_array());
        expect(timeline["longestEngineInactivitySeconds"].is_null()) << "missing engine logs are unknown, never zero";
        expect(timeline["activityAtEnd"]["engines"][0]["preparation"]["runningUnits"][0]["module"] == "sample");
        // Work evidence does not invent completed nodes or relax the original interval metric.
        expect(timeline["longestStallSeconds"].get<double>() > 0.0);
        const auto evidenceText = fs::read_file(evidence);
        expect(evidenceText.has_value());
        if (evidenceText) {
            const Json document = Json::parse(*evidenceText, nullptr, false);
            expect(!document.is_discarded());
            if (!document.is_discarded()) expect(document["report"]["note"] == "中文😀");
        }
    };
    "unwritable measurement is a reported run failure"_test = [&] {
        const auto result = invoke(root, evidence);
        expect(result.has_value());
        if (result) {
            expect(result->exitCode == 1) << result->output << result->error;
            expect(result->output.find("conformance: cannot write " + root) != std::string::npos);
        }
    };
    "unwritable timing evidence is a reported run failure"_test = [&] {
        const auto result = invoke(measure, root);
        expect(result.has_value());
        if (result) {
            expect(result->exitCode == 1) << result->output << result->error;
            expect(result->output.find("conformance: cannot write " + root) != std::string::npos);
        }
    };
    "an inactivity gate falls back to the original node gap when engine evidence is missing"_test = [&] {
        Json withoutLogs = scenario;
        withoutLogs["checks"][2]["budget"] = Json { { "maxEngineInactivitySeconds", 0 } };
        expect(fs::write_file(base::join_path(fixture, "scenario.json"), withoutLogs.dump()).has_value());
        const auto result = invoke(measure, evidence);
        expect(result.has_value());
        if (result) {
            expect(result->exitCode == 1) << result->output << result->error;
            expect(result->output.find("engine work evidence unavailable") != std::string::npos);
        }
    };
    "a failed timeline preserves exact counted transitions and restart boundaries"_test = [&] {
        const Json sample { { "name", "timing" }, { "server-arguments", Json::array({ "--timeline-states" }) },
            { "checks", Json::array({
                Json { { "id", "PUMP" }, { "kind", "hover-contains" }, { "file", "main.cpp" }, { "at", Json::array({ 0, 0 }) }, { "expect", "中文" } },
                Json { { "id", "STATES" }, { "kind", "timeline" }, { "budget", Json { { "maxChangesPerMinute", 5 } } } }
            }) }
        };
        expect(fs::write_file(base::join_path(fixture, "scenario.json"), sample.dump()).has_value());
        const auto result = invoke(measure, evidence);
        expect(fatal(result.has_value()));
        expect(result->exitCode == 1) << result->output << result->error;
        const auto document = fs::read_file(measure);
        expect(fatal(document.has_value()));
        const Json summary = Json::parse(*document);
        expect(summary["checks"][0]["ok"] == true);
        expect(summary["checks"][1]["ok"] == false);
        const Json& timeline = summary["checks"][1]["measure"];
        expect(timeline["stateChanges"] == 6 && timeline["busiestMinuteChanges"] == 6);
        expect(fatal(timeline.contains("stateTransitionTrace") && timeline.contains("stateTransitionStatusSamples")
                     && timeline.contains("busiestMinuteWindow"))) << "the failing gate must retain its transition evidence";
        expect(timeline["stateTransitionStatusSamples"] == 12 && timeline["errorStates"] == 1);
        const Json& trace = timeline["stateTransitionTrace"];
        expect(fatal(trace.is_array() && trace.size() == 9));
        expect(trace[0]["kind"] == "initial" && trace[0]["from"].is_null() && trace[0]["counted"] == false);
        expect(trace[1]["from"] == "loading" && trace[1]["to"] == "preparing");
        expect(trace[1]["done"] == 1 && trace[1]["total"] == 3 && trace[1]["issues"] == Json::array({ "preparation-stalled" }));
        expect(trace[5]["kind"] == "restart" && trace[5]["to"] == "restart" && trace[5]["counted"] == false);
        expect(trace[6]["kind"] == "initial" && trace[6]["from"].is_null() && trace[6]["counted"] == false);
        Json counted = Json::array();
        std::int64_t previous { -1 };
        for (std::size_t i { 0 }; i < trace.size(); ++i) {
            const auto at { trace[i]["sinceStartNanoseconds"].get<std::int64_t>() };
            expect(at >= previous && trace[i]["seconds"].get<double>() >= 0.0);
            previous = at;
            if (trace[i]["counted"].get<bool>()) counted.push_back(i);
        }
        expect(counted.size() == timeline["stateChanges"].get<std::size_t>());
        const Json& window = timeline["busiestMinuteWindow"];
        expect(window["traceIndices"] == counted && window["count"] == counted.size());
        const auto start { window["startNanoseconds"].get<std::int64_t>() };
        const auto end { window["endNanosecondsExclusive"].get<std::int64_t>() };
        expect(end - start == 60'000'000'000LL);
        for (const auto& index : window["traceIndices"]) {
            const auto at { trace[index.get<std::size_t>()]["sinceStartNanoseconds"].get<std::int64_t>() };
            expect(at >= start && at < end);
        }
        expect(trace.dump().find("private-status-text") == std::string::npos) << "keep only issue codes, no full status";
    };
    "typing trace preserves per-word requests and records the original completion metadata"_test = [&] {
        const Json sample { { "name", "timing" }, { "checks", Json::array({ Json {
            { "id", "WORDS" }, { "kind", "typing" }, { "file", "main.cpp" }, { "seconds", 1 }, { "hz", 5 },
            { "completion-every-ms", 0 }, { "reask", "incomplete" },
            { "scripts", Json::array({ Json { { "line", 0 }, { "text", "abc" } } }) }
        } }) } };
        expect(fs::write_file(base::join_path(fixture, "scenario.json"), sample.dump()).has_value());
        const auto result = invoke(measure, evidence);
        expect(fatal(result.has_value()));
        expect(result->exitCode == 0) << result->output << result->error;
        const auto document = fs::read_file(measure);
        expect(fatal(document.has_value()));
        const Json summary = Json::parse(*document);
        const Json& typed = summary["checks"][0]["measure"];
        expect(typed["typedWords"] == 1 && typed["askedAsTyped"] == 2 && typed["askedForIncomplete"] == 1);
        expect(typed["completions"] == 2 && typed["answered"] == 2);
        expect(typed["wordTraceLimit"] == 256 && typed["wordTraceDropped"] == 0);
        const Json& trace = typed["wordTrace"];
        expect(fatal(trace.is_array() && trace.size() == 2));
        expect(trace[0]["word"] == 1 && trace[1]["word"] == 1);
        expect(trace[0]["reason"] == "first" && trace[1]["reason"] == "incomplete");
        expect(trace[0]["id"].get<std::int64_t>() + 1 == trace[1]["id"].get<std::int64_t>());
        expect(trace[0]["documentVersion"].get<int>() + 1 == trace[1]["documentVersion"].get<int>());
        expect(trace[0]["position"] == Json({ { "line", 0 }, { "character", 1 } }));
        expect(trace[1]["position"] == Json({ { "line", 0 }, { "character", 2 } }));
        expect(trace[0]["isIncomplete"] == true && trace[1]["isIncomplete"] == false);
        expect(trace[0]["items"] == 1 && trace[1]["items"] == 2);
        expect(trace[0]["sentSeconds"].get<double>() >= 0.0);
        expect(trace[1]["sentSeconds"].get<double>() >= trace[0]["sentSeconds"].get<double>());
        for (const auto& entry : trace) {
            expect(entry["responseSeconds"].is_number() && entry["responseSeconds"].get<double>() >= 0.0);
            expect(entry["error"] == false);
        }
        expect(trace.dump().find("private") == std::string::npos) << "trace keeps counts, never completion text";
    };
    "failed periodic typing keeps exact completion evidence at the original statistics boundary"_test = [&] {
        const Json sample { { "name", "timing" }, { "server-arguments", Json::array({ "--completion-trace" }) },
            { "checks", Json::array({ Json { { "id", "PERIODIC" }, { "kind", "typing" }, { "file", "main.cpp" },
                { "seconds", 1 }, { "hz", 5 }, { "completion-every-ms", 100 }, { "requestTimeout", 0 },
                { "scripts", Json::array({ Json { { "line", 0 }, { "text", "abc" } } }) },
                { "budget", Json { { "max", 0 } } }
            } }) } };
        expect(fs::write_file(base::join_path(fixture, "scenario.json"), sample.dump()).has_value());
        const auto result = invoke(measure, evidence);
        expect(fatal(result.has_value()));
        expect(result->exitCode == 1) << result->output << result->error;
        const auto text = fs::read_file(measure);
        expect(fatal(text.has_value()));
        const Json summary = Json::parse(*text);
        expect(summary["checks"][0]["ok"] == false);
        const Json& typed = summary["checks"][0]["measure"];
        expect(typed["reask"] == "" && !typed.contains("wordTrace"));
        expect(fatal(typed.contains("completionTrace")));
        const Json& trace = typed["completionTrace"];
        const Json& entries = trace["entries"];
        expect(trace["origin"] == "typing-start" && trace["population"] == "original-completion-statistics");
        expect(trace["limit"] == 256 && trace["dropped"] == 0);
        expect(fatal(entries.size() >= 3 && entries.size() == typed["completions"].get<std::size_t>()));
        expect(trace["total"] == entries.size());
        expect(entries[0]["items"] == 1 && entries[0]["isIncomplete"] == true);
        expect(entries[1]["error"] == true && entries[1]["items"].is_null());
        expect(entries[2]["answeredNanoseconds"].is_null() && entries[2]["elapsedNanoseconds"].is_null());
        expect(entries[2]["elapsedSeconds"].is_null()) << "the late response must not rewrite the original timeout";
        std::vector<double> answered;
        std::size_t errors { 0 }, unanswered { 0 };
        std::int64_t previousId { -1 }, previousSent { -1 };
        for (std::size_t index { 0 }; index < entries.size(); ++index) {
            const auto& entry = entries[index];
            expect(entry["index"] == index);
            expect(entry["id"].is_number_integer() && entry["sentNanoseconds"].is_number_integer());
            const auto id = entry["id"].get<std::int64_t>();
            const auto sent = entry["sentNanoseconds"].get<std::int64_t>();
            expect(id > previousId && sent >= previousSent && sent >= 0);
            previousId = id;
            previousSent = sent;
            if (entry["answeredNanoseconds"].is_null()) { ++unanswered; continue; }
            expect(entry["answeredNanoseconds"].is_number_integer() && entry["elapsedNanoseconds"].is_number_integer());
            expect(entry["answeredNanoseconds"].get<std::int64_t>() - sent == entry["elapsedNanoseconds"].get<std::int64_t>());
            expect(entry["elapsedNanoseconds"].get<std::int64_t>() >= 0);
            if (entry["error"].get<bool>()) ++errors;
            else answered.push_back(entry["elapsedSeconds"].get<double>());
        }
        expect(unanswered == 1 && typed["clientTimeouts"] == unanswered);
        expect(errors == 1 && typed["errors"] == errors);
        std::ranges::sort(answered);
        expect(fatal(!answered.empty()));
        expect(typed["answered"] == answered.size() && typed["completion"]["n"] == answered.size());
        for (const auto& [field, fraction] : std::array<std::pair<std::string_view, double>, 2> {{ { "p50", 0.5 }, { "p95", 0.95 } }}) {
            const auto rank = static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(answered.size())));
            expect(typed["completion"][std::string { field }] == answered[rank - 1]);
        }
        expect(typed["completion"]["max"] == answered.back());
        expect(trace["slowestSuccessful"]["elapsedSeconds"] == answered.back());
        expect(trace.dump().find("private") == std::string::npos && trace.dump().find("main.cpp") == std::string::npos);
        const Json& server = typed["serverCompletionEvidence"];
        expect(server["requestsBefore"]["count"] == 10);
        expect(server["requestsAfter"]["count"] == 10 + entries.size());
        expect(server["requestsAfter"]["engineP95Ms"] == 30 && server["requestsAfter"]["overheadP95Ms"] == 4);
        expect(server["recentSampleLimit"] == 256);
        const Json& slow = server["slowRequests"];
        expect(slow["totalBefore"] == 5 && slow["totalAfter"] == 8 && slow["added"] == 3);
        expect(slow["afterRecentCount"] == 4 && slow["droppedByTraceLimit"] == 0);
        expect(slow["newRetained"] == 3 && slow["newNotRetained"] == 0);
        expect(slow["eventsAfter"].size() == 4 && slow["eventsAfter"][0].size() == 5);
        expect(slow["eventsAfter"][0]["at"] == "fake-at-0" && slow["eventsAfter"][0]["ms"] == 8765);
        expect(server.dump().find("private") == std::string::npos) << "only public method statistics and whitelisted event fields";
        const auto artifact = fs::read_file(evidence);
        expect(fatal(artifact.has_value()));
        const Json observed = Json::parse(*artifact)["report"]["traceObservation"];
        expect(observed["completionRequests"] == entries.size() && observed["reportRequests"] == 3)
            << "tracing must add neither completion requests nor reports";
    };
    "truncated completion traces still identify the original slowest response"_test = [&] {
        const Json sample { { "name", "timing" }, { "server-arguments", Json::array({ "--completion-trace-overflow" }) },
            { "checks", Json::array({ Json { { "id", "OVERFLOW" }, { "kind", "typing" }, { "file", "main.cpp" },
                { "seconds", 1 }, { "hz", 1000 }, { "completion-every-ms", 0 }, { "reask", "always" }, { "requestTimeout", 5 },
                { "scripts", Json::array({ Json { { "line", 0 }, { "text", std::string(300, 'a') } } }) }
            } }) } };
        expect(fs::write_file(base::join_path(fixture, "scenario.json"), sample.dump()).has_value());
        const auto result = invoke(measure, evidence);
        expect(fatal(result.has_value()));
        expect(result->exitCode == 0) << result->output << result->error;
        const auto text = fs::read_file(measure);
        expect(fatal(text.has_value()));
        const Json typed = Json::parse(*text)["checks"][0]["measure"];
        expect(fatal(typed.contains("completionTrace")));
        const Json& trace = typed["completionTrace"];
        const auto total = typed["completions"].get<std::size_t>();
        expect(total >= 300 && trace["entries"].size() == 256);
        expect(trace["total"] == total && trace["dropped"] == total - 256);
        expect(trace["slowestSuccessful"]["elapsedSeconds"] == typed["completion"]["max"]);
        expect(trace["slowestAnswered"]["elapsedSeconds"] == typed["completion"]["max"]);
        expect(trace["slowestSuccessful"]["index"].get<std::size_t>() >= 256)
            << "the deliberately slow response beyond the prefix must survive truncation";
        expect(typed["answered"] == total && typed["completion"]["n"] == total);
        expect(trace.dump().find("private") == std::string::npos);
        const Json& server = typed["serverCompletionEvidence"];
        expect(server["requestsBefore"].is_null() && server["requestsAfter"].is_null());
        expect(server["slowRequests"]["eventsAfter"].is_null() && server["slowRequests"]["newRetained"].is_null());
    };
    "module reuse drains partial read copies and preserves cold, rebuild and missing-state checks"_test = [&] {
        // Owned payloads are Linux's cache representation; its source scanner uses POSIX paths.
        if constexpr (mcppls::os::FAMILY != mcppls::os::Family::linux) return;
        const std::string cache { base::join_path(root, "module-cache") };
        const std::string source { base::join_path(cache, "std.cppm") };
        const std::string stable { base::join_path(cache, "modules/.owned-payload-v1/generation-0-1/payload.pcm") };
        for (const std::string mode : { "--cache-copy", "--cache-cold", "--cache-new", "--cache-missing", "--cache-never" }) {
            fs::remove_all(cache);
            expect(fs::create_directories(base::parent_path(stable)).has_value());
            expect(fs::write_file(source, "export module std;\n").has_value());
            const bool cold { mode == "--cache-cold" };
            if (!cold) expect(fs::write_file(stable, pcm_bytes(source)).has_value());
            const Json sample { { "name", "timing" }, { "server-arguments", Json::array({ mode }) },
                { "checks", Json::array({ Json { { "id", "SC4" }, { "kind", "module-cache-reused" }, { "module", "std" } } }) } };
            expect(fs::write_file(base::join_path(fixture, "scenario.json"), sample.dump()).has_value());
            const auto result = invoke(measure, evidence, cache, !cold);
            expect(fatal(result.has_value()));
            const bool good { mode == "--cache-copy" || cold };
            expect(result->exitCode == (good ? 0 : 1)) << mode << result->output << result->error;
            const auto document = fs::read_file(measure);
            expect(fatal(document.has_value()));
            const Json summary = Json::parse(*document);
            const Json& check = summary["checks"][0];
            expect(check["ok"] == good) << mode << check.dump();
            expect(fatal(check.contains("measure") && check["measure"].contains("cacheDrain"))) << mode;
            const Json& drained = check["measure"]["cacheDrain"];
            const bool drainedReady { good || mode == "--cache-new" };
            expect(drained["ready"] == drainedReady) << mode << drained.dump();
            if (drainedReady) {
                expect(drained["seconds"].get<double>() >= 1.0) << "snapshot must wait for the fake's second report";
                expect(drained["activityAtEnd"]["engines"][0]["filesAwaitingDiagnostics"] == 0);
            } else {
                expect(check["detail"].get<std::string>().find("module cache work did not finish") != std::string::npos);
            }
            if (mode == "--cache-new") expect(check["detail"].get<std::string>().find("added ") != std::string::npos);
        }
    };
    const auto drain_scenario = [&](std::string mode, int bound) {
        const Json sample { { "name", "timing" }, { "server-arguments", Json::array({ std::move(mode) }) },
            { "checks", Json::array({ Json { { "id", "U8-config" }, { "kind", "graph-edit" }, { "action", "config" },
                { "file", "mcpp.toml" }, { "replace", Json { { "from", "release" }, { "with", "dev" } } },
                { "background-work-seconds", bound }, { "wait-seconds", 2 }, { "budget", Json { { "revertNewBmi", 0 } } }
            } }) } };
        expect(fs::write_file(base::join_path(fixture, "scenario.json"), sample.dump()).has_value());
        expect(fs::write_file(base::join_path(fixture, "mcpp.toml"), "default-profile = \"release\"\n").has_value());
        return invoke(measure, evidence);
    };
    "configuration undo drains explicit unfinished preparation despite a ready status"_test = [&] {
        const auto result = drain_scenario("--drain-ready", 10);
        expect(result.has_value());
        if (!result) return;
        expect(result->exitCode == 0) << result->output << result->error;
        const auto document = fs::read_file(measure);
        expect(document.has_value());
        if (!document) return;
        const Json summary = Json::parse(*document);
        const Json& sample = summary["checks"][0]["measure"];
        for (const std::string phase : { "beforeChangeDrain", "beforeRevertDrain", "afterRevertDrain" }) {
            expect(sample[phase]["ready"] == true);
            expect(sample[phase]["seconds"].get<double>() >= 1.0) << "the mock's pending reports must not be skipped";
            expect(sample[phase]["activityAtEnd"]["engines"][0]["preparation"]["done"] == 140);
        }
        expect(sample["revertNewBmi"] == 0);
    };
    "ready status cannot pass a missing or timed out background work report"_test = [&] {
        for (const std::string mode : { "--drain-never", "--drain-missing", "--drain-restarting" }) {
            const auto result = drain_scenario(mode, 0);
            expect(result.has_value());
            if (!result) continue;
            expect(result->exitCode == 1) << result->output << result->error;
            expect(result->output.find("background work did not finish within its bound") != std::string::npos);
            const auto document = fs::read_file(measure);
            expect(document.has_value());
            if (document) expect(Json::parse(*document)["checks"][0]["measure"]["beforeChangeDrain"]["ready"] == false);
        }
    };
    fs::remove_all(root);
    return report();
}
