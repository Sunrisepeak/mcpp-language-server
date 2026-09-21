// The LSP entry (overall design 8): one event loop feeding one or more workspace roots (usable plan
// W9.1), from the editor's standard input, each root's engines and background model loads.
// SessionOptions and the Workspace live in mcppls.orchestrator.workspace, re-exported here so a
// caller needs only this one import.
export module mcppls.server.session;

import std;
export import mcppls.orchestrator.workspace;

export namespace mcppls::server {

using orchestrator::SessionOptions;

// Serves the Language Server Protocol on standard input and output until the client exits.
// Returns the process exit code.
int run_session(const SessionOptions& options);

} // namespace mcppls::server
