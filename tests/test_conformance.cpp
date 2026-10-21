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

int serve() {
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
            if (!request.contains("id")) continue;
            Json result = nullptr;
            if (method == "initialize") result = Json { { "capabilities", Json { { "experimental", Json { { "cxxModules", Json::object() } } }, { "hoverProvider", true } } } };
            if (method == "textDocument/hover") result = Json { { "contents", Json { { "kind", "plaintext" }, { "value", std::string(159, 'a') + "中文😀" } } } };
            if (method == "cxxModules/report") result = Json {
                { "note", "中文😀" },
                { "roots", Json::array({ Json { { "state", "preparing" }, { "engines", Json::array({ Json {
                    { "name", "clangd" }, { "state", "preparing" },
                    { "details", Json { { "preparation", Json { { "runningUnits", Json::array({ Json { { "module", "sample" }, { "seconds", 12 } } }) } } } } }
                } }) } } }) }
            };
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
    const auto invoke = [&](const std::string& measure, const std::string& evidence) {
        platform::SpawnOptions spawn;
        spawn.program = *runner;
        spawn.arguments = { "run", "--server", self_path(), "--fixture", fixture, "--measure", measure, "--timing-evidence", evidence, "--timeout", "5" };
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
    fs::remove_all(root);
    return report();
}
