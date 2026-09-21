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

// How a server build is asked for: a profile and, optionally, a target triple.
struct ServerBuild {
    std::string profile { "release" };   // what a payload ships
    std::string target;                  // empty: this host's own openkal target
};

// The server `mcpp build` produced for `build`: built if it was not, and found by the fingerprint
// the build itself prints rather than by guessing among the directories under target/ (tooling
// architecture §5.5). Not `mcpp pack --format dir`, which the design proposed: its default mode
// walks the dependency closure by executing the program under LD_TRACE_LOADED_OBJECTS, and an
// openkal executable is static and simply runs, so the pack fails (measured, mcpp 2026.9.21.3).
base::Result<std::string> locate_server(const std::string& root, const ServerBuild& build);

// This host's openkal triple, which is what `--target` names for the server a payload carries.
std::string_view host_triple();

// Writes `value` as the command's machine-readable answer.
void print_json(const nlohmann::json& value);

} // namespace mcppls::devtools
