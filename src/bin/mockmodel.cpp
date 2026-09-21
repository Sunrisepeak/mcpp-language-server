// mcppls-mock-model: a fake mcppls-model gateway for tests, speaking model-gateway/PROTOCOL.md's
// line protocol over stdin/stdout. Its answers come from a small JSON script
// (MCPPLS_MOCK_MODEL_SCRIPT or --script <path>), so a test can script a valid answer, schema-invalid
// output, findings that cite unknown evidence, a slow answer (for timeout tests), or exiting before
// answering at all. An optional trace file (MCPPLS_MOCK_MODEL_TRACE or --trace <path>) records which
// methods were received, one JSON object per line, so a test can confirm a `cancel` arrived without
// depending on timing. Each `complete` runs on its own thread, so a slow one does not hold up a later
// request's answer (the reference gateway is a single worker, but a test needs the ability to make
// two pipelined calls answer out of send-order to be a meaningful check of id-based correlation) and
// so `cancel` can be honored while an answer is still being delayed, in ~50ms slices, as PROTOCOL.md
// describes for the real gateway's own polling.
//
//   mcppls-mock-model [--script <path>] [--trace <path>]
//   mcppls-mock-model --self-check <path>   # exercises the script parser directly; see tests/test_model.cpp
//
// Script fields, all optional: "model" (the id `initialize` advertises and `complete` echoes back,
// default "mock-model"), "delayMs" (added before every answer), "delayFirstMs" (added on top of
// delayMs, but only for the first `complete` this process receives, so a test can make one of two
// concurrent calls answer after the other without slowing every call down), "exitBeforeResponse"
// (the process exits, unanswered, once that call's delay has passed without being cancelled), "error"
// ({code, message} to answer with instead of a result), "result" (the `complete` result payload:
// content/json/finishReason/usage; "model" is filled in from the request when absent).
import std;
import nlohmann.json;
import mcppls.platform.env;
import mcppls.platform.fs;

using Json = nlohmann::json;
namespace platform = mcppls::platform;

namespace {

struct Script {
    std::string model { "mock-model" };
    int delayMs { 0 };
    int delayFirstMs { 0 };
    bool exitBeforeResponse { false };
    std::optional<Json> error;
    Json result = Json { { "content", "" },
                         { "finishReason", "stop" },
                         { "usage", Json { { "inputTokens", 0 }, { "outputTokens", 0 } } } };
};

// Exposed for --self-check as well as the real stdio loop, so the parser has a code path a test can
// reach even where spawning the built binary is not possible (see tests/test_model.cpp).
Script load_script(const std::string& path) {
    Script script;
    if (path.empty()) return script;
    auto text = platform::fs::read_file(path);
    if (!text) return script;
    Json document = Json::parse(*text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) return script;
    script.model = document.value("model", script.model);
    script.delayMs = document.value("delayMs", script.delayMs);
    script.delayFirstMs = document.value("delayFirstMs", script.delayFirstMs);
    script.exitBeforeResponse = document.value("exitBeforeResponse", script.exitBeforeResponse);
    if (const auto found = document.find("error"); found != document.end() && found->is_object()) script.error = *found;
    if (const auto found = document.find("result"); found != document.end() && found->is_object()) script.result = *found;
    return script;
}

std::string tracePath;
std::mutex traceMutex;

void trace(const Json& record) {
    if (tracePath.empty()) return;
    std::lock_guard<std::mutex> lock { traceMutex };
    std::ofstream stream { tracePath, std::ios::app };
    if (stream) stream << record.dump() << "\n";
}

std::mutex outputMutex;

void write_line(const Json& message) {
    std::lock_guard<std::mutex> lock { outputMutex };
    std::println("{}", message.dump());
    std::cout.flush();
}

Json initialize_result(const Script& script) {
    Json models = Json::array();
    if (!script.model.empty()) models.push_back(Json { { "id", script.model }, { "provider", "mock" }, { "structuredOutput", true } });
    return Json { { "protocol", 1 }, { "gateway", Json { { "name", "mcppls-mock-model" }, { "version", "0.0.0-mock" } } }, { "models", std::move(models) } };
}

struct Pending {
    std::shared_ptr<std::atomic<bool>> cancelled;
};

std::mutex pendingMutex;
std::map<long long, Pending> pending;
std::atomic<bool> firstClaimed { false };

long long id_key(const Json& id) { return id.is_number_integer() ? id.get<long long>() : -1; }

void handle_cancel(const Json& params) {
    const auto found = params.find("id");
    if (found == params.end()) return;
    trace(Json { { "method", "cancel" } });
    const long long key { id_key(*found) };
    std::lock_guard<std::mutex> lock { pendingMutex };
    if (const auto entry = pending.find(key); entry != pending.end()) entry->second.cancelled->store(true);
}

void handle_complete(Json id, const Json& params, const Script& script) {
    trace(Json { { "method", "complete" }, { "model", params.value("model", std::string {}) } });
    const bool isFirst { !firstClaimed.exchange(true) };
    const int delay { script.delayMs + (isFirst ? script.delayFirstMs : 0) };

    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    const long long key { id_key(id) };
    {
        std::lock_guard<std::mutex> lock { pendingMutex };
        pending[key] = Pending { cancelled };
    }
    for (int waited { 0 }; waited < delay && !cancelled->load(); waited += 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds { std::min(50, delay - waited) });
    }
    {
        std::lock_guard<std::mutex> lock { pendingMutex };
        pending.erase(key);
    }

