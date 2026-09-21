module mcppls.ai.model.gateway;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.log;
import mcppls.platform.process;
import mcppls.platform.task;
import mcppls.lsp.connection;
import mcppls.ai.model.source;

namespace mcppls::ai::model {

namespace {

Json build_request(int id, std::string_view method, Json params) {
    Json message = Json::object();
    message["id"] = id;
    message["method"] = std::string { method };
    message["params"] = std::move(params);
    return message;
}

} // namespace

struct GatewayClient::State {
    GatewayOptions options;
    std::unique_ptr<lsp::Connection> connection;
    std::atomic<int> nextId { 1 };
    std::atomic<bool> closed { true };

    std::mutex pendingMutex;
    std::map<int, std::shared_ptr<platform::Channel<Json>>> pending;

    mutable std::mutex usageMutex;
    Usage usage;

    std::shared_ptr<platform::Channel<Json>> register_pending(int id) {
        auto channel = std::make_shared<platform::Channel<Json>>();
        std::lock_guard<std::mutex> lock { pendingMutex };
        pending[id] = channel;
        return channel;
    }

    void forget(int id) {
        std::lock_guard<std::mutex> lock { pendingMutex };
        pending.erase(id);
    }

    // Runs on the connection's reader thread: hand the response to whichever call is waiting for
    // this id, or drop it silently (an answer that arrived after that call already gave up).
    void deliver(Json message) {
        if (!message.is_object()) return;
        const auto idField = message.find("id");
        if (idField == message.end() || !idField->is_number_integer()) return;
        std::shared_ptr<platform::Channel<Json>> channel;
        {
            std::lock_guard<std::mutex> lock { pendingMutex };
            const auto found = pending.find(idField->get<int>());
            if (found == pending.end()) return;
            channel = found->second;
        }
        channel->push(std::move(message));
    }

    void handle_closed() {
        closed.store(true);
        std::lock_guard<std::mutex> lock { pendingMutex };
        for (auto& [id, channel] : pending) channel->close();
    }

    void add_usage(const Usage& delta) {
        std::lock_guard<std::mutex> lock { usageMutex };
        usage.inputTokens += delta.inputTokens;
        usage.outputTokens += delta.outputTokens;
    }
};

GatewayClient::GatewayClient(GatewayOptions options) : state_ { std::make_unique<State>() } { state_->options = std::move(options); }

GatewayClient::~GatewayClient() { stop(); }

base::Result<void> GatewayClient::start() {
    if (state_->connection && !state_->closed.load()) return {};

    platform::SpawnOptions spawn;
    spawn.program = state_->options.executable;
    spawn.arguments = state_->options.arguments;
    spawn.workDirectory = state_->options.workDirectory;
    State* state { state_.get() };
    auto connection = lsp::Connection::start(
        std::move(spawn),
        [state](Json message) { state->deliver(std::move(message)); },
        [state] { state->handle_closed(); },
        {},
        lsp::Framing::lines);
    if (!connection) return std::unexpected { connection.error() };
    state_->connection = std::move(*connection);
    state_->closed.store(false);

    const int id { state_->nextId.fetch_add(1) };
    auto channel = state_->register_pending(id);
    auto sent = state_->connection->send(build_request(id, "initialize", Json::object()));
    if (!sent) {
        state_->forget(id);
        return std::unexpected { sent.error() };
    }
    auto response = channel->pop_until(std::chrono::steady_clock::now() + state_->options.requestTimeout);
    state_->forget(id);
    if (!response) {
        const bool exited { state_->closed.load() };
        base::log::warning("mcppls-model: {}", exited ? "the child exited before answering initialize" : "initialize timed out");
        return base::fail(exited ? "model-gateway-exited" : "model-gateway-timeout",
                          exited ? "the model gateway process exited before answering initialize"
                                 : "the model gateway did not answer initialize within the request timeout");
    }
    if (const auto error = response->find("error"); error != response->end()) {
        return base::fail("model-gateway-error", error->value("message", std::string { "initialize failed" }));
    }
    return {};
}

void GatewayClient::stop() {
    if (!state_->connection) return;
    state_->connection->stop(state_->options.stopGrace);
    state_->connection.reset();
    state_->closed.store(true);
}

bool GatewayClient::running() const { return state_->connection && !state_->closed.load(); }

base::Result<CompletionResult> GatewayClient::complete(const CompletionRequest& request) {
    if (!running()) {
        auto started = start();
        if (!started) return std::unexpected { started.error() };
    }

    Json params = Json::object();
    params["model"] = request.model;
    Json messages = Json::array();
    for (const auto& message : request.messages) messages.push_back(Json { { "role", message.role }, { "content", message.content } });
    params["messages"] = std::move(messages);
    if (!request.schema.is_null()) params["schema"] = request.schema;
    if (request.maxTokens > 0) params["maxTokens"] = request.maxTokens;
    params["temperature"] = 0;

    const int id { state_->nextId.fetch_add(1) };
    auto channel = state_->register_pending(id);
    auto sent = state_->connection->send(build_request(id, "complete", std::move(params)));
    if (!sent) {
        state_->forget(id);
        return std::unexpected { sent.error() };
    }

    auto response = channel->pop_until(std::chrono::steady_clock::now() + state_->options.requestTimeout);
    if (!response) {
        const bool exited { state_->closed.load() };
        if (!exited && state_->connection) {
            // Best-effort per PROTOCOL.md: a notification, never answered, so its own send failure
            // is not reported as this call's error, the timeout already is.
            (void)state_->connection->send(Json { { "method", "cancel" }, { "params", Json { { "id", id } } } });
        }
        state_->forget(id);
        base::log::warning("mcppls-model: {}", exited ? "the child exited before answering complete" : "complete timed out; cancel sent");
        return base::fail(exited ? "model-gateway-exited" : "model-gateway-timeout",
                          exited ? "the model gateway process exited before answering"
                                 : "the model gateway did not answer within the request timeout");
    }
    state_->forget(id);

    if (const auto error = response->find("error"); error != response->end()) {
        const int code { error->value("code", 0) };
        const std::string suffix { code != 0 ? std::format(" ({})", code) : std::string {} };
        return base::fail("model-gateway-error", error->value("message", std::string { "the model gateway reported an error" }) + suffix);
    }

    const Json result = response->value("result", Json::object());
    CompletionResult completion;
    completion.modelName = result.value("model", request.model);
    completion.output = result.value("json", Json {});
    if (const auto usage = result.find("usage"); usage != result.end() && usage->is_object()) {
        completion.usage.inputTokens = usage->value("inputTokens", 0);
        completion.usage.outputTokens = usage->value("outputTokens", 0);
    }
    state_->add_usage(completion.usage);
    return completion;
}

Usage GatewayClient::usage() const {
    std::lock_guard<std::mutex> lock { state_->usageMutex };
    return state_->usage;
}

} // namespace mcppls::ai::model
