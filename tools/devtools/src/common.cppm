// What every mcppls-devtools command shares: where the repository is, how an external step runs,
// how a missing tool is reported, and how the server that was just built is found.
export module mcppls.devtools.common;

import std;
import nlohmann.json;
import mcppls.base.error;

export namespace mcppls::devtools {

// The repository this is working on: the nearest directory, upward from the current one and
// then from this program's own location, whose mcpp.toml declares the workspace and which has
// `packaging/`. Commands run from anywhere inside a checkout, and from a copied binary (CI's
// stage/) run in one.
std::string repository_root();

// One external step. Prints what it runs and how long it took, so a failure is reproducible by
// hand; on failure prints the tail of what the program said. A `.cmd`/`.bat` on Windows is run
// through the command interpreter, which is the only thing that can start one.
bool step(std::string_view what, const std::string& program, std::vector<std::string> arguments,
          const std::string& directory);

// A program run for its answer rather than its effect: standard output on success, an error
// carrying the tail of standard error otherwise.
base::Result<std::string> capture(const std::string& program, std::vector<std::string> arguments,
                                  const std::string& directory,
                                  std::chrono::milliseconds bound = std::chrono::minutes { 30 });

// A program packaging runs and does not ship, found on PATH, or a sentence naming it and the
// declaration that provides it (mcpp.toml's [xlings.workspace]; `mcpp run -p devtools` provisions
// it before this process starts). Nothing is installed from here.
std::optional<std::string> tool(std::string_view name, std::string_view whatItIsFor);

// The server as the payload carries it (ecosystem design 2026-10-29 §9): one okb holding the image of
// every architecture, and the tree of the `openkal` set packed self-contained for every system the
// platform's runner reaches -- its `libexec/okrun/<system>/` holds the runner of each system, a
// program of that system that needs nothing installed beside it (mcpp SPEC-014 §4.2, §4.4).
struct ServerParts {
    std::string image;     // the okb
    std::string archive;   // the tree's archive (`mcppls-<version>-openkal.zip`)
    std::string tree;      // the tree, unpacked
};

// `mcpp pack --profile <profile> --target openkal --mode self-contained --systems all --pkg-format
// zip,okb`, once: one build, both containers. Found through the pack's own report
// (`--message-format json`), not by guessing under target/.
base::Result<ServerParts> pack_server(const std::string& root, std::string_view profile);

// The server tree `given` names: a directory as it is, or an archive (`.zip`, `.tar.gz`) unpacked
// into `scratch`.
base::Result<std::string> server_tree(std::string_view given, const std::string& scratch);

// Writes `value` as the command's machine-readable answer.
void print_json(const nlohmann::json& value);

} // namespace mcppls::devtools