    if (cancelled->load()) {
        write_line(Json { { "id", id }, { "error", Json { { "code", 1003 }, { "message", "cancelled" } } } });
        return;
    }
    if (script.exitBeforeResponse) std::_Exit(0);   // PROTOCOL.md "exit": gone without answering, whatever else is pending
    if (script.error) {
        write_line(Json { { "id", id }, { "error", *script.error } });
        return;
    }
    Json result = script.result;
    if (!result.contains("model")) result["model"] = params.value("model", script.model);
    write_line(Json { { "id", id }, { "result", std::move(result) } });
}

int self_check(const std::string& path) {
    const Script defaults { load_script("") };
    if (defaults.model != "mock-model" || defaults.delayMs != 0 || defaults.exitBeforeResponse) {
        std::println(std::cerr, "self-check: defaults are wrong");
        return 1;
    }
    if (path.empty()) return 0;   // no script file given: only the defaults were being checked
    const Script script { load_script(path) };
    Json record { { "model", script.model },
                  { "delayMs", script.delayMs },
                  { "delayFirstMs", script.delayFirstMs },
                  { "exitBeforeResponse", script.exitBeforeResponse },
                  { "hasError", script.error.has_value() },
                  { "result", script.result } };
    std::println("{}", record.dump());
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    std::string scriptPath { platform::env::get("MCPPLS_MOCK_MODEL_SCRIPT").value_or("") };
    tracePath = platform::env::get("MCPPLS_MOCK_MODEL_TRACE").value_or("");
    for (int i { 1 }; i < argc; ++i) {
        const std::string_view argument { argv[i] };
        if (argument == "--script" && i + 1 < argc) scriptPath = argv[++i];
        else if (argument == "--trace" && i + 1 < argc) tracePath = argv[++i];
        else if (argument == "--self-check") return self_check(i + 1 < argc ? std::string { argv[++i] } : std::string {});
    }
    const Script script { load_script(scriptPath) };

    std::vector<std::jthread> workers;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        Json message = Json::parse(line, nullptr, false);
        if (message.is_discarded() || !message.is_object()) continue;
        const bool hasId { message.contains("id") && !message.at("id").is_null() };
        const std::string method { message.value("method", std::string {}) };

        if (!hasId) {
            if (method == "cancel") handle_cancel(message.value("params", Json::object()));
            else if (method == "exit") std::_Exit(0);   // PROTOCOL.md: gone as soon as this is read, nothing awaited
            continue;
        }
        const Json id = message["id"];

        if (method == "initialize") {
            trace(Json { { "method", "initialize" } });
            write_line(Json { { "id", id }, { "result", initialize_result(script) } });
        } else if (method == "shutdown") {
            write_line(Json { { "id", id }, { "result", nullptr } });
        } else if (method == "complete") {
            const Json params = message.value("params", Json::object());
            workers.emplace_back([id, params, &script] { handle_complete(id, params, script); });
        } else {
            write_line(Json { { "id", id }, { "error", Json { { "code", -32601 }, { "message", "unknown method" } } } });
        }
    }
    // End of input (PROTOCOL.md): a clean shutdown waits for what was already read, unlike `exit`.
    return 0;
}
