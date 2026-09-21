module mcppls.ai.mcp.server;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.base.version;
import mcppls.platform.env;
import mcppls.platform.stdio;
import mcppls.orchestrator.workspace;
import mcppls.ai.model.source;
import mcppls.ai.model.gateway;
import mcppls.lsp.jsonrpc;
import mcppls.orchestrator.kernel;
import mcppls.ai.query.view;
import mcppls.ai.mcp.tools;

namespace mcppls::ai::mcp {

namespace {

using Json = nlohmann::json;
namespace log = base::log;

constexpr int PARSE_ERROR { -32700 };
constexpr int INVALID_REQUEST { -32600 };
constexpr int METHOD_NOT_FOUND { -32601 };
constexpr int INVALID_PARAMS { -32602 };

constexpr std::string_view INSTRUCTIONS {
    "C++ code intelligence for this workspace, from the compiler's view of it (C++20 modules included). "
    "Prefer these tools to text search for C++ names: cxx_symbol finds declarations and definitions, cxx_references finds uses, "
    "callers and callees across modules, cxx_verify checks an edit (changed files and what imports them, or a snippet before it is written). "
    "Lines and columns start at 1. Every tool is read-only."
};

Json response(const Json& id, Json result) { return Json { { "jsonrpc", "2.0" }, { "id", id }, { "result", std::move(result) } }; }

Json error_response(const Json& id, int code, std::string_view message) {
    return Json { { "jsonrpc", "2.0" }, { "id", id }, { "error", Json { { "code", code }, { "message", std::string { message } } } } };
}

} // namespace

std::vector<std::string> take_lines(std::string& buffer) {
    std::vector<std::string> lines;
    std::size_t start { 0 };
    for (std::size_t at { buffer.find('\n') }; at != std::string::npos; at = buffer.find('\n', start)) {
        std::string line { buffer.substr(start, at - start) };
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!base::trim(line).empty()) lines.push_back(std::move(line));
        start = at + 1;
    }
    buffer.erase(0, start);
    return lines;
}

Session::Session(orchestrator::Kernel& kernel, std::chrono::seconds toolTimeout, std::function<void(const Json&)> send, model::ModelSettings modelSettings)
    : kernel_ { kernel }, toolTimeout_ { toolTimeout }, send_ { std::move(send) }, model_ { std::move(modelSettings) } {}

std::unique_ptr<model::ModelClient> Session::make_client(model::SourceKind source) {
    if (source == model::SourceKind::gateway) {
        model::GatewayOptions options;
        options.executable = model_.gatewayExecutable.empty() ? platform::env::find_executable("mcppls-model").value_or("") : model_.gatewayExecutable;
        options.arguments = model_.gatewayArguments;
        if (!model_.model.empty()) options.arguments.insert(options.arguments.end(), { "--model", model_.model });
        options.workDirectory = kernel_.root();
        options.requestTimeout = toolTimeout_;
        if (options.executable.empty()) return nullptr;
        return std::make_unique<model::GatewayClient>(std::move(options));
    }
    if (source != model::SourceKind::mcp_sampling || !clientCapabilities_.contains("sampling")) return nullptr;
    // MCP sampling: the agent's own client asks its model, with its user's consent, on this server's behalf.
    return std::make_unique<model::TransportClient>(model::SourceKind::mcp_sampling, [this](const model::CompletionRequest& request) -> base::Result<model::CompletionResult> {
        Json messages = Json::array();
        std::string system;
        for (const auto& message : request.messages) {
            if (message.role == "system") {
                system += message.content + "\n";
                continue;
            }
            messages.push_back(Json { { "role", message.role == "assistant" ? "assistant" : "user" }, { "content", Json { { "type", "text" }, { "text", message.content } } } });
        }
        // Unique across the sessions a daemon serves at once, whose responses share one queue.
        static std::atomic<std::uint64_t> serial { 0 };
        const std::string id { std::format("mcppls-sampling-{}-{}", ++serial, nextRequest_++) };
        send_(Json { { "jsonrpc", "2.0" }, { "id", id }, { "method", "sampling/createMessage" },
                     { "params", Json { { "messages", std::move(messages) }, { "systemPrompt", system }, { "includeContext", "none" }, { "temperature", 0 },
                                        { "maxTokens", request.maxTokens > 0 ? request.maxTokens : 4096 } } } });
        // A daemon wraps each connection's messages with the connection they came from.
        const auto unwrap = [](const Json& message) -> const Json& { return message.is_object() && message.contains("message") && message.contains("mcppls-daemon") ? message["message"] : message; };
        auto response = kernel_.take_external([&](const Json& message) {
            const Json& inner = unwrap(message);
            return inner.is_object() && !inner.contains("method") && inner.value("id", Json {}) == Json(id);
        }, toolTimeout_);
        if (!response) return base::fail("sampling-timeout", "the client did not answer sampling/createMessage in time");
        if (response->contains("mcppls-daemon")) {
            Json inner = (*response)["message"];
            response = std::move(inner);
        }
        if (response->contains("error")) return base::fail("sampling-refused", response->value("error", Json::object()).value("message", std::string { "refused" }));
        const Json result = response->value("result", Json::object());
        std::string text { result.value("content", Json::object()).value("text", std::string {}) };
        // A model can wrap its JSON in prose or a code fence; the object is what counts.
        const std::size_t open { text.find('{') };
        const std::size_t close { text.rfind('}') };
        Json output = open != std::string::npos && close != std::string::npos && close > open ? Json::parse(text.substr(open, close - open + 1), nullptr, false) : Json {};
        if (output.is_discarded()) output = nullptr;
        return model::CompletionResult { std::move(output), result.value("model", std::string {}), {} };
    });
}

