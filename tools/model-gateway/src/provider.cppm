// Provider-specific request building and response parsing for `complete`
// (PROTOCOL.md): an OpenAI-compatible chat-completions dialect, and
// Anthropic's messages dialect.
export module mcppls.model.provider;

import std;
import nlohmann.json;
import mcppls.model.config;
import mcppls.model.protocol;

export namespace mcppls::model {

struct ChatMessage {
    std::string role;      // "system" | "user" | "assistant"
    std::string content;
};

struct CompleteRequest {
    std::string model;                       // already resolved: params.model, else config.model
    std::vector<ChatMessage> messages;
    std::optional<nlohmann::json> schema;    // a JSON Schema object, when structured output was requested
    std::optional<int> maxTokens;
    double temperature { 0.0 };
};

struct Usage {
    int inputTokens { 0 };
    int outputTokens { 0 };
};

struct CompleteResponse {
    std::string content;
    std::optional<nlohmann::json> json;      // parsed and schema-checked, only when a schema was given
    std::string model;
    std::string finishReason;                // "stop" | "length"
    Usage usage;
};

// OpenAI: real structured output (`response_format` with `json_schema`).
// Anthropic: no such field here, so `complete` falls back to an instruction.
bool provider_supports_structured_output(const std::string& provider);

// Builds the provider-shaped request, sends it, and parses the reply. When
// `request.schema` is set, the reply's text is parsed as JSON and checked
// against it (mcppls.model.schema); a reply that is not such JSON answers
// SCHEMA_MISMATCH (1004). `cancelled` reaches the transport so a `cancel`
// notification can be honored while blocked in I/O.
std::expected<CompleteResponse, RpcError> complete(const Config& config, const CompleteRequest& request,
                                                    const std::atomic<bool>* cancelled);

} // namespace mcppls::model
