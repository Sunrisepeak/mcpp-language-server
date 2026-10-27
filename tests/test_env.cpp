import std;
import mcppls.testing;
import mcppls.base.path;
import mcpplibs.os;
import mcppls.base.home;
import mcppls.toolrun;
import mcppls.toolenv;

namespace toolrun = mcppls::toolrun;

namespace os = mcpplibs::os;
namespace base = mcppls::base;

int main() {
    // Stands in for `mcppls print-environment`: the login shell runs this, and what it prints
    // between the markers is the environment the shell had. A child leaves before the suite runs.
    {
        const auto arguments = os::env::arguments();
        if (arguments.size() == 3 && arguments[1] == "print-environment") {
            std::print("{}", mcppls::toolenv::print_environment_text(arguments[2]));
            std::_Exit(0);
        }
    }
    using namespace mcppls::testing;

    "the environment is visible"_test = [] {
        expect(!os::env::variables().empty());
        expect(os::env::get("PATH").has_value());
        expect(!os::env::search_path().empty());
        expect(!os::env::get("MCPPLS_SURELY_UNSET_VARIABLE").has_value());
    };

    "a value is reported as it was set"_test = [] {
        if (os::host() == os::platform::windows) {
            const auto interpreter = os::env::get("ComSpec");
            expect(fatal(interpreter.has_value()));
            expect(interpreter->contains('\\') && !interpreter->contains('/')) << *interpreter;
        } else {
            expect(os::env::get("PATH").value_or("").contains('/'));
        }
    };

    "a system executable is found on PATH"_test = [] {
        if (os::host() == os::platform::windows) {
            const auto found = toolrun::find_program("cmd");
            expect(fatal(found.has_value()));
            expect(base::file_name(*found) == "cmd.exe" || base::file_name(*found) == "CMD.EXE") << *found;
        } else {
            const auto found = toolrun::find_program("sh");
            expect(fatal(found.has_value()));
            expect(base::is_absolute_path(*found)) << *found;
        }
        expect(!toolrun::find_program("mcppls-no-such-program").has_value());
    };

    "an executable is found on another environment's PATH"_test = [] {
        std::error_code ignored;
        const std::string first { base::join_path(base::spelled(os::dirs::temp()),
            std::format("mcppls-test-env-first-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        const std::string second { first + "-second" };
        const std::string name { std::string { "mcppls-fake-tool" } + std::string { os::conventions().exe_suffix } };
        (void)std::filesystem::create_directories(first, ignored);
        (void)std::filesystem::create_directories(second, ignored);
        (void)os::fs::write(base::join_path(second, name), "");
        const std::string pathList { first + std::string { os::conventions().path_list_separator } + second };
        const auto found = toolrun::find_program("mcppls-fake-tool", pathList);
        expect(found.has_value() && base::same_path(*found, base::join_path(second, name))) << found.value_or("<none>");
        expect(!toolrun::find_program("mcppls-fake-tool").has_value()) << "not on this process's PATH";
        expect(!toolrun::find_program("mcppls-fake-tool", first).has_value());
        std::filesystem::remove_all(first, ignored);
        std::filesystem::remove_all(second, ignored);
    };

    // Windows executability is the extension, and there are several of them. npm, npx, code,
    // gradle and xlings are `.cmd` shims with no `.exe` anywhere — and npm's directory ALSO holds an
    // extension-less `npm`, the POSIX shell script, which Windows cannot run at all. Both halves
    // cost a CI cycle: appending blindly asked for `npm.cmd.exe`, and then accepting the bare name
    // picked the shell script and failed with an I/O error at spawn.
    "a Windows executable is found by extension, and only by extension"_test = [] {
        std::error_code ignored;
        const std::string root { base::join_path(base::spelled(os::dirs::temp()),
            std::format("mcppls-test-env-shim-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        (void)std::filesystem::create_directories(root, ignored);

        if (os::host() == os::platform::windows) {
            // npm's layout: the shim, and the shell script beside it. The shim is the answer.
            (void)os::fs::write(base::join_path(root, "mcppls-fake-npm.cmd"), "");
            (void)os::fs::write(base::join_path(root, "mcppls-fake-npm"), "");
            const auto found = toolrun::find_program("mcppls-fake-npm", root);
            expect(fatal(found.has_value()));
            expect(base::same_path(*found, base::join_path(root, "mcppls-fake-npm.cmd"))) << *found;

            // Asked for by its own name, a shim is taken as written rather than suffixed again.
            const auto byName = toolrun::find_program("mcppls-fake-npm.cmd", root);
            expect(byName.has_value() && base::same_path(*byName, base::join_path(root, "mcppls-fake-npm.cmd")))
                << byName.value_or("<none>");

            // An extension-less file is not executable here, whatever POSIX would say.
            (void)os::fs::write(base::join_path(root, "mcppls-fake-script"), "");
            expect(!toolrun::find_program("mcppls-fake-script", root).has_value());
        }

        // Everywhere: a dotted name is not a name with an extension, so it is still suffixed.
        const std::string dotted { std::string { "mcppls-fake-1.13" } + std::string { os::conventions().exe_suffix } };
        (void)os::fs::write(base::join_path(root, dotted), "");
        const auto byAppending = toolrun::find_program("mcppls-fake-1.13", root);
        expect(byAppending.has_value() && base::same_path(*byAppending, base::join_path(root, dotted)))
            << byAppending.value_or("<none>");

        std::filesystem::remove_all(root, ignored);
    };

    "arguments include this program"_test = [] {
        const auto arguments = os::env::arguments();
        expect(fatal(!arguments.empty()));
        expect(arguments.front().find("test_env") != std::string::npos) << arguments.front();
    };

    "well-known directories are absolute"_test = [] {
        expect(base::is_absolute_path(base::home_directory())) << base::home_directory();
        expect(base::is_absolute_path(base::spelled(os::dirs::temp()))) << base::spelled(os::dirs::temp());
        const std::string cache { base::cache_directory() };
        expect(base::is_absolute_path(cache)) << cache;
        expect(cache.ends_with("mcppls") || os::env::get("MCPPLS_CACHE_DIR").has_value()) << cache;
    };

    // Design 4.3. An editor started from a desktop entry has none of the user's shell configuration,
    // so the server asks the login shell and merges what it says over its own environment.
    "the login shell's values win, but the session's own do not travel"_test = [] {
        namespace toolenv = mcppls::toolenv;
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
        namespace toolenv = mcppls::toolenv;
        const std::string text { toolenv::print_environment_text("MARK-1") };
        auto parsed = toolenv::parse_marked(std::format("a shell greeting\n{}more chatter\n", text), "MARK-1");
        expect(fatal(parsed.has_value())) << text;
        expect(std::ranges::any_of(*parsed, [](const std::string& variable) { return variable.starts_with("PATH="); }));
        expect(toolenv::parse_marked("nothing here", "MARK-1") == std::nullopt);
        expect(toolenv::parse_marked("MARK-1\nnot json\nMARK-1\n", "MARK-1") == std::nullopt);
        expect(toolenv::parse_marked("MARK-1\n{\"A\":\"1\"}\n", "MARK-1") == std::nullopt) << "one marker is not a delimited answer";
    };

    "the modes are named as the setting spells them"_test = [] {
        namespace toolenv = mcppls::toolenv;
        expect(toolenv::parse_mode("auto") == toolenv::Mode::automatic);
        expect(toolenv::parse_mode("editor") == toolenv::Mode::editor);
        expect(toolenv::parse_mode("something-else") == std::nullopt);
        expect(toolenv::mode_name(toolenv::Mode::editor) == "editor");
    };

    // The whole of design 4.3 through a real shell: the server asks it to run this program's
    // `print-environment`, and reads what comes back between the markers. A shell that greets, sets
    // variables or prints anything else does not disturb the answer.
    "the login shell's environment is read through a real shell"_test = [] {
        std::error_code ignored;
        if (os::host() == os::platform::windows) {
            return;   // a graphical program takes its environment from the registry; there is no shell to ask
        } else {
            namespace toolenv = mcppls::toolenv;
            // What starts this program again: the executable, or an ok image's loader and image.
            const auto self = os::env::self_command();
            expect(fatal(!self.empty()));
            expect(base::is_absolute_path(self.front())) << self.front();
            if (!std::filesystem::is_regular_file("/bin/sh", ignored)) return;

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
