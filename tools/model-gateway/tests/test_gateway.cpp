// PROTOCOL.md's contract, checked against a real gateway process and a mock provider, with no
// real model, no API key and no Python.
//
// THE GATEWAY UNDER TEST IS THIS PROGRAM. A test is compiled with every module of the package, so
// the gateway's own `run` is already linked in; started with `--as-gateway` this binary is the
// gateway and nothing else. That spares the test from finding the package's bin target under a
// fingerprint directory it cannot name, and it still exercises what matters: a separate process,
// the line protocol over real pipes, and HTTP over a real socket to a provider that misbehaves on
// command -- which is why the provider is a mock at all (no real one fails with a 500 when asked).
//
// The provider listens on 127.0.0.1 at a port the system chooses. The reply is chosen by the last
// user message: plain, json-ok, json-bad (not JSON, for error 1004), http-500 (for 1001), slow
// (answers after SLOW, for the cancel check, 1003).
import std;
import nlohmann.json;
import mcpplibs.tinyhttps;
import mcppls.testing;
import mcppls.model.gateway;
import mcppls.base.path;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.platform.net;
import mcppls.platform.process;

namespace net = mcppls::platform::net;
namespace platform = mcppls::platform;
using Json = nlohmann::json;

namespace {

constexpr auto SLOW { std::chrono::seconds { 4 } };
constexpr auto CANCEL_DELAY { std::chrono::milliseconds { 300 } };

// Built on first use rather than as a namespace-scope constant. On the macOS runner this test
// died with exit 139 before its output reached the log, while the same source passed on Linux and
// under Wine; a JSON value built by a dynamic initializer before main was the one thing here that
// nothing else in the repository does, so it is not done here either.
const Json& test_schema() {
    static const Json schema = Json::parse(R"({
        "type": "object",
        "required": ["answer", "score"],
        "properties": { "answer": { "type": "string" }, "score": { "type": "integer" } }
    })");
    return schema;
}

// ---- the mock provider ----------------------------------------------------------------------

