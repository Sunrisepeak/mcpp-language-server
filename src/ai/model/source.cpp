module mcppls.ai.model.source;

import std;

namespace mcppls::ai::model {

std::string_view to_string(SourceKind kind) {
    switch (kind) {
    case SourceKind::none: return "none";
    case SourceKind::agent: return "agent";
    case SourceKind::mcp_sampling: return "mcp-sampling";
    case SourceKind::client: return "client";
    case SourceKind::gateway: return "gateway";
    }
    return "none";
}

std::optional<SourceKind> parse_source(std::string_view text) {
    if (text == "none") return SourceKind::none;
    if (text == "agent") return SourceKind::agent;
    if (text == "mcp-sampling" || text == "mcp_sampling") return SourceKind::mcp_sampling;
    if (text == "client") return SourceKind::client;
    if (text == "gateway") return SourceKind::gateway;
    return std::nullopt;
}

TransportClient::TransportClient(SourceKind source, Transport transport) : source_ { source }, transport_ { std::move(transport) } { }

base::Result<CompletionResult> TransportClient::complete(const CompletionRequest& request) {
    if (!transport_) return base::fail("model-not-configured", "no transport function was supplied for this model source");
    auto result = transport_(request);
    if (result) {
        std::lock_guard<std::mutex> lock { mutex_ };
        usage_.inputTokens += result->usage.inputTokens;
        usage_.outputTokens += result->usage.outputTokens;
    }
    return result;
}

Usage TransportClient::usage() const {
    std::lock_guard<std::mutex> lock { mutex_ };
    return usage_;
}

} // namespace mcppls::ai::model
