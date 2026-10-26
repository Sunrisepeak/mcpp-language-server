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
import mcppls.lsp.connection;
import nlohmann.json;

namespace platform = mcppls::platform;
namespace base = mcppls::base;

namespace {

// An independent work oracle: the C library's processor clock (`std::clock`, which openkal-musl
// answers from this program's own kal_program_info since 0.22), not platform::cpu_seconds, which
// asks about another program by its identifier through mcpplibs.os.
std::optional<double> native_self_cpu_seconds() {
    const std::clock_t now { std::clock() };
    if (now == static_cast<std::clock_t>(-1)) return std::nullopt;
    return static_cast<double>(now) / 1'000'000.0;   // CLOCKS_PER_SEC, which POSIX fixes at a million
}

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
    if (mode == "--cpu-idle") {
        (void)platform::stdio::write_output("ready\n");
        (void)platform::stdio::read_input();   // stay alive and idle until the parent ends the test
        return 0;
    }
    if (mode == "--spin-cpu") {
        (void)platform::stdio::write_output("ready\n");
        std::string start;
        while (!start.ends_with('\n') && start.size() < 64) {
            const auto part = platform::stdio::read_input();
            if (!part || part->empty()) return 101;
            start += *part;
        }
        if (start != "start\n") return 101;
        const auto before = native_self_cpu_seconds();
        if (!before) {
            (void)platform::stdio::write_output("cpu-oracle-unavailable\n");
            return 102;
        }
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds { 30 };
        volatile std::uint64_t sink { 0 };
        bool completed { false };
        double completedCpu { 0 };
        while (std::chrono::steady_clock::now() < until) {
            for (int i { 0 }; i < 4096; ++i) sink = sink + 1;
            const auto cpu = native_self_cpu_seconds();
            if (!cpu || *cpu < *before) {
                (void)platform::stdio::write_output("cpu-oracle-invalid\n");
                return 103;
            }
            completedCpu = *cpu - *before;
            if (completedCpu >= 0.75) { completed = true; break; }
        }
        (void)platform::stdio::write_output(completed ? std::format("done {:.9f}\n", completedCpu) : "cpu-work-timeout\n");
        (void)platform::stdio::read_input();   // keep the completed process available to cpu_seconds
        return completed ? 0 : 104;
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
    // Plan 2026-09-30 §13: clangd reading slowly on a busy Windows machine held every send, and the server's event
    // loop with it, 11 to 13 s an edit. What is sent to a peer is queued for a writer thread of the connection's own.
    "a peer that reads nothing does not hold what is sent to it, and stopping it still ends"_test = [&] {
        using namespace std::chrono_literals;
        auto connection = mcppls::lsp::Connection::start({ .program = self, .arguments = { "--sleep" } }, [](nlohmann::json) {}, [] {});
        expect(fatal(connection.has_value())) << (connection ? "" : connection.error().message);
        const std::string big(64 * 1024, 'x');
        const auto started = std::chrono::steady_clock::now();
        bool sent { true };
        for (int i { 0 }; i < 64; ++i) {
            sent = sent && (*connection)->send(nlohmann::json { { "jsonrpc", "2.0" }, { "method", "m" }, { "params", big } }).has_value();
        }
        expect(sent);
        const auto sendElapsed = std::chrono::steady_clock::now() - started;
        expect(sendElapsed < 2s) << "4 MB to a peer that reads none of it; actual "
                               << std::chrono::duration<double>(sendElapsed).count() << " s";
        const auto stopping = std::chrono::steady_clock::now();
        (*connection)->stop(200ms);
        const auto stopElapsed = std::chrono::steady_clock::now() - stopping;
        std::println("connection nonreader evidence: 64 x 64 KiB send={:.9f}s; stop={:.9f}s",
                     std::chrono::duration<double>(sendElapsed).count(), std::chrono::duration<double>(stopElapsed).count());
        expect(stopElapsed < 10s) << "the writer, blocked in a write, lets go once the peer is gone; actual "
                                 << std::chrono::duration<double>(stopElapsed).count() << " s";
    };

    "connection writer drains owned snapshots in FIFO order through both framings"_test = [&] {
        using namespace std::chrono_literals;
        using Json = nlohmann::json;
        for (const auto framing : { mcppls::lsp::Framing::content_length, mcppls::lsp::Framing::lines }) {
            const std::string_view framingName { framing == mcppls::lsp::Framing::lines ? "lines" : "content-length" };
            struct Received {
                std::mutex mutex;
                std::condition_variable ready;
                std::vector<Json> messages;
                bool release { false };
                bool gateTimedOut { false };
                bool closed { false };
            } received;
            auto connection = mcppls::lsp::Connection::start(
                { .program = self, .arguments = { "--echo" } },
                [&](Json message) {
                    std::unique_lock lock { received.mutex };
                    received.messages.push_back(std::move(message));
                    received.ready.notify_all();
                    // Hold the first reply while later messages queue. A large following frame
                    // also supplies backpressure, so the writer must own queued nested values.
                    if (received.messages.size() == 1
                        && !received.ready.wait_for(lock, 20s, [&] { return received.release; })) received.gateTimedOut = true;
                },
                [&] {
                    const std::lock_guard lock { received.mutex };
                    received.closed = true;
                    received.ready.notify_all();
                }, {}, framing);
            expect(fatal(connection.has_value())) << framingName;
            // On any assertion/exception unwind, release the callback before Connection joins it.
            struct Release {
                Received& received;
                void open() {
                    { const std::lock_guard lock { received.mutex }; received.release = true; }
                    received.ready.notify_all();
                }
                ~Release() { open(); }
            } release { received };
            std::vector<Json> expected;
            const auto send = [&](Json message) {
                expected.push_back(message);
                return (*connection)->send(std::move(message)).has_value();
            };
            bool sent { send(Json { { "jsonrpc", "2.0" }, { "id", 0 }, { "result", "first" } }) };
            bool firstArrived { false };
            {
                std::unique_lock lock { received.mutex };
                firstArrived = received.ready.wait_for(lock, 20s, [&] { return !received.messages.empty() || received.closed; })
                            && !received.messages.empty();
            }
            expect(firstArrived) << framingName << " first echo did not arrive";
            sent = send(Json { { "jsonrpc", "2.0" }, { "id", 1 }, { "result", std::string(4 * 1024 * 1024, 'b') } }) && sent;
            Json original { { "jsonrpc", "2.0" }, { "id", 2 },
                            { "result", { { "nested", { { "text", "h\xC3\xA9llo \xF0\x9F\x98\x80\n" },
                                                        { "values", Json::array({ 1, 2, 3 }) } } } } } };
            expected.push_back(original);
            sent = (*connection)->send(original).has_value() && sent;
            original["result"]["nested"]["text"] = "changed after send";
            original["result"]["nested"]["values"][0] = 99;
            original.clear();
            {
                Json temporary { { "jsonrpc", "2.0" }, { "id", 3 }, { "result", Json::array({ "temporary", 7 }) } };
                expected.push_back(temporary);
                sent = (*connection)->send(std::move(temporary)).has_value() && sent;
            } // The moved-from source is destroyed before the writer is released.
            Json invalid { { "jsonrpc", "2.0" }, { "id", 4 }, { "result", std::string { "A\xFF\xFE" "Z" } } };
            expected.push_back(Json { { "jsonrpc", "2.0" }, { "id", 4 }, { "result", "A\xEF\xBF\xBD\xEF\xBF\xBD" "Z" } });
            sent = (*connection)->send(std::move(invalid)).has_value() && sent;
            sent = send(Json { { "jsonrpc", "2.0" }, { "id", 5 }, { "result", "last" } }) && sent;
            release.open();
            // Stop without first waiting for all replies: normal shutdown must drain the queue,
            // close stdin, then allow --echo to finish and the reader to deliver every reply.
            (*connection)->stop(20s);
            expect(sent) << framingName;
            expect(!received.gateTimedOut) << framingName << " first-reply gate timed out";
            expect(received.closed && (*connection)->closed()) << framingName;
            expect(received.messages.size() == expected.size()) << framingName << " expected " << expected.size()
                                                               << " replies, received " << received.messages.size();
            for (std::size_t i { 0 }; i < std::min(received.messages.size(), expected.size()); ++i) {
                expect(received.messages[i] == expected[i]) << framingName << " reply " << i << " differs from its sent snapshot";
            }
            expect(!(*connection)->send(Json { { "after", "stop" } }).has_value()) << framingName;
            (*connection)->stop(200ms); // Repeated cleanup must remain harmless after a normal drain.
        }
    };

    "connection reports a departed peer and stopping it cleans up"_test = [&] {
        using namespace std::chrono_literals;
        std::mutex mutex;
        std::condition_variable ready;
        bool closed { false };
        auto connection = mcppls::lsp::Connection::start(
            { .program = self, .arguments = { "--exit", "0" } }, [](nlohmann::json) {},
            [&] { const std::lock_guard lock { mutex }; closed = true; ready.notify_all(); });
        expect(fatal(connection.has_value()));
        bool observedClosed { false };
        {
            std::unique_lock lock { mutex };
            observedClosed = ready.wait_for(lock, 20s, [&] { return closed; });
        }
        expect(observedClosed) << "the real child exited without closing the connection";
        const auto sent = (*connection)->send(nlohmann::json { { "jsonrpc", "2.0" }, { "method", "after-exit" } });
        expect(!sent.has_value());
        if (!sent) expect(sent.error().code == "connection-closed" || sent.error().code == "connection-write") << sent.error().code;
        const auto stopping = std::chrono::steady_clock::now();
        (*connection)->stop(200ms);
        (*connection)->stop(200ms);
        expect(std::chrono::steady_clock::now() - stopping < 10s) << "departed peer cleanup";
        expect((*connection)->closed());
    };

    "writing after the child exits returns an error and preserves the parent"_test = [&] {
        auto process = platform::Process::spawn({ .program = self, .arguments = { "--exit", "0" } });
        expect(fatal(process.has_value()));
        const auto status = process->wait();
        expect(fatal(status.has_value()));
        expect(*status == 0);
        const auto written = process->write("peer is gone");
        expect(!written.has_value());
        if (!written) expect(written.error().code == "process-write");
    };

    "native_pid is the child's identifier while it runs, on every system"_test = [&] {
        auto process = platform::Process::spawn({ .program = self, .arguments = { "--sleep" } });
        expect(fatal(process.has_value())) << (process ? "" : process.error().message);
        const auto pid { process->native_pid() };
        expect(fatal(pid.has_value()));
        expect(*pid > 0) << *pid;
        expect(platform::process_alive(*pid) == std::optional<bool> { true }) << *pid;
        if (mcppls::os::family() == mcppls::os::Family::linux) {
            expect(platform::fs::is_directory(std::format("/proc/{}", *pid))) << *pid;
        }
        process->kill();
        (void)process->wait();
        expect(!process->native_pid().has_value());
    };

    // StuckWatch in the clangd engine tells a stuck clangd from a busy one by this.
    "cpu_seconds tells a busy child from an idle one, on every system"_test = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds { 30 };
        auto idle = platform::Process::spawn({ .program = self, .arguments = { "--cpu-idle" } });
        auto busy = platform::Process::spawn({ .program = self, .arguments = { "--spin-cpu" } });
        expect(fatal(idle.has_value() && busy.has_value()));
        struct Cleanup {
            platform::Process& idle;
            platform::Process& busy;
            ~Cleanup() { idle.kill(); busy.kill(); (void)idle.wait(); (void)busy.wait(); }
        } cleanup { *idle, *busy };
        const auto announced = [&](platform::Process& process, std::string_view expected, std::string* message = nullptr) {
            std::string text;
            while (std::chrono::steady_clock::now() < deadline && text.size() < 128) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                if (left.count() <= 0) break;
                auto bytes = process.read_output_for(left);
                if (!bytes || !*bytes || (**bytes).empty()) return false;
                text += **bytes;
                if (text.ends_with('\n')) {
                    if (message != nullptr) { *message = text; return text.starts_with(expected); }
                    return text == expected;
                }
            }
            return false;
        };
        expect(fatal(announced(*idle, "ready\n") && announced(*busy, "ready\n"))) << "children did not become ready within the shared 30 s bound";
        const auto read = [](const platform::Process& process) -> std::optional<double> {
            const auto pid = process.native_pid();
            return pid ? platform::cpu_seconds(*pid) : std::nullopt;
        };
        const auto idleBefore = read(*idle);
        const auto busyBefore = read(*busy);
        std::optional<double> nativeWork;
        {
            expect(fatal(busy->write("start\n").has_value()));
            std::string done;
            expect(fatal(announced(*busy, "done ", &done))) << "independent native CPU work did not complete within the shared 30 s bound: " << done;
            const std::string_view number { base::trim(std::string_view { done }.substr(5)) };
            double seconds { 0 };
            const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), seconds);
            expect(fatal(error == std::errc {} && end == number.data() + number.size() && std::isfinite(seconds) && seconds >= 0.75))
                << "invalid independent CPU-work acknowledgement: " << done;
            nativeWork = seconds;
        }
        const auto idleAfter = read(*idle);
        const auto busyAfter = read(*busy);
        const auto value = [](const std::optional<double>& number) { return number ? std::format("{:.9f}", *number) : std::string { "unavailable" }; };
        std::println("cpu-seconds evidence: independent native work={}s; idle {} -> {}; busy {} -> {}",
                     value(nativeWork), value(idleBefore), value(idleAfter), value(busyBefore), value(busyAfter));
        expect(std::chrono::steady_clock::now() < deadline) << "CPU sampling exceeded the shared 30 s bound";
        expect(fatal(idleBefore && idleAfter && busyBefore && busyAfter));
        expect(*idleAfter - *idleBefore < 0.2) << *idleBefore << " -> " << *idleAfter;
        expect(*busyAfter - *busyBefore > 0.5) << *busyBefore << " -> " << *busyAfter;
        expect(!platform::cpu_seconds(0).has_value());
        // Fix plan F17.2: an incident says which thread of the engine is busy, where the platform can say.
        if (const auto pid = busy->native_pid()) {
            const auto threads = platform::thread_cpu(*pid);
            if (mcppls::os::family() == mcppls::os::Family::linux) {
                expect(fatal(!threads.empty()));
                const auto busiest = std::ranges::max_element(threads, {}, &platform::ThreadCpu::seconds);
                expect(busiest->seconds > 0.5 && busiest->id > 0 && !busiest->name.empty()) << busiest->name << " " << busiest->seconds;
            } else {
                expect(threads.empty()) << "only Linux can say, per thread";
            }
        }
        expect(platform::thread_cpu(0).empty());
    };

    // X-6 (plan 2026-10-03): the platform can name this process and tell incarnations apart.
    "process_self is the process asking, and process_identity answers for a live pid"_test = [&] {
        const auto self = platform::process_self();
        if (mcppls::os::family() == mcppls::os::Family::windows) {
            expect(self.has_value()) << "Windows names its own pid and start";
        } else {
            expect(fatal(self.has_value()));
        }
        if (self.has_value()) {
            expect(self->pid > 0);
            expect(!self->started.empty());
            // Asking again answers the same incarnation.
            const auto again = platform::process_identity(self->pid);
            expect(again.has_value() && again->started == self->started) << "stable while the process lives";
        }
        // A pid nothing answers for is gone (or cannot be judged), and never "the same process".
        const auto ghost = platform::process_identity(999999999);
        expect(!ghost.has_value() || ghost->pid != (self ? self->pid : 0));
        expect(platform::process_identity(0) == std::nullopt) << "pid 0 is not a question";
    };

    // X-6, the ux U10 defect: a killed-but-unreaped process (a zombie) keeps its /proc entry and
    // its start time. The identity must refuse it, or a lease whose owner died reads as "the same
    // live process" and a restart within the lease expiry starts cold in a private directory --
    // the ux budget caught it as 222 s where 25 was allowed.
    "a zombie has no identity, so its lease is taken over"_test = [&] {
        if (mcppls::os::family() == mcppls::os::Family::linux) {
            // A shell that starts a child, ignores SIGCHLD (so it never reaps) and becomes a sleep:
            // the child exits, stays a zombie for as long as this test runs, and names itself.
            platform::SpawnOptions options;
            options.program = "/bin/sh";
            options.arguments = { "-c", "trap '' CHLD; sleep 0.4 & echo $!; exec sleep 30" };
            auto shell = platform::Process::spawn(options);
            expect(fatal(shell.has_value()));
            const auto announced = shell->read_output_for(std::chrono::milliseconds { 2000 });
            std::int64_t zombie { 0 };
            if (announced.has_value() && announced->has_value()) zombie = std::stoll(announced->value());
            expect(fatal(zombie > 0)) << "the shell did not name its child";
            std::this_thread::sleep_for(std::chrono::milliseconds { 900 });   // the child has exited by now
            expect(platform::process_identity(zombie) == std::nullopt) << "a zombie has no identity to answer with";
            const auto alive = platform::process_alive(zombie);
            expect(!alive.has_value() || !*alive) << "a zombie is not running";
            shell->kill();
            (void)shell->wait();
        }
    };

    "process_alive answers on Windows too, and never says a live pid is gone"_test = [&] {
        if (const auto self = platform::process_self(); self && self->pid > 0) {
            const auto alive = platform::process_alive(self->pid);
            expect(alive.value_or(true)) << "this process is running, wherever the platform can say it or not";
        }
        const auto gone = platform::process_alive(999999999);
        expect(!gone.has_value() || !*gone) << "an unanswerable pid is gone or unknown, never alive";
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
        if (mcppls::os::family() != mcppls::os::Family::windows) {
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
