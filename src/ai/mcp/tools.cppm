// The MCP tools (overall design 7.6, S5 6): the queries, contexts and verifications an agent calls,
// each read-only, each answering with S5 data. Descriptions stay short: an agent pays for every
// word of them in every conversation.
export module mcppls.ai.mcp.tools;

import std;
import nlohmann.json;
import mcppls.ai.query.view;
import mcppls.ai.model.source;

export namespace mcppls::ai::mcp {

struct ToolResult {
    nlohmann::json value;     // the S5 result, or {"error": {...}}
    bool isError { false };
};

// What an entry gives the tools beyond the workspace: the model source its user enabled when the
// server started — a tool call can ask for that source, or for the agent source, never enable one
// (overall design 11) — and how to reach it.
struct ToolContext {
    model::ModelSettings model;
    std::string modelCache;
    std::function<std::unique_ptr<model::ModelClient>(model::SourceKind)> makeClient;
};

// tools/list's tools.
nlohmann::json tool_list();
bool has_tool(std::string_view name);
// Runs a tool; arguments were not validated against its schema yet.
ToolResult call_tool(query::View& view, std::string_view name, const nlohmann::json& arguments, query::Clock::time_point deadline, const ToolContext& context = {});

} // namespace mcppls::ai::mcp
