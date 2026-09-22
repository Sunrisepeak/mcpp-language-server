// The test program starts copies of itself as the child, so the same test runs
// on every system without depending on a shell or a system utility. The one
// exception is on Windows, where the command interpreter is itself the subject:
// build tools there are often batch files, and those run through it.
import std;
import mcppls.os;
import mcppls.testing;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.process;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.platform.dirs;
import mcppls.platform.stdio;

namespace platform = mcppls::platform;
namespace base = mcppls::base;

namespace {

std::string self_path() {
    const auto arguments = platform::env::arguments();
    std::string path { arguments.empty() ? std::string {} : arguments.front() };
    if (!base::is_absolute_path(path)) path = base::join_path(platform::fs::current_directory(), path);
    return base::normalize_path(path);
}

int child_main(std::span<const std::string> arguments) {
    const std::string_view mode { arguments[1] };
    if (mode == "--echo") {
        while (true) {
            auto chunk = platform::stdio::read_input();
            if (!chunk || chunk->empty()) break;
            (void)platform::stdio::write_output(*chunk);
        }
        return 0;
    }
    if (mode == "--exit") return std::stoi(arguments[2]);
    if (mode == "--sleep") {
        std::this_thread::sleep_for(std::chrono::seconds { 30 });
        return 0;
    }
    if (mode == "--env") {
        (void)platform::stdio::write_output(platform::env::get(arguments[2]).value_or("<unset>"));
        return 0;
    }
    if (mode == "--read-relative") {
        std::ifstream stream { arguments[2] };
        std::string line;
        std::getline(stream, line);
        (void)platform::stdio::write_output(stream || !line.empty() ? line : std::string { "<missing>" });
        return 0;
    }
    if (mode == "--arguments") {
        std::string joined;
        for (std::size_t i { 2 }; i < arguments.size(); ++i) joined += std::format("[{}]", arguments[i]);
        (void)platform::stdio::write_output(joined);
        return 0;
    }
    if (mode == "--stderr") {
        (void)platform::stdio::write_error("to-stderr");
        return 3;
    }
    // Sleeps while holding whatever streams it was given, then leaves a mark. The mark is how a
    // test tells "the unit was ended" from "the sleep finished".
    if (mode == "--descendant") {
        std::this_thread::sleep_for(std::chrono::seconds { 3 });
        (void)platform::fs::write_file(base::join_path(arguments[2], "late.txt"), "late");
        return 0;
    }
    // Starts a descendant that inherits this program's standard error --- the caller's pipe ---
    // and leaves at once. The caller's read of that pipe cannot end until the descendant does.
    //
    // `--hold-pipe` starts it as a unit of its own, which is what a tool started through a shell
    // does and what no group kill can reach: only a bounded read frees the caller. `--hold-pipe-
    // and-sleep` starts an ordinary child, whose lifetime is bound to this one.
    if (mode == "--hold-pipe" || mode == "--hold-pipe-and-sleep") {
        const bool ownUnit { mode == "--hold-pipe" };
        auto child = platform::Process::spawn({ .program = self_path(),
                                                .arguments = { "--descendant", arguments[2] },
                                                .pipeInput = false, .pipeOutput = false, .pipeError = false,
                                                .detached = ownUnit,
                                                // Not bound to this program's lifetime, which is what a
                                                // build tool's own children are: only ending the unit reaches them.
                                                .boundLifetime = false });
        if (child) child->detach();
        (void)platform::stdio::write_output(child ? "started" : std::format("spawn-failed:{}", child.error().message));
        if (!ownUnit) std::this_thread::sleep_for(std::chrono::seconds { 30 });
        return 0;
    }
    return 100;
}

} // namespace

