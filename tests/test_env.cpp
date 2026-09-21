import std;
import mcppls.testing;
import mcppls.os;
import mcppls.base.path;
import mcppls.platform.env;
import mcppls.platform.dirs;
import mcppls.platform.fs;
import mcppls.platform.toolenv;

namespace env = mcppls::platform::env;
namespace base = mcppls::base;

int main() {
    // Stands in for `mcppls print-environment`: the login shell runs this, and what it prints
    // between the markers is the environment the shell had. A child leaves before the suite runs.
    {
        const auto arguments = env::arguments();
        if (arguments.size() == 3 && arguments[1] == "print-environment") {
            std::print("{}", mcppls::platform::toolenv::print_environment_text(arguments[2]));
            std::_Exit(0);
        }
    }
    using namespace mcppls::testing;

    "the environment is visible"_test = [] {
        expect(!env::variables().empty());
        expect(env::get("PATH").has_value());
        expect(!env::search_path().empty());
        expect(!env::get("MCPPLS_SURELY_UNSET_VARIABLE").has_value());
    };

    "a value is reported as it was set"_test = [] {
        if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) {
            const auto interpreter = env::get("ComSpec");
            expect(fatal(interpreter.has_value()));
            expect(interpreter->contains('\\') && !interpreter->contains('/')) << *interpreter;
        } else {
            expect(env::get("PATH").value_or("").contains('/'));
        }
    };

    "a system executable is found on PATH"_test = [] {
        if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) {
            const auto found = env::find_executable("cmd");
            expect(fatal(found.has_value()));
            expect(base::file_name(*found) == "cmd.exe" || base::file_name(*found) == "CMD.EXE") << *found;
        } else {
            const auto found = env::find_executable("sh");
            expect(fatal(found.has_value()));
            expect(base::is_absolute_path(*found)) << *found;
        }
        expect(!env::find_executable("mcppls-no-such-program").has_value());
    };

    "an executable is found on another environment's PATH"_test = [] {
        const std::string first { base::join_path(mcppls::platform::dirs::temp_directory(),
            std::format("mcppls-test-env-first-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        const std::string second { first + "-second" };
        const std::string name { std::string { "mcppls-fake-tool" } + std::string { mcppls::os::EXECUTABLE_SUFFIX } };
        (void)mcppls::platform::fs::create_directories(first);
        (void)mcppls::platform::fs::create_directories(second);
        (void)mcppls::platform::fs::write_file(base::join_path(second, name), "");
        const std::string pathList { first + std::string { mcppls::os::PATH_LIST_SEPARATOR } + second };
        const auto found = env::find_executable("mcppls-fake-tool", pathList);
        expect(found.has_value() && base::same_path(*found, base::join_path(second, name))) << found.value_or("<none>");
        expect(!env::find_executable("mcppls-fake-tool").has_value()) << "not on this process's PATH";
        expect(!env::find_executable("mcppls-fake-tool", first).has_value());
        mcppls::platform::fs::remove_all(first);
        mcppls::platform::fs::remove_all(second);
    };

    // Windows executability is the extension, and there are several of them. npm, npx, code,
    // gradle and xlings are `.cmd` shims with no `.exe` anywhere — and npm's directory ALSO holds an
    // extension-less `npm`, the POSIX shell script, which Windows cannot run at all. Both halves
    // cost a CI cycle: appending blindly asked for `npm.cmd.exe`, and then accepting the bare name
    // picked the shell script and failed with an I/O error at spawn.
    "a Windows executable is found by extension, and only by extension"_test = [] {
        const std::string root { base::join_path(mcppls::platform::dirs::temp_directory(),
            std::format("mcppls-test-env-shim-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        (void)mcppls::platform::fs::create_directories(root);

        if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) {
            // npm's layout: the shim, and the shell script beside it. The shim is the answer.
            (void)mcppls::platform::fs::write_file(base::join_path(root, "mcppls-fake-npm.cmd"), "");
            (void)mcppls::platform::fs::write_file(base::join_path(root, "mcppls-fake-npm"), "");
            const auto found = env::find_executable("mcppls-fake-npm", root);
            expect(fatal(found.has_value()));
            expect(base::same_path(*found, base::join_path(root, "mcppls-fake-npm.cmd"))) << *found;

            // Asked for by its own name, a shim is taken as written rather than suffixed again.
            const auto byName = env::find_executable("mcppls-fake-npm.cmd", root);
            expect(byName.has_value() && base::same_path(*byName, base::join_path(root, "mcppls-fake-npm.cmd")))
                << byName.value_or("<none>");

            // An extension-less file is not executable here, whatever POSIX would say.
            (void)mcppls::platform::fs::write_file(base::join_path(root, "mcppls-fake-script"), "");
            expect(!env::find_executable("mcppls-fake-script", root).has_value());
        }

        // Everywhere: a dotted name is not a name with an extension, so it is still suffixed.
        const std::string dotted { std::string { "mcppls-fake-1.13" } + std::string { mcppls::os::EXECUTABLE_SUFFIX } };
        (void)mcppls::platform::fs::write_file(base::join_path(root, dotted), "");
        const auto byAppending = env::find_executable("mcppls-fake-1.13", root);
        expect(byAppending.has_value() && base::same_path(*byAppending, base::join_path(root, dotted)))
            << byAppending.value_or("<none>");

        mcppls::platform::fs::remove_all(root);
    };

    "arguments include this program"_test = [] {
        const auto arguments = env::arguments();
        expect(fatal(!arguments.empty()));
        expect(arguments.front().find("test_env") != std::string::npos) << arguments.front();
    };

    "well-known directories are absolute"_test = [] {
        expect(base::is_absolute_path(mcppls::platform::dirs::home_directory())) << mcppls::platform::dirs::home_directory();
        expect(base::is_absolute_path(mcppls::platform::dirs::temp_directory())) << mcppls::platform::dirs::temp_directory();
        const std::string cache { mcppls::platform::dirs::cache_directory() };
        expect(base::is_absolute_path(cache)) << cache;
        expect(cache.ends_with("mcppls") || env::get("MCPPLS_CACHE_DIR").has_value()) << cache;
    };

    // Design 4.3. An editor started from a desktop entry has none of the user's shell configuration,
    // so the server asks the login shell and merges what it says over its own environment.
    "the login shell's values win, but the session's own do not travel"_test = [] {
        namespace toolenv = mcppls::platform::toolenv;
        const std::vector<std::string> editor { "PATH=/usr/bin", "HOME=/home/u", "SHLVL=2", "VSCODE_PID=7", "TERM=dumb", "LANG=C" };
        const std::vector<std::string> shell { "PATH=/home/u/.xlings/bin:/usr/bin", "XLINGS_HOME=/home/u/.xlings", "SHLVL=9",
                                               "PWD=/somewhere", "TERM=xterm", "VSCODE_RESOLVING_ENVIRONMENT=1", "MCPPLS_RESOLVING_ENVIRONMENT=1",
                                               "LANG=C" };
        std::vector<std::string> differing;
        const auto merged = toolenv::merge(editor, shell, &differing);
        auto value_of = [&](std::string_view name) -> std::string {
            for (const auto& variable : merged) {
                if (variable.starts_with(std::format("{}=", name))) return variable.substr(name.size() + 1);
            }
            return "<unset>";
        };
        expect(value_of("PATH") == "/home/u/.xlings/bin:/usr/bin") << value_of("PATH");
        expect(value_of("XLINGS_HOME") == "/home/u/.xlings");
        expect(value_of("HOME") == "/home/u") << "what the shell did not mention stays";
        expect(value_of("SHLVL") == "2") << "the shell's own depth is not the editor's";
        expect(value_of("TERM") == "dumb") << "a terminal the tool does not have is not described to it";
        expect(value_of("VSCODE_PID") == "7");
        expect(value_of("MCPPLS_RESOLVING_ENVIRONMENT") == "<unset>") << "the marker the server itself set does not come back";
        // Only what actually changed is reported, and by name only: a value may be a credential.
        expect(std::ranges::find(differing, "PATH") != differing.end());
        expect(std::ranges::find(differing, "XLINGS_HOME") != differing.end());
        expect(std::ranges::find(differing, "LANG") == differing.end()) << "an identical value is not a difference";
        expect(std::ranges::find(differing, "SHLVL") == differing.end());
    };

    "an environment is read between the markers, and nothing else is"_test = [] {
        namespace toolenv = mcppls::platform::toolenv;
        const std::string text { toolenv::print_environment_text("MARK-1") };
        auto parsed = toolenv::parse_marked(std::format("a shell greeting\n{}more chatter\n", text), "MARK-1");
        expect(fatal(parsed.has_value())) << text;
        expect(std::ranges::any_of(*parsed, [](const std::string& variable) { return variable.starts_with("PATH="); }));
        expect(toolenv::parse_marked("nothing here", "MARK-1") == std::nullopt);
        expect(toolenv::parse_marked("MARK-1\nnot json\nMARK-1\n", "MARK-1") == std::nullopt);
        expect(toolenv::parse_marked("MARK-1\n{\"A\":\"1\"}\n", "MARK-1") == std::nullopt) << "one marker is not a delimited answer";
    };

    "the modes are named as the setting spells them"_test = [] {
        namespace toolenv = mcppls::platform::toolenv;
        expect(toolenv::parse_mode("auto") == toolenv::Mode::automatic);
        expect(toolenv::parse_mode("editor") == toolenv::Mode::editor);
        expect(toolenv::parse_mode("something-else") == std::nullopt);
        expect(toolenv::mode_name(toolenv::Mode::editor) == "editor");
    };

    // The whole of design 4.3 through a real shell: the server asks it to run this program's
    // `print-environment`, and reads what comes back between the markers. A shell that greets, sets
    // variables or prints anything else does not disturb the answer.
    "the login shell's environment is read through a real shell"_test = [] {
        if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) {
            return;   // a graphical program takes its environment from the registry; there is no shell to ask
        } else {
            namespace toolenv = mcppls::platform::toolenv;
            namespace fs = mcppls::platform::fs;
            const auto arguments = env::arguments();
            std::string self { arguments.empty() ? std::string {} : arguments.front() };
            if (!base::is_absolute_path(self)) self = base::join_path(fs::current_directory(), self);
            self = base::normalize_path(self);
            if (!fs::is_regular_file("/bin/sh")) return;

            toolenv::reset();
            toolenv::configure(toolenv::Mode::automatic, self, "/bin/sh");
            toolenv::begin();
            const auto resolved = toolenv::get(std::chrono::seconds { 20 });
            expect(resolved.resolved) << resolved.reason;
            expect(resolved.source == "login-shell") << resolved.reason;
            expect(std::ranges::any_of(resolved.variables, [](const std::string& variable) { return variable.starts_with("PATH="); }));
            toolenv::reset();

            // And with the setting at `editor` the shell is not asked at all.
            toolenv::configure(toolenv::Mode::editor, self, "/bin/sh");
            toolenv::begin();
            const auto editorOnly = toolenv::get(std::chrono::seconds { 5 });
            expect(editorOnly.source == "editor") << editorOnly.reason;
            toolenv::reset();
        }
    };

    return report();
}
