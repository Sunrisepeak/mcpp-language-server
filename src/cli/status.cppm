// mcppls status: the state of the servers that serve a workspace, and what this mcppls is, without
// starting an engine or waiting for one (S3 section 8).
//   mcppls status [--root <dir>] [--format text|json]
//
// A running server states its last `cxxModules/status` beside its own `instance.json`; this command
// reads those of the instances still alive and adds what is known without a server: the version, the
// platform, the payload, the semantic kit, and the actions an editor offers under its own ids. It is
// the contract an IDE reads (mcpp-ide's `languageService/status`), so its output is versioned.
export module mcppls.cli.status;

import std;
import mcpplibs.cmdline;
import nlohmann.json;

export namespace mcppls::cli {

mcpplibs::cmdline::App status_command(bool& handled, int& status);

// The document the command prints with --format json, for the workspace at `root` (normalized), read
// from `cacheDirectory` (empty: this user's mcppls cache).
nlohmann::json status_document(std::string_view root, std::string_view cacheDirectory = {});

} // namespace mcppls::cli