int main() {
    const auto arguments = platform::env::arguments();
    // A child leaves without running the test framework's report at exit.
    if (arguments.size() >= 2 && arguments[1].starts_with("--")) std::_Exit(child_main(arguments));

    using namespace mcppls::testing;
    const std::string self { self_path() };

    "self path is an existing absolute file"_test = [&] {
        expect(base::is_absolute_path(self)) << self;
        expect(platform::fs::is_regular_file(self)) << self;
    };

    "echo round trip through pipes"_test = [&] {
        auto process = platform::Process::spawn({ .program = self, .arguments = { "--echo" } });
        expect(fatal(process.has_value())) << (process ? "" : process.error().message);
        std::string collected;
        std::jthread reader { [&] {
            while (true) {
                auto chunk = process->read_output();
                if (!chunk || chunk->empty()) break;
                collected += *chunk;
            }
        } };
        expect(process->write("hello\n").has_value());
        expect(process->write("from the parent\n").has_value());
        process->close_input();
        reader.join();
        auto status = process->wait();
        expect(status.has_value() && *status == 0);
        expect(collected == "hello\nfrom the parent\n") << collected;
    };

    // Each argument arrives exactly as given, whatever the system does to pass a
    // vector through one command line.
    "arguments arrive unaltered"_test = [&] {
        const std::vector<std::string> given { "plain", "two words", "", "C:\\dir\\file.txt", "trailing\\",
                                               "quote\"inside", "back\\\"quote", "\\\\server\\share", "tab\there", "--flag=a b" };
        std::vector<std::string> arguments { "--arguments" };
        arguments.insert(arguments.end(), given.begin(), given.end());
        auto result = platform::run({ .program = self, .arguments = arguments }, std::chrono::seconds { 60 });
        expect(fatal(result.has_value()));
        std::string expected;
        for (const auto& argument : given) expected += std::format("[{}]", argument);
        expect(result->output == expected) << result->output;
    };

    // The stress check's process-tree sampler (mcppls-conformance) reads this to find the server
    // it started on POSIX, where openkal's process handle is the OS pid it waits on with wait4.
    "native_pid is the process a signal of 0 reaches while it runs, or nullopt on Windows"_test = [&] {
        auto process = platform::Process::spawn({ .program = self, .arguments = { "--sleep" } });
        expect(fatal(process.has_value())) << (process ? "" : process.error().message);
        const auto pid { process->native_pid() };
        if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) {
            expect(!pid.has_value());
        } else {
            expect(fatal(pid.has_value()));
            expect(*pid > 0) << *pid;
            if constexpr (mcppls::os::FAMILY == mcppls::os::Family::linux) {
                expect(platform::fs::is_directory(std::format("/proc/{}", *pid))) << *pid;
            }
        }
        process->kill();
        (void)process->wait();
        if constexpr (mcppls::os::FAMILY != mcppls::os::Family::windows) expect(!process->native_pid().has_value());
    };

    "exit status is propagated"_test = [&] {
        auto result = platform::run({ .program = self, .arguments = { "--exit", "7" } }, std::chrono::seconds { 60 });
        expect(fatal(result.has_value()));
        expect(result->exitCode == 7) << result->exitCode;
        expect(!result->timedOut);
    };

    "standard error is captured separately"_test = [&] {
        auto result = platform::run({ .program = self, .arguments = { "--stderr" } }, std::chrono::seconds { 60 });
        expect(fatal(result.has_value()));
        expect(result->exitCode == 3);
        expect(result->error == "to-stderr") << result->error;
        expect(result->output.empty());
    };

    "a bound on waiting ends a sleeping child"_test = [&] {
        const auto started = std::chrono::steady_clock::now();
        auto result = platform::run({ .program = self, .arguments = { "--sleep" } }, std::chrono::milliseconds { 500 });
        const auto elapsed = std::chrono::steady_clock::now() - started;
        expect(fatal(result.has_value()));
        expect(result->timedOut);
        expect(elapsed < std::chrono::seconds { 20 });
    };

    "environment reaches the child"_test = [&] {
        auto environment = platform::env::variables();
        environment.push_back("MCPPLS_TEST_VARIABLE=from-parent");
        auto result = platform::run({ .program = self, .arguments = { "--env", "MCPPLS_TEST_VARIABLE" },
                                      .environment = environment }, std::chrono::seconds { 60 });
        expect(fatal(result.has_value()));
        expect(result->output == "from-parent") << result->output;
    };

    // A value is reported as it was set, backslashes included: a program that
    // starts the command interpreter from %ComSpec% received a forward-slashed
    // name, which the interpreter reads as switches.
    "an environment value arrives unaltered"_test = [&] {
        auto environment = platform::env::variables();
        environment.push_back("MCPPLS_TEST_PATHS=C:\\dir\\sub;\\\\host\\share\\x;a\"b");
        auto result = platform::run({ .program = self, .arguments = { "--env", "MCPPLS_TEST_PATHS" },
                                      .environment = environment }, std::chrono::seconds { 60 });
        expect(fatal(result.has_value()));
        expect(result->output == "C:\\dir\\sub;\\\\host\\share\\x;a\"b") << result->output;
    };

    "work directory is where relative names resolve"_test = [&] {
        const std::string directory { base::join_path(platform::dirs::temp_directory(),
            std::format("mcppls-test-process-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        expect(fatal(platform::fs::create_directories(directory).has_value())) << directory;
        expect(platform::fs::write_file(base::join_path(directory, "marker.txt"), "marker-content\n").has_value());
        auto result = platform::run({ .program = self, .arguments = { "--read-relative", "marker.txt" },
                                      .workDirectory = directory }, std::chrono::seconds { 60 });
        expect(fatal(result.has_value()));
        expect(result->output == "marker-content") << result->output;
        platform::fs::remove_all(directory);
    };

    // The command interpreter refuses a current directory given in the `\\?\` form
    // ("UNC paths are not supported") and runs in the Windows directory instead,
    // so a batch file started in a project ran somewhere else.
    "the command interpreter runs in the work directory"_test = [&] {
        if constexpr (mcppls::os::FAMILY != mcppls::os::Family::windows) {
            return;
        } else {
            const std::string directory { base::join_path(platform::dirs::temp_directory(),
                std::format("mcppls-test-cmd-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
            expect(fatal(platform::fs::create_directories(directory).has_value())) << directory;
            expect(platform::fs::write_file(base::join_path(directory, "marker.txt"), "marker-content").has_value());
            const std::string interpreter { base::normalize_path(platform::env::get("ComSpec").value_or("C:/Windows/System32/cmd.exe")) };
            auto result = platform::run({ .program = interpreter, .arguments = { "/d", "/c", "type", "marker.txt" },
                                          .workDirectory = directory }, std::chrono::seconds { 60 });
            expect(fatal(result.has_value()));
            expect(base::trim(result->output) == "marker-content") << result->output << result->error;
            expect(!result->error.contains("UNC")) << result->error;
            platform::fs::remove_all(directory);
        }
    };

    // The failure this runner exists for: a program leaves behind something that inherited the
    // caller's pipe. Reading to the end of the stream then waits for the descendant, which is how
    // one hung index refresh kept a language server without a build database for twelve minutes.
    "a descendant holding a pipe does not hold the caller, and the unit is ended"_test = [&] {
        const std::string directory { base::join_path(platform::dirs::temp_directory(),
            std::format("mcppls-test-hold-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        expect(fatal(platform::fs::create_directories(directory).has_value())) << directory;
        const auto started = std::chrono::steady_clock::now();
        auto result = platform::run({ .program = self, .arguments = { "--hold-pipe", directory } },
                                    platform::RunBounds { .hard = std::chrono::seconds { 30 },
                                                          .drain = std::chrono::seconds { 1 } });
        const auto elapsed = std::chrono::steady_clock::now() - started;
        expect(fatal(result.has_value()));
        expect(result->exitCode == 0) << result->exitCode;
        expect(!result->timedOut);
        expect(result->outputHeldOpen) << "the descendant held the pipe and should have been reported";
        expect(elapsed < std::chrono::seconds { 3 }) << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        expect(result->output == "started") << result->output;
        // `outputHeldOpen` is itself the proof that the descendant was alive and holding the pipe
        // through the drain: had it gone with its parent, the stream would have ended and there
        // would have been nothing to report. What happens to it afterwards is the system's own
        // business --- a unit of its own outlives the caller on POSIX and is collected with its
        // job on Windows --- and is not what this test is about.
        std::this_thread::sleep_for(std::chrono::seconds { 4 });
        platform::fs::remove_all(directory);
    };

    // The child leaves politely when asked; what it started does not end with it (no lifetime
    // binding, as on macOS and as for anything a build tool starts through a shell). Only ending
    // the unit reaches that, and the bound must do it whether or not the child went quietly.
    "the hard bound ends what the tool started, not only the tool"_test = [&] {
        const std::string directory { base::join_path(platform::dirs::temp_directory(),
            std::format("mcppls-test-unit-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        expect(fatal(platform::fs::create_directories(directory).has_value())) << directory;
        const auto started = std::chrono::steady_clock::now();
        auto result = platform::run({ .program = self, .arguments = { "--hold-pipe-and-sleep", directory } },
                                    platform::RunBounds { .hard = std::chrono::milliseconds { 300 },
                                                          .grace = std::chrono::milliseconds { 200 },
                                                          .drain = std::chrono::milliseconds { 500 } });
        const auto elapsed = std::chrono::steady_clock::now() - started;
        expect(fatal(result.has_value()));
        expect(result->timedOut);
        expect(elapsed < std::chrono::seconds { 5 }) << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        std::this_thread::sleep_for(std::chrono::seconds { 4 });
        expect(!platform::fs::exists(base::join_path(directory, "late.txt")));
        platform::fs::remove_all(directory);
    };

    "output above the limit is truncated and said to be"_test = [&] {
        auto result = platform::run({ .program = self, .arguments = { "--arguments", std::string(4096, 'x') } },
                                    platform::RunBounds { .hard = std::chrono::seconds { 60 }, .outputLimit = 512 });
        expect(fatal(result.has_value()));
        expect(result->outputTruncated);
        expect(result->output.size() == 512) << result->output.size();
    };

    "the last lines of a stream are what a record keeps"_test = [] {
        expect(platform::last_lines("a\nb\nc\n", 2) == "b\nc") << platform::last_lines("a\nb\nc\n", 2);
        expect(platform::last_lines("only", 3) == "only");
        expect(platform::last_lines("", 3).empty());
    };

    "a relative program path is refused"_test = [] {
        auto process = platform::Process::spawn({ .program = "relative/program" });
        expect(!process.has_value());
    };

    "every absolute path of this program matches a preopen"_test = [&] {
        expect(platform::match_preopen(self).has_value()) << self;
    };

    return report();
}
