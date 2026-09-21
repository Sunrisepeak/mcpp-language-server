// The environment the server starts the user's build tools in.
//
// An editor started from a desktop entry, a Dock icon or a command-line wrapper does not carry
// the environment the user's terminal has: VS Code resolves the login shell only when it was not
// started from the command line, and a wrapper that sets VSCODE_CLI skips even that. The server
// therefore resolves it itself, once, in the background, and starts build tools in the result.
//
// What this can and cannot reach: a login shell gives back what the user's shell configuration
// sets --- PATH entries, XLINGS_HOME and the like. It does not give back what was set by hand in
// one terminal window, proxy variables among them; nothing an editor runs can reach those. So the
// network behaviour of a build tool is settled by running it offline (design 4.4), not by hoping
// the environment matches the terminal's.
export module mcppls.platform.toolenv;

import std;

export namespace mcppls::platform::toolenv {

enum class Mode {
    automatic,   // POSIX: the login shell. Windows: this process's environment (the registry already agrees).
    editor,      // always this process's environment, for a shell configuration with side effects
};

std::optional<Mode> parse_mode(std::string_view name);
std::string_view mode_name(Mode mode);

struct Environment {
    std::vector<std::string> variables;      // "NAME=value", ready for SpawnOptions::environment
    std::string source { "editor" };         // "login-shell" | "editor"
    std::string reason;                      // why the login shell was not used, when it was not
    std::vector<std::string> differing;      // names only: a value may be a credential
    std::chrono::milliseconds duration { 0 };
    bool resolved { false };                 // false while the first resolution is still running
};

// `self` is this executable, which the login shell runs to report its environment. `shell` names
// the shell to ask; empty takes $SHELL, then /bin/sh. Tests name one they control.
void configure(Mode mode, std::string self, std::string shell = {});
Mode mode();

// Starts the resolution if it has not started. Returns at once.
void begin();

// The tool environment, waiting at most `wait` for a resolution in flight. Never fails: where the
// login shell cannot be reached, this process's own environment is the answer and `reason` says why.
Environment get(std::chrono::milliseconds wait = std::chrono::milliseconds { 0 });

// Forgets what was resolved so the next begin() resolves again ("restart the language server").
void reset();

// The text `print-environment` writes: the environment as a JSON object between two marker lines.
std::string print_environment_text(std::string_view marker);

// Exposed for tests: the merge rule of design 4.3 step 2.
std::vector<std::string> merge(const std::vector<std::string>& base, const std::vector<std::string>& fromShell,
                               std::vector<std::string>* differing);
// Exposed for tests: the JSON object between two marker lines, or nullopt.
std::optional<std::vector<std::string>> parse_marked(std::string_view text, std::string_view marker);

} // namespace mcppls::platform::toolenv