std::string http_response(int status, const Json& body) {
    const std::string text { body.dump() };
    return std::format("HTTP/1.1 {} {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                       status, status == 200 ? "OK" : "Error", text.size(), text);
}

// Reads one request: the head, then as many body bytes as Content-Length says.
std::optional<std::pair<std::string, std::string>> read_request(net::Connection& connection) {
    std::string buffer;
    std::size_t headEnd { std::string::npos };
    while ((headEnd = buffer.find("\r\n\r\n")) == std::string::npos) {
        auto bytes = connection.read();
        if (!bytes || bytes->empty()) return std::nullopt;
        buffer += *bytes;
    }
    const std::string head { buffer.substr(0, headEnd) };
    std::size_t length { 0 };
    std::string lowered { head };
    std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (const auto at = lowered.find("content-length:"); at != std::string::npos) {
        length = static_cast<std::size_t>(std::stoul(head.substr(at + 15)));
    }
    std::string body { buffer.substr(headEnd + 4) };
    while (body.size() < length) {
        auto bytes = connection.read();
        if (!bytes || bytes->empty()) break;
        body += *bytes;
    }
    return std::pair { head, body };
}

void serve(net::Connection connection) {
    auto request = read_request(connection);
    if (!request) return;
    const auto& [head, raw] = *request;
    const std::string path { head.substr(head.find(' ') + 1, head.find(' ', head.find(' ') + 1) - head.find(' ') - 1) };
    if (path != "/v1/chat/completions" && path != "/chat/completions") {
        (void)connection.write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        return;
    }
    Json body = Json::parse(raw, nullptr, false);
    if (body.is_discarded()) body = Json::object();
    std::string behaviour { "plain" };
    if (body.contains("messages") && body["messages"].is_array()) {
        for (auto it = body["messages"].rbegin(); it != body["messages"].rend(); ++it) {
            if ((*it).value("role", "") == "user") {
                behaviour = (*it).value("content", "");
                if (behaviour.empty()) behaviour = "plain";
                break;
            }
        }
    }
    const std::string model { body.value("model", std::string { "test-model" }) };
    if (behaviour == "http-500") {
        (void)connection.write(http_response(500, { { "error", { { "message", "mock provider failure" }, { "type", "server_error" } } } }));
        return;
    }
    if (behaviour == "slow") {
        std::this_thread::sleep_for(SLOW);
        behaviour = "plain";
    }
    std::string content { "hello from the mock" };
    if (behaviour == "json-bad") content = "this is not json, sorry";
    else if (behaviour == "json-ok") content = Json { { "answer", "42" }, { "score", 7 } }.dump();
    // A cancelled slow request's client is gone by now; the write failing is expected.
    (void)connection.write(http_response(200, {
        { "id", "mock-completion-1" },
        { "model", model },
        { "choices", Json::array({ { { "index", 0 },
                                     { "message", { { "role", "assistant" }, { "content", content } } },
                                     { "finish_reason", "stop" } } }) },
        { "usage", { { "prompt_tokens", 11 }, { "completion_tokens", 5 } } },
    }));
    connection.shutdown_write();
}

class MockProvider {
public:
    static std::optional<MockProvider> start() {
        auto listener = net::Listener::listen_local();
        if (!listener) {
            std::println(std::cerr, "mock provider: cannot listen: {}", listener.error().message);
            return std::nullopt;
        }
        return MockProvider { std::move(*listener) };
    }
    MockProvider(MockProvider&&) = default;
    // Closing a listener does not wake a thread blocked in accept() on every platform, so the
    // acceptor is told to stop and then woken with one connection of its own.
    ~MockProvider() {
        if (!listener_) return;
        stopping_->store(true);
        (void)net::Connection::connect_local(port_);
        if (acceptor_.joinable()) acceptor_.join();
        listener_->close();
    }
    int port() const { return port_; }

private:
    explicit MockProvider(net::Listener listener)
        : listener_ { std::make_unique<net::Listener>(std::move(listener)) }, port_ { listener_->port() } {
        acceptor_ = std::thread { [listener = listener_.get(), stopping = stopping_.get()] {
            while (true) {
                auto connection = listener->accept();
                if (!connection || stopping->load()) return;   // the test is over
                // Detached: a slow reply outlives the request the gateway cancelled.
                std::thread { serve, std::move(*connection) }.detach();
            }
        } };
    }
    std::unique_ptr<net::Listener> listener_;
    std::unique_ptr<std::atomic<bool>> stopping_ { std::make_unique<std::atomic<bool>>(false) };
    int port_ { 0 };
    std::thread acceptor_;
};

// ---- the gateway, as a child process speaking one JSON object per line -------------------

class Gateway {
public:
    static std::optional<Gateway> start(const std::vector<std::string>& arguments) {
        const auto self = platform::env::arguments();
        std::vector<std::string> all { "--as-gateway" };
        all.insert(all.end(), arguments.begin(), arguments.end());
        // The gateway's diagnostics go to this test's standard error, where a failure is read.
        // Normalized first: on Windows argv[0] is `C:\...` with backslashes, which the C++ library
        // above musl does not recognize as absolute and would prefix with the current directory.
        std::string program { mcppls::base::normalize_path(self.front()) };
        if (!mcppls::base::is_absolute_path(program)) {
            program = mcppls::base::join_path(mcppls::platform::fs::current_directory(), program);
        }
        auto process = platform::Process::spawn({ .program = program, .arguments = all });
        if (!process) {
            std::println(std::cerr, "gateway: cannot start: {}", process.error().message);
            return std::nullopt;
        }
        return Gateway { std::move(*process) };
    }

    int send_request(const std::string& method, const Json& params = nullptr) {
        const int id { nextId_++ };
        Json payload { { "id", id }, { "method", method } };
        if (!params.is_null()) payload["params"] = params;
        (void)process_.write(payload.dump() + "\n");
        return id;
    }
    void send_notification(const std::string& method, const Json& params) {
        (void)process_.write(Json { { "method", method }, { "params", params } }.dump() + "\n");
    }

    // One JSON line from the gateway's output, or null on timeout or a closed stream.
    Json read_response(std::chrono::milliseconds timeout = std::chrono::seconds { 15 }) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            if (const auto newline = buffer_.find('\n'); newline != std::string::npos) {
                const std::string line { buffer_.substr(0, newline) };
                buffer_.erase(0, newline + 1);
                Json parsed = Json::parse(line, nullptr, false);
                return parsed.is_discarded() ? Json {} : parsed;
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left <= std::chrono::milliseconds::zero()) return {};
            auto bytes = process_.read_output_for(left);
            if (!bytes || !*bytes) continue;          // bound expired; the loop re-checks the deadline
            if ((*bytes)->empty()) return {};         // the stream ended
            buffer_ += **bytes;
        }
    }

    Json request(const std::string& method, const Json& params = nullptr) {
        (void)send_request(method, params);
        return read_response();
    }

    int close() {
        process_.close_input();
        auto code = process_.wait_for(std::chrono::seconds { 5 });
        if (!code || !*code) {
            process_.kill();
            return -1;
        }
        return **code;
    }

private:
    explicit Gateway(platform::Process process) : process_ { std::move(process) } {}
    platform::Process process_;
    std::string buffer_;
    int nextId_ { 1 };
};

