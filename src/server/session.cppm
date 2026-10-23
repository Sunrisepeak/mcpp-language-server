// The LSP entry (overall design 8): one event loop feeding one or more workspace roots (usable plan
// W9.1), from the editor's standard input, each root's engines and background model loads.
// SessionOptions and the Workspace live in mcppls.orchestrator.workspace, re-exported here so a
// caller needs only this one import.
export module mcppls.server.session;

import std;
import nlohmann.json;
export import mcppls.orchestrator.workspace;

export namespace mcppls::server {

using orchestrator::SessionOptions;

struct WorkspaceFolder {
    std::string path;
    std::string uri;
};

// Resolves initialize's workspace folders, retaining the client's URI spelling.
// Existing regular files are not workspace directories; absent directories remain valid.
std::vector<WorkspaceFolder> workspace_folders(const nlohmann::json& params, std::string_view fallbackDirectory);

// Serves the Language Server Protocol on standard input and output until the client exits.
// Returns the process exit code.
int run_session(const SessionOptions& options);

} // namespace mcppls::server
