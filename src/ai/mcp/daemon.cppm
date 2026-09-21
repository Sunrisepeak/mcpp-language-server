// The workspace daemon (overall design 6.3 step two, work item A3): one warm headless session per
// workspace that any number of MCP connections share, so every agent, and every `mcppls mcp` an agent
// starts, reaches engines that have already loaded the project and built its modules.
//
// It listens on 127.0.0.1 at a port the system picks, and says where in <cache>/workspaces/<key>/daemon.json
// with a token a connection's first line must carry: {"token": ..., "session": "mcp" | "control"}. After
// that, an MCP connection carries MCP messages one per line, exactly as standard streams do; a control
// connection asks {"method": "status" | "stop"}. The daemon exits when told to, or when no connection
// has been open for its idle time. LSP sessions stay in their own process in this version: a workspace
// shared between an editor's unsaved buffers and an agent's reads needs a workspace with several clients.
export module mcppls.ai.mcp.daemon;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.ai.mcp.server;

export namespace mcppls::ai::mcp {

struct DaemonOptions {
    ServerOptions server;
    std::chrono::minutes idle { 30 };
};

struct DaemonInfo {
    int port { 0 };
    std::string token;
    std::string version;
    std::string root;
};

// Where the daemon of a workspace root says where it is.
std::string discovery_path(std::string_view root);
// The daemon of a root, when its discovery file is there and it is this version's.
std::optional<DaemonInfo> find_daemon(std::string_view root);

// Serves in the foreground until stopped or idle; returns the exit status.
int run_daemon(const DaemonOptions& options);

// Starts `executable daemon run --root <root> <arguments>` detached, and waits for it to say where it is.
base::Result<DaemonInfo> start_daemon(std::string_view executable, std::string_view root, std::span<const std::string> arguments,
                                      std::chrono::milliseconds timeout);

// A control request, answered with one JSON line.
base::Result<nlohmann::json> control(const DaemonInfo& daemon, std::string_view method);

// `mcppls mcp --daemon`: standard input and output relayed to an MCP connection of the daemon.
int relay_mcp(const DaemonInfo& daemon);

} // namespace mcppls::ai::mcp
