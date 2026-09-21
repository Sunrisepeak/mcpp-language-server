module mcppls.model.gateway;

import std;
import nlohmann.json;
import mcppls.model.config;
import mcppls.model.protocol;
import mcppls.model.provider;

namespace mcppls::model {

namespace {

using Json = nlohmann::json;

Json gateway_info() {
    Json info;
    info["name"] = "mcppls-model";
    info["version"] = "0.1.0";
    return info;
}

Json initialize_result(const Config& config) {
    Json result;
    result["protocol"] = 1;
    result["gateway"] = gateway_info();
    Json models = Json::array();
    if ((config.provider == "openai" || config.provider == "anthropic") && !config.model.empty()) {
        Json entry;
        entry["id"] = config.model;
        entry["provider"] = config.provider;
        entry["structuredOutput"] = provider_supports_structured_output(config.provider);
        models.push_back(std::move(entry));
    }
    result["models"] = std::move(models);
    return result;
}

// Maps PROTOCOL.md's `complete` params onto a provider-neutral request,
// applying the wire-level checks that answer INVALID_PARAMS (-32602). A field
// explicitly sent as JSON null is treated the same as an absent field.
std::expected<CompleteRequest, RpcError> parse_complete_params(const Json& params, const Config& config) {
    if (!params.is_object()) return std::unexpected(RpcError { INVALID_PARAMS, "params must be an object" });

    const auto messagesField = params.find("messages");
    if (messagesField == params.end() || !messagesField->is_array() || messagesField->empty()) {
        return std::unexpected(RpcError { INVALID_PARAMS, "params.messages must be a non-empty array" });
    }

    CompleteRequest request;
    for (const auto& item : *messagesField) {
        if (!item.is_object()) {
            return std::unexpected(RpcError { INVALID_PARAMS, "each message must be an object" });
        }
        const auto roleField = item.find("role");
        const auto contentField = item.find("content");
        if (roleField == item.end() || !roleField->is_string() || contentField == item.end() || !contentField->is_string()) {
            return std::unexpected(RpcError { INVALID_PARAMS, "each message needs a string role and a string content" });
        }
        std::string role = roleField->get<std::string>();
        if (role != "system" && role != "user" && role != "assistant") {
            return std::unexpected(RpcError { INVALID_PARAMS, "message role must be system, user or assistant" });
        }
        request.messages.push_back(ChatMessage { std::move(role), contentField->get<std::string>() });
    }

    if (const auto modelField = params.find("model"); modelField != params.end() && !modelField->is_null()) {
        if (!modelField->is_string()) return std::unexpected(RpcError { INVALID_PARAMS, "params.model must be a string" });
        request.model = modelField->get<std::string>();
    } else {
        request.model = config.model; // PROTOCOL.md: "optional: first model"
    }

    if (const auto schemaField = params.find("schema"); schemaField != params.end() && !schemaField->is_null()) {
        if (!schemaField->is_object()) return std::unexpected(RpcError { INVALID_PARAMS, "params.schema must be an object" });
        request.schema = *schemaField;
    }

    if (const auto maxTokensField = params.find("maxTokens"); maxTokensField != params.end() && !maxTokensField->is_null()) {
        if (!maxTokensField->is_number_integer() || maxTokensField->get<long long>() <= 0) {
            return std::unexpected(RpcError { INVALID_PARAMS, "params.maxTokens must be a positive integer" });
        }
        request.maxTokens = maxTokensField->get<int>();
    }

    if (const auto temperatureField = params.find("temperature"); temperatureField != params.end() && !temperatureField->is_null()) {
        if (!temperatureField->is_number()) return std::unexpected(RpcError { INVALID_PARAMS, "params.temperature must be a number" });
        request.temperature = temperatureField->get<double>();
    }

    return request;
}

Json complete_result_payload(const CompleteResponse& response) {
    Json payload;
    payload["content"] = response.content;
    if (response.json) payload["json"] = *response.json;
    payload["model"] = response.model;
    payload["finishReason"] = response.finishReason;
    Json usage;
    usage["inputTokens"] = response.usage.inputTokens;
    usage["outputTokens"] = response.usage.outputTokens;
    payload["usage"] = std::move(usage);
    return payload;
}

// One request the worker is about to process, or already is; `cancelFlag` is
// threaded down into provider::complete/transport::post_json so a `cancel`
// notification can be honored while blocked in I/O.
struct Pending {
    std::shared_ptr<std::atomic<bool>> cancelFlag;
};

class Gateway {
public:
    explicit Gateway(Config config) : config_ { std::move(config) } { }

