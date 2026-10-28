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

// The server as a payload carries it (mcpp design 2026-10-28 part 2 §15): one okb holding the image
// of every architecture (`mcpp pack --pkg-format okb`), and the platform's runner of one system and
// architecture, which starts it -- the runner a self-contained universal package carries
// (`libexec/okrun/<system>-<arch>/`, mcpp SPEC-014 §4.4), a program of that system that needs
// nothing installed beside it.
struct ImageServer {
    std::string image;
    std::string runner;
};

// Packs both, in `profile`, for `system` (linux, macos or windows) and `arch` (x86_64 or aarch64).
// Found through the packs' own report (`--message-format json`), not by guessing under target/.
base::Result<ImageServer> locate_image_server(const std::string& root, std::string_view profile, std::string_view system,
                                              std::string_view arch);

// Writes `value` as the command's machine-readable answer.
void print_json(const nlohmann::json& value);

} // namespace mcppls::devtools