void Session::handle(const Json& message) {
    if (!message.is_object() || message.value("jsonrpc", std::string {}) != "2.0") {
        send_(error_response(message.is_object() ? message.value("id", Json {}) : Json {}, INVALID_REQUEST, "not a JSON-RPC 2.0 message"));
        return;
    }
    const bool isRequest { message.contains("id") && message.contains("method") };
    const std::string method { message.value("method", std::string {}) };
    const Json params = message.contains("params") && message["params"].is_object() ? message["params"] : Json::object();
    // notifications/initialized needs nothing; requests run to completion one at a time, so a
    // notifications/cancelled arrives after the request it names was answered.
    if (!isRequest) return;
    const Json& id { message["id"] };
    if (method == "initialize") {
        const std::string requested { params.value("protocolVersion", std::string {}) };
        const bool known { std::ranges::find(PROTOCOL_VERSIONS, requested) != PROTOCOL_VERSIONS.end() };
        protocolVersion_ = known ? requested : std::string { PROTOCOL_VERSIONS.front() };
        if (const auto capabilities = params.find("capabilities"); capabilities != params.end() && capabilities->is_object()) clientCapabilities_ = *capabilities;
        send_(response(id, Json { { "protocolVersion", protocolVersion_ },
                                  { "capabilities", Json { { "tools", Json { { "listChanged", false } } } } },
                                  { "serverInfo", Json { { "name", "mcppls" }, { "title", "mcpp-language-server" }, { "version", std::string { base::VERSION } } } },
                                  { "instructions", std::string { INSTRUCTIONS } } }));
        return;
    }
    if (method == "ping") {
        send_(response(id, Json::object()));
        return;
    }
    if (method == "tools/list") {
        send_(response(id, Json { { "tools", tool_list() } }));
        return;
    }
    if (method == "tools/call") {
        const std::string name { params.value("name", std::string {}) };
        if (!has_tool(name)) {
            send_(error_response(id, INVALID_PARAMS, std::format("unknown tool: {}", name)));
            return;
        }
        const Json arguments = params.contains("arguments") ? params["arguments"] : Json::object();
        query::View view { kernel_ };
        const auto started = query::Clock::now();
        ToolContext context { model_, base::join_path(kernel_.workspace().cache_directory(), "model"),
                              [this](model::SourceKind source) { return make_client(source); } };
        ToolResult result { call_tool(view, name, arguments, started + toolTimeout_, context) };
        log::info("mcp: {} in {} ms{}", name, std::chrono::duration_cast<std::chrono::milliseconds>(query::Clock::now() - started).count(),
                  result.isError ? " (error)" : "");
        Json answer { { "content", Json::array({ Json { { "type", "text" }, { "text", result.value.dump() } } }) }, { "isError", result.isError } };
        // Structured results came with the 2025-06-18 revision; older clients read the text.
        if (!result.isError && protocolVersion_ >= "2025-06-18") answer["structuredContent"] = result.value;
        send_(response(id, std::move(answer)));
        return;
    }
    send_(error_response(id, METHOD_NOT_FOUND, std::format("unknown method: {}", method)));
}

int run_server(const ServerOptions& options) {
    auto kernel = orchestrator::Kernel::start(options.kernel);
    orchestrator::Kernel* handle { kernel.get() };
    std::thread { [handle] {
        std::string buffer;
        while (true) {
            auto chunk = platform::stdio::read_input();
            if (!chunk || chunk->empty()) break;
            buffer += *chunk;
            for (auto& line : take_lines(buffer)) {
                Json message = Json::parse(line, nullptr, false);
                // A line that is not JSON is answered with a parse error on the loop, like any message.
                if (message.is_discarded()) message = Json { { "mcppls-parse-error", line.substr(0, 200) } };
                handle->post_external(std::move(message));
            }
        }
        handle->close_external();
    } }.detach();

    auto send = [](const Json& message) {
        // One message per line: dump() escapes every newline inside strings.
        if (auto written = platform::stdio::write_output(message.dump() + "\n"); !written) log::error("cannot write to the MCP client: {}", written.error().message);
    };
    Session session { *kernel, options.toolTimeout, send, options.model };
    while (true) {
        auto message = kernel->next_external(std::chrono::hours { 1 });
        if (!message) {
            if (kernel->input_closed()) break;
            continue;
        }
        if (message->contains("mcppls-parse-error")) {
            send(error_response(Json {}, PARSE_ERROR, "a line is not JSON"));
            continue;
        }
        session.handle(*message);
    }
    kernel->shut_down();
    return 0;
}

} // namespace mcppls::ai::mcp