    int run() {
        std::jthread worker { [this] { worker_loop(); } };
        read_loop();
        {
            std::lock_guard<std::mutex> lock { mutex_ };
            stopping_ = true;
        }
        queueCv_.notify_all();
        // `worker` joins in its own destructor (std::jthread) as `run` returns.
        return 0;
    }

private:
    void read_loop() {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.find_first_not_of(" \t\r\n") == std::string::npos) continue;
            Json message;
            try {
                message = Json::parse(line);
            } catch (const std::exception&) {
                write_line(make_error(nullptr, PARSE_ERROR, "parse error"));
                continue;
            }
            handle_incoming(std::move(message));
        }
        // End of input (PROTOCOL.md: "the process exits at end of input").
    }

    void handle_incoming(Json message) {
        if (!message.is_object()) return; // not a request or a notification we recognize; nothing to correlate an error to
        const bool hasId = message.contains("id") && !message.at("id").is_null();
        const std::string method = message.value("method", std::string {});

        if (!hasId) {
            if (method == "cancel") {
                handle_cancel(message.value("params", Json::object()));
            } else if (method == "exit") {
                std::cout.flush();
                std::exit(0); // PROTOCOL.md: "the process exits ... on an exit notification"
            }
            return; // an unrecognized notification is ignored, per usual JSON-RPC practice
        }

        std::lock_guard<std::mutex> lock { mutex_ };
        queue_.push_back(std::move(message));
        queueCv_.notify_all();
    }

    void handle_cancel(const Json& params) {
        const auto idField = params.find("id");
        if (idField == params.end() || !idField->is_number_integer()) return;
        const long long id = idField->get<long long>();

        std::lock_guard<std::mutex> lock { mutex_ };
        if (const auto pending = pending_.find(id); pending != pending_.end()) {
            pending->second.cancelFlag->store(true);
        } else {
            earlyCancels_.insert(id); // the worker has not dequeued this request yet; flag it once it starts
        }
    }

    void worker_loop() {
        while (true) {
            Json message;
            {
                std::unique_lock<std::mutex> lock { mutex_ };
                queueCv_.wait(lock, [this] { return !queue_.empty() || stopping_; });
                if (queue_.empty()) return; // stopping, with nothing left to answer
                message = std::move(queue_.front());
                queue_.pop_front();
            }
            process_request(std::move(message));
        }
    }

    void process_request(Json message) {
        const Json id = message.value("id", Json {});
        const std::string method = message.value("method", std::string {});
        const long long idKey = id.is_number_integer() ? id.get<long long>() : -1;

        auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
        {
            std::lock_guard<std::mutex> lock { mutex_ };
            if (earlyCancels_.erase(idKey) > 0) cancelFlag->store(true);
            pending_[idKey] = Pending { cancelFlag };
        }

        Json response;
        if (cancelFlag->load()) {
            response = make_error(id, CANCELLED, "cancelled");
        } else if (method == "initialize") {
            response = make_result(id, initialize_result(config_));
        } else if (method == "complete") {
            response = handle_complete(id, message.value("params", Json::object()), cancelFlag);
        } else if (method == "shutdown") {
            response = make_result(id, nullptr);
        } else {
            response = make_error(id, METHOD_NOT_FOUND, "unknown method: " + method);
        }

        {
            std::lock_guard<std::mutex> lock { mutex_ };
            pending_.erase(idKey);
        }
        write_line(response);
    }

    Json handle_complete(const Json& id, const Json& params, const std::shared_ptr<std::atomic<bool>>& cancelFlag) {
        auto request = parse_complete_params(params, config_);
        if (!request) return make_error(id, request.error());

        auto result = complete(config_, *request, cancelFlag.get());
        if (!result) return make_error(id, result.error());

        return make_result(id, complete_result_payload(*result));
    }

    void write_line(const Json& message) {
        std::lock_guard<std::mutex> lock { writeMutex_ };
        std::println("{}", message.dump());
        std::cout.flush();
    }

    Config config_;
    std::mutex mutex_;
    std::condition_variable queueCv_;
    std::deque<Json> queue_;
    std::map<long long, Pending> pending_;
    std::set<long long> earlyCancels_;
    bool stopping_ { false };
    std::mutex writeMutex_;
};

} // namespace

int run(int argc, char* argv[]) {
    auto configResult = resolve_config(argc, argv);
    if (!configResult.config) return configResult.exitCode;
    Gateway gateway { std::move(*configResult.config) };
    return gateway.run();
}

} // namespace mcppls::model