Json complete_params(const std::string& userMessage, const Json& schema = nullptr) {
    Json params { { "messages", Json::array({ { { "role", "user" }, { "content", userMessage } } }) } };
    if (!schema.is_null()) params["schema"] = schema;
    return params;
}

int run_tests() {
    using namespace mcppls::testing;

    auto provider = MockProvider::start();
    expect(fatal(provider.has_value())) << "the mock provider listens";
    if (!provider) return report();
    auto started = Gateway::start({ "--provider", "openai", "--endpoint",
                                    std::format("http://127.0.0.1:{}/v1", provider->port()),
                                    "--model", "test-model", "--timeout", "20" });
    expect(fatal(started.has_value())) << "the gateway starts";
    if (!started) return report();
    Gateway& gateway { *started };

    "initialize answers protocol 1 and lists the configured model"_test = [&] {
        const Json response = gateway.request("initialize");
        expect(fatal(response.is_object())) << "initialize responds";
        const Json result = response.value("result", Json::object());
        expect(result.value("protocol", 0) == 1) << result.dump();
        expect(result["gateway"].value("name", "") == "mcppls-model") << result.dump();
        const Json models = result.value("models", Json::array());
        const auto model = std::ranges::find_if(models, [](const Json& m) { return m.value("id", "") == "test-model"; });
        expect(fatal(model != models.end())) << models.dump();
        expect((*model).value("structuredOutput", false) == true) << model->dump();
    };

    "a plain completion carries text, stop and usage"_test = [&] {
        const Json response = gateway.request("complete", complete_params("plain"));
        expect(fatal(response.is_object())) << "plain responds";
        const Json result = response.value("result", Json::object());
        expect(result["content"].is_string() && !result["content"].get<std::string>().empty()) << result.dump();
        expect(result.value("finishReason", "") == "stop") << result.dump();
        const Json usage = result.value("usage", Json::object());
        expect(usage["inputTokens"].is_number_integer() && usage["inputTokens"].get<int>() > 0) << usage.dump();
        expect(usage["outputTokens"].is_number_integer() && usage["outputTokens"].get<int>() > 0) << usage.dump();
    };

    "a schema completion returns the parsed JSON"_test = [&] {
        const Json response = gateway.request("complete", complete_params("json-ok", test_schema()));
        expect(fatal(response.is_object())) << "json-ok responds";
        const Json parsed = response.value("result", Json::object()).value("json", Json {});
        expect(parsed.is_object() && parsed["answer"].is_string() && parsed["score"].is_number_integer()) << response.dump();
    };

    "a reply that is not JSON against a schema is error 1004"_test = [&] {
        const Json response = gateway.request("complete", complete_params("json-bad", test_schema()));
        expect(fatal(response.is_object())) << "json-bad responds";
        expect(response.value("error", Json::object()).value("code", 0) == 1004) << response.dump();
    };

    "a provider failure is error 1001"_test = [&] {
        const Json response = gateway.request("complete", complete_params("http-500"));
        expect(fatal(response.is_object())) << "http-500 responds";
        expect(response.value("error", Json::object()).value("code", 0) == 1001) << response.dump();
    };

    "cancel answers 1003 promptly, well before the provider would have"_test = [&] {
        const int id { gateway.send_request("complete", complete_params("slow")) };
        std::this_thread::sleep_for(CANCEL_DELAY);
        const auto sent = std::chrono::steady_clock::now();
        gateway.send_notification("cancel", { { "id", id } });
        const Json response = gateway.read_response();
        const auto latency = std::chrono::steady_clock::now() - sent;
        expect(fatal(response.is_object())) << "cancel responds";
        expect(response.value("id", 0) == id) << response.dump();
        expect(response.value("error", Json::object()).value("code", 0) == 1003) << response.dump();
        expect(latency < std::chrono::seconds { 3 })
            << std::format("{} ms", std::chrono::duration_cast<std::chrono::milliseconds>(latency).count());
    };

    "shutdown answers null, and the gateway exits 0 at the end of its input"_test = [&] {
        const Json response = gateway.request("shutdown");
        expect(fatal(response.is_object())) << "shutdown responds";
        expect(response.contains("result") && response["result"].is_null()) << response.dump();
        expect(gateway.close() == 0);
    };

    return report();
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc > 1 && std::string_view { argv[1] } == "--as-gateway") {
        // Exactly what src/main.cpp does, with this binary's first argument removed.
        argv[1] = argv[0];
        mcpplibs::tinyhttps::Socket::platform_init();
        const int status { mcppls::model::run(argc - 1, argv + 1) };
        mcpplibs::tinyhttps::Socket::platform_cleanup();
        return status;
    }
    return run_tests();
}
