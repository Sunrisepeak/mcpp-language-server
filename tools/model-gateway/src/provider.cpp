module mcppls.model.provider;

import std;
import nlohmann.json;
import mcppls.model.config;
import mcppls.model.protocol;
import mcppls.model.schema;
import mcppls.model.transport;

namespace mcppls::model {

namespace {

using Json = nlohmann::json;

std::string trimmed(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// A model asked for raw JSON sometimes still wraps it in a fenced code block
// ("```json\n...\n```" or "```\n...\n```"); unwrap the first one found so the
// text still parses. Anything without a fence is returned unchanged.
std::string unwrap_code_fence(const std::string& text) {
    const auto fenceStart = text.find("```");
    if (fenceStart == std::string::npos) return text;
    auto contentStart = fenceStart + 3;
    if (const auto languageEnd = text.find('\n', contentStart);
        languageEnd != std::string::npos && languageEnd - contentStart <= 16) {
        contentStart = languageEnd + 1; // skip an opening-line language tag, e.g. ```json
    }
    const auto fenceEnd = text.find("```", contentStart);
    if (fenceEnd == std::string::npos) return text;
    return text.substr(contentStart, fenceEnd - contentStart);
}

std::string schema_instruction(const Json& schema) {
    return "Respond with only a single JSON object matching this JSON Schema. "
           "No prose, no markdown code fence, no explanation — the entire reply must be the JSON object itself.\n"
           "Schema: " + schema.dump();
}

std::string finish_reason_openai(const std::string& raw) {
    return raw == "length" ? "length" : "stop";
}

std::string finish_reason_anthropic(const std::string& raw) {
    return raw == "max_tokens" ? "length" : "stop";
}

// OpenAI-shaped: {"error": {"message": "..."}}. Anthropic-shaped: {"error":
// {"type": "...", "message": "..."}}. Both land here.
std::string extract_provider_message(const Json& body) {
    if (!body.is_object()) return {};
    const auto errorField = body.find("error");
    if (errorField == body.end()) return {};
    if (errorField->is_object()) {
        if (const auto message = errorField->find("message"); message != errorField->end() && message->is_string()) {
            return message->get<std::string>();
        }
    } else if (errorField->is_string()) {
        return errorField->get<std::string>();
    }
    return {};
}

std::map<std::string, std::string> auth_headers(const std::string& provider, const std::string& apiKey) {
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    if (apiKey.empty()) return headers; // absent is fine for local endpoints (PROTOCOL.md)
    if (provider == "openai") {
        headers["Authorization"] = "Bearer " + apiKey;
    } else if (provider == "anthropic") {
        headers["x-api-key"] = apiKey;
        headers["anthropic-version"] = "2023-06-01";
    }
    return headers;
}

Json build_openai_body(const CompleteRequest& request) {
    Json body;
    body["model"] = request.model;
    Json messages = Json::array();
    for (const auto& message : request.messages) {
        Json entry;
        entry["role"] = message.role;
        entry["content"] = message.content;
        messages.push_back(std::move(entry));
    }
    body["messages"] = std::move(messages);
    body["temperature"] = request.temperature;
    if (request.maxTokens) body["max_tokens"] = *request.maxTokens;
    if (request.schema) {
        Json jsonSchema;
        jsonSchema["name"] = "response";
        jsonSchema["schema"] = *request.schema;
        Json responseFormat;
        responseFormat["type"] = "json_schema";
        responseFormat["json_schema"] = std::move(jsonSchema);
        body["response_format"] = std::move(responseFormat);
    }
    return body;
}

Json build_anthropic_body(const CompleteRequest& request) {
    Json body;
    body["model"] = request.model;

    std::string systemText;
    Json messages = Json::array();
    for (const auto& message : request.messages) {
        if (message.role == "system") {
            if (!systemText.empty()) systemText += "\n";
            systemText += message.content;
            continue;
        }
        Json entry;
        entry["role"] = message.role == "assistant" ? "assistant" : "user";
        entry["content"] = message.content;
        messages.push_back(std::move(entry));
    }
    if (request.schema) {
        // Anthropic has no response_format-style structured output here: add an instruction (PROTOCOL.md).
        if (!systemText.empty()) systemText += "\n";
        systemText += schema_instruction(*request.schema);
    }
    if (!systemText.empty()) body["system"] = systemText;
    body["messages"] = std::move(messages);
    body["max_tokens"] = request.maxTokens.value_or(4096); // Anthropic requires max_tokens
    body["temperature"] = request.temperature;
    return body;
}

CompleteResponse parse_openai_response(const Json& body, const CompleteRequest& request) {
    CompleteResponse response;
    response.model = body.value("model", request.model);

    std::string rawFinish;
    if (const auto choices = body.find("choices"); choices != body.end() && choices->is_array() && !choices->empty()) {
        const Json& choice = (*choices)[0];
        if (const auto message = choice.find("message"); message != choice.end() && message->is_object()) {
            response.content = message->value("content", std::string {});
        }
        rawFinish = choice.value("finish_reason", std::string {});
    }
    response.finishReason = finish_reason_openai(rawFinish);

    if (const auto usage = body.find("usage"); usage != body.end() && usage->is_object()) {
        response.usage.inputTokens = usage->value("prompt_tokens", 0);
        response.usage.outputTokens = usage->value("completion_tokens", 0);
    }
    return response;
}

CompleteResponse parse_anthropic_response(const Json& body, const CompleteRequest& request) {
    CompleteResponse response;
    response.model = body.value("model", request.model);

    std::string text;
    if (const auto content = body.find("content"); content != body.end() && content->is_array()) {
        for (const auto& block : *content) {
            if (block.is_object() && block.value("type", std::string {}) == "text") {
                text += block.value("text", std::string {});
            }
        }
    }
    response.content = text;
    response.finishReason = finish_reason_anthropic(body.value("stop_reason", std::string {}));

    if (const auto usage = body.find("usage"); usage != body.end() && usage->is_object()) {
        response.usage.inputTokens = usage->value("input_tokens", 0);
        response.usage.outputTokens = usage->value("output_tokens", 0);
    }
    return response;
}

} // namespace

bool provider_supports_structured_output(const std::string& provider) {
    return provider == "openai";
}

std::expected<CompleteResponse, RpcError> complete(const Config& config, const CompleteRequest& request,
                                                    const std::atomic<bool>* cancelled) {
    if (config.provider != "openai" && config.provider != "anthropic") {
        return std::unexpected(RpcError { NOT_CONFIGURED, "no provider configured (--provider, or MCPPLS_MODEL_PROVIDER)" });
    }
    if (request.model.empty()) {
        return std::unexpected(RpcError { NOT_CONFIGURED, "no model configured (--model / MCPPLS_MODEL_NAME, or params.model)" });
    }

    const bool isOpenAi { config.provider == "openai" };
    const Json requestBody = isOpenAi ? build_openai_body(request) : build_anthropic_body(request);
    const std::string url { config.endpoint + (isOpenAi ? "/chat/completions" : "/messages") };
    const auto headers = auth_headers(config.provider, config.apiKey);

    const HttpResult httpResult { post_json(url, headers, requestBody.dump(), config.timeoutSeconds, cancelled) };
    if (httpResult.cancelled) return std::unexpected(RpcError { CANCELLED, "cancelled" });
    if (!httpResult.ok) {
        return std::unexpected(RpcError { PROVIDER_ERROR, "transport error: " + httpResult.transportError });
    }
    if (httpResult.status < 200 || httpResult.status >= 300) {
        std::string providerMessage;
        try {
            providerMessage = extract_provider_message(Json::parse(httpResult.body));
        } catch (...) {
            // fall through with an empty providerMessage; the raw body excerpt below covers it
        }
        if (providerMessage.empty()) providerMessage = httpResult.body.substr(0, 200);
        return std::unexpected(RpcError { PROVIDER_ERROR,
            std::format("HTTP {} from {}: {}", httpResult.status, config.provider, providerMessage) });
    }

    Json responseBody;
    try {
        responseBody = Json::parse(httpResult.body);
    } catch (const std::exception& error) {
        return std::unexpected(RpcError { PROVIDER_ERROR, std::string { "malformed provider response: " } + error.what() });
    }

    CompleteResponse response { isOpenAi ? parse_openai_response(responseBody, request) : parse_anthropic_response(responseBody, request) };

    if (request.schema) {
        const std::string candidate { trimmed(unwrap_code_fence(response.content)) };
        Json parsedJson;
        bool isJson { true };
        try {
            parsedJson = Json::parse(candidate);
        } catch (...) {
            isJson = false;
        }
        if (!isJson || !matches_schema(*request.schema, parsedJson)) {
            return std::unexpected(RpcError { SCHEMA_MISMATCH, "the model's reply is not JSON matching the requested schema" });
        }
        response.json = std::move(parsedJson);
    }

    return response;
}

} // namespace mcppls::model
