// Model sources (overall design 7.5): where a review's model judgment, if any, comes from, the
// settings that pick one, and the request/response shapes every source answers the same way.
// `none` and `agent` never produce a client (an agent calling mcppls directly, or over MCP without
// sampling, judges the evidence itself); `mcp-sampling` and `client` are backed by a transport function
// the entry (MCP server or LSP session) supplies once it exists, so they are testable with a fake one;
// `gateway` is mcppls.ai.model.gateway's child-process client.
export module mcppls.ai.model.source;

import std;
import nlohmann.json;
import mcppls.base.error;

export namespace mcppls::ai::model {

using Json = nlohmann::json;

enum class SourceKind { none, agent, mcp_sampling, client, gateway };

std::string_view to_string(SourceKind kind);
std::optional<SourceKind> parse_source(std::string_view text);

struct ModelSettings {
    SourceKind source { SourceKind::none };
    std::string gatewayExecutable;
    std::vector<std::string> gatewayArguments;
    std::string model;
    std::size_t tokenBudget { 8000 };
    std::vector<std::string> excludedPaths;   // glob patterns (mcppls.base.glob)
    bool explicitlyEnabled { false };         // design 11: model use is opt-in
};

struct Message {
    std::string role;      // system | user | assistant
    std::string content;
};

struct Usage {
    int inputTokens { 0 };
    int outputTokens { 0 };
};

struct CompletionRequest {
    std::string model;
    std::vector<Message> messages;
    Json schema;            // JSON Schema requesting structured output; null for none
    int maxTokens { 0 };    // 0: let the source pick its own default
};

struct CompletionResult {
    Json output;            // the schema-shaped reply; null when no schema was requested
    std::string modelName;
    Usage usage;
};

class ModelClient {
public:
    virtual ~ModelClient() = default;
    virtual base::Result<CompletionResult> complete(const CompletionRequest& request) = 0;
    // Tokens spent across every call made through this client so far.
    virtual Usage usage() const = 0;
};

// mcp-sampling and client: the transport (an MCP `sampling/createMessage` call, an LSP custom
// request to the editor's model API, ...) is supplied by whichever entry point wires the source up;
// this class only does the bookkeeping the two sources share, so tests can hand it a fake function.
class TransportClient : public ModelClient {
public:
    using Transport = std::function<base::Result<CompletionResult>(const CompletionRequest&)>;

    TransportClient(SourceKind source, Transport transport);
    TransportClient(const TransportClient&) = delete;
    TransportClient& operator=(const TransportClient&) = delete;

    base::Result<CompletionResult> complete(const CompletionRequest& request) override;
    Usage usage() const override;
    SourceKind source() const { return source_; }

private:
    SourceKind source_;
    Transport transport_;
    mutable std::mutex mutex_;
    Usage usage_;
};

} // namespace mcppls::ai::model
