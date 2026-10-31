// How mcppls starts, speaks to and bounds programs, over mcpplibs.os `process`: the LSP connection's
// writer and reader, the bounded runs a tool is given, the identity a lease records, and the
// behaviours a build tool depends on (arguments, environment values, the work directory). The test
// program starts copies of itself as the child, so the same test runs on every system without
// depending on a shell or a system utility. The one exception is on Windows, where the command
// interpreter is itself the subject: build tools there are often batch files, and those run through it.
import std;
import mcpplibs.os;
import mcppls.testing;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.lsp.connection;
import mcppls.orchestrator.instance;
import mcppls.toolrun;
import nlohmann.json;

namespace os = mcpplibs::os;
namespace base = mcppls::base;

namespace {

// An independent work oracle: the C library's processor clock (`std::clock`, which openkal-musl
// answers from this program's own kal_program_info since 0.22), not mcpplibs.os `process::describe`,
// which asks about another program by its identifier.
std::optional<double> native_self_cpu_seconds() {
    const std::clock_t now { std::clock() };
    if (now == static_cast<std::clock_t>(-1)) return std::nullopt;
    return static_cast<double>(now) / 1'000'000.0;   // CLOCKS_PER_SEC, which POSIX fixes at a million
}

std::string self_path() {
    const auto arguments = os::env::arguments();
    std::string path { arguments.empty() ? std::string {} : arguments.front() };
    if (!base::is_absolute_path(path)) path = base::join_path(base::current_directory(), path);
    return base::normalize_path(path);
}

// This program again, with its input and output on pipes and its error output on this program's.
os::process::spawn_options child(std::vector<std::string> arguments) {
    return { .program = self_path(), .args = std::move(arguments), .in = os::process::stdio::pipe,
             .out = os::process::stdio::pipe, .err = os::process::stdio::inherit, .bound_lifetime = true };
}

int child_main(std::span<const std::string> arguments) {
    const std::string_view mode { arguments[1] };
    if (mode == "--echo") {
        while (true) {
            auto chunk = os::stdio::read_input();
            if (!chunk || chunk->empty()) break;
            (void)os::stdio::write_output(*chunk);
        }
        return 0;
    }
    if (mode == "--exit") return std::stoi(arguments[2]);
    if (mode == "--sleep") {
        std::this_thread::sleep_for(std::chrono::seconds { 30 });
        return 0;
    }
    if (mode == "--cpu-idle") {
        (void)os::stdio::write_output("ready\n");
        (void)os::stdio::read_input();   // stay alive and idle until the parent ends the test
        return 0;
    }
    if (mode == "--spin-cpu") {
        (void)os::stdio::write_output("ready\n");
        std::string start;
        while (!start.ends_with('\n') && start.size() < 64) {
            const auto part = os::stdio::read_input();
            if (!part || part->empty()) return 101;
            start += *part;
        }
        if (start != "start\n") return 101;
        const auto before = native_self_cpu_seconds();
        if (!before) {
            (void)os::stdio::write_output("cpu-oracle-unavailable\n");
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
                (void)os::stdio::write_output("cpu-oracle-invalid\n");
                return 103;
            }
            completedCpu = *cpu - *before;
            if (completedCpu >= 0.75) { completed = true; break; }
        }
        (void)os::stdio::write_output(completed ? std::format("done {:.9f}\n", completedCpu) : "cpu-work-timeout\n");
        (void)os::stdio::read_input();   // keep the completed process available to cpu_seconds
        return completed ? 0 : 104;
    }
    if (mode == "--env") {
        (void)os::stdio::write_output(os::env::get(arguments[2]).value_or("<unset>"));
        return 0;
    }
    if (mode == "--read-relative") {
        std::ifstream stream { arguments[2] };
        std::string line;
        std::getline(stream, line);
        (void)os::stdio::write_output(stream || !line.empty() ? line : std::string { "<missing>" });
        return 0;
    }
    if (mode == "--arguments") {
        std::string joined;
        for (std::size_t i { 2 }; i < arguments.size(); ++i) joined += std::format("[{}]", arguments[i]);
        (void)os::stdio::write_output(joined);
        return 0;
    }
    if (mode == "--stderr") {
        (void)os::stdio::write_error("to-stderr");
        return 3;
    }
    // Sleeps while holding whatever streams it was given, then leaves a mark. The mark is how a
    // test tells "the unit was ended" from "the sleep finished".
    if (mode == "--descendant") {
        std::this_thread::sleep_for(std::chrono::seconds { 3 });
        (void)os::fs::write(base::join_path(arguments[2], "late.txt"), "late");
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
        // Its output goes to this program's error output (the caller's pipe). Not bound to this program's
        // lifetime, which is what a build tool's own children are: only ending the unit reaches them.
        auto descendant = os::process::process::spawn({ .program = self_path(), .args = { "--descendant", arguments[2] },
                                                        .in = os::process::stdio::null, .out = os::process::stdio::error,
                                                        .err = os::process::stdio::inherit, .own_unit = ownUnit });
        if (descendant) descendant->detach();
        (void)os::stdio::write_output(descendant ? "started" : std::format("spawn-failed:{}", descendant.error().message()));
        if (!ownUnit) std::this_thread::sleep_for(std::chrono::seconds { 30 });
        return 0;
    }
    return 100;
}

} // namespace

int main() {
    const auto arguments = os::env::arguments();
    // A child leaves without running the test framework's report at exit.
    if (arguments.size() >= 2 && arguments[1].starts_with("--")) std::_Exit(child_main(arguments));

    using namespace mcppls::testing;
    const std::string self { self_path() };

    "self path is an existing absolute file"_test = [&] {
        expect(base::is_absolute_path(self)) << self;
        std::error_code ignored;
        expect(std::filesystem::is_regular_file(self, ignored)) << self;
    };

    "echo round trip through pipes"_test = [&] {
        auto process = os::process::process::spawn(child({ "--echo" }));
        expect(fatal(process.has_value())) << (process ? "" : process.error().message());
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
        expect(status.has_value() && status->success());
        expect(collected == "hello\nfrom the parent\n") << collected;
    };

    // Each argument arrives exactly as given, whatever the system does to pass a
    // vector through one command line.
    "arguments arrive unaltered"_test = [&] {
        const std::vector<std::string> given { "plain", "two words", "", "C:\\dir\\file.txt", "trailing\\",
                                               "quote\"inside", "back\\\"quote", "\\\\server\\share", "tab\there", "--flag=a b" };
        std::vector<std::string> arguments { "--arguments" };
        arguments.insert(arguments.end(), given.begin(), given.end());
        auto result = os::process::run(child(arguments), os::process::bounds { .hard = std::chrono::seconds { 60 } });
        expect(fatal(result.has_value()));
        std::string expected;
        for (const auto& argument : given) expected += std::format("[{}]", argument);
        expect(result->out == expected) << result->out;
    };

    // The stress check's process-tree sampler (mcppls-conformance) reads this to find the server
    // it started on POSIX, where openkal's process handle is the OS pid it waits on with wait4.
    // Plan 2026-09-30 §13: clangd reading slowly on a busy Windows machine held every send, and the server's event
    // loop with it, 11 to 13 s an edit. What is sent to a peer is queued for a writer thread of the connection's own.
    "a peer that reads nothing does not hold what is sent to it, and stopping it still ends"_test = [&] {
        using namespace std::chrono_literals;
        auto connection = mcppls::lsp::Connection::start(child({ "--sleep" }), [](nlohmann::json) {}, [] {});
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
                child({ "--echo" }),
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
            child({ "--exit", "0" }), [](nlohmann::json) {},
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
        auto process = os::process::process::spawn(child({ "--exit", "0" }));
        expect(fatal(process.has_value()));
        const auto status = process->wait();
        expect(fatal(status.has_value()));
        expect(status->success());
        const auto written = process->write("peer is gone");
        expect(!written.has_value());
        if (!written) expect(written.error().code == os::errc::closed) << written.error().message();
    };

    "the identifier is the child's while it runs, on every system"_test = [&] {
        auto process = os::process::process::spawn(child({ "--sleep" }));
        expect(fatal(process.has_value())) << (process ? "" : process.error().message());
        const auto pid { process->id() };
        expect(pid > 0) << pid;
        const auto described = os::process::describe(pid);
        expect(described.has_value() && !described->ended) << pid;
        if (os::host() == os::platform::linux) {
            std::error_code ignored;
            expect(std::filesystem::is_directory(std::format("/proc/{}", pid), ignored)) << pid;
        }
        process->kill();
        (void)process->wait();
    };

    // StuckWatch in the clangd engine tells a stuck clangd from a busy one by this.
    "cpu_seconds tells a busy child from an idle one, on every system"_test = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds { 30 };
        auto idle = os::process::process::spawn(child({ "--cpu-idle" }));
        auto busy = os::process::process::spawn(child({ "--spin-cpu" }));
        expect(fatal(idle.has_value() && busy.has_value()));
        struct Cleanup {
            os::process::process& idle;
            os::process::process& busy;
            ~Cleanup() { idle.kill(); busy.kill(); (void)idle.wait(); (void)busy.wait(); }
        } cleanup { *idle, *busy };
        const auto announced = [&](os::process::process& process, std::string_view expected, std::string* message = nullptr) {
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
        // What the clangd engine's StuckWatch reads (ClangdProcess::cpu_reader).
        const auto read = [](const os::process::process& process) -> std::optional<double> {
            const auto described = os::process::describe(process.id());
            if (!described || described->ended) return std::nullopt;
            return static_cast<double>(described->cpu_ns) / 1e9;
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
        expect(!os::process::describe(0).has_value());
        // Fix plan F17.2: an incident says which thread of the engine is busy, where the system can say.
        {
            const auto threads = os::process::threads(busy->id());
            if (os::host() == os::platform::linux) {
                expect(fatal(!threads.empty()));
                const auto busiest = std::ranges::max_element(threads, {}, &os::process::thread_info::cpu_ns);
                expect(busiest->cpu_ns > 500'000'000 && busiest->id > 0 && !busiest->name.empty()) << busiest->name << " " << busiest->cpu_ns;
            } else {
                expect(threads.empty()) << "only Linux can say, per thread";
            }
        }
        expect(os::process::threads(0).empty());
    };

    // X-6 (plan 2026-10-03): the system names this process and tells incarnations apart; a lease records both.
    "this program is named, and asking again answers the same incarnation"_test = [&] {
        const auto me = os::process::self();
        expect(fatal(me.has_value())) << "every system names this program and its start";
        expect(me->id > 0);
        expect(me->started_ns > 0);
        const auto again = os::process::describe(me->id);
        expect(again.has_value() && again->who == *me) << "stable while the process lives";
        // An identifier nothing answers for is gone (or cannot be judged), and never "the same process".
        const auto ghost = os::process::describe(999999999);
        expect(!ghost.has_value() || ghost->who.id != me->id);
        expect(!os::process::describe(0).has_value()) << "identifier 0 is not a question";
        // The lease's own reading of it (instance.cpp): this process is not gone, another incarnation is.
        const std::string started { std::format("ok:{}", me->started_ns) };
        expect(!mcppls::orchestrator::lease_owner_gone(static_cast<std::int64_t>(me->id), started));
        expect(mcppls::orchestrator::lease_owner_gone(static_cast<std::int64_t>(me->id), started + "!"));
    };

    // X-6, the ux U10 defect: a killed-but-unreaped process (a zombie) keeps its /proc entry and
    // its start time. The identity must refuse it, or a lease whose owner died reads as "the same
    // live process" and a restart within the lease expiry starts cold in a private directory --
    // the ux budget caught it as 222 s where 25 was allowed.
    "a zombie has no identity, so its lease is taken over"_test = [&] {
        if (os::host() == os::platform::linux) {
            // A shell that starts a child, ignores SIGCHLD (so it never reaps) and becomes a sleep:
            // the child exits, stays a zombie for as long as this test runs, and names itself.
            auto shell = os::process::process::spawn({ .program = "/bin/sh", .args = { "-c", "trap '' CHLD; sleep 0.4 & echo $!; exec sleep 30" },
                                                       .in = os::process::stdio::null, .out = os::process::stdio::pipe,
                                                       .err = os::process::stdio::inherit, .bound_lifetime = true });
            expect(fatal(shell.has_value()));
            const auto announced = shell->read_output_for(std::chrono::milliseconds { 2000 });
            std::int64_t zombie { 0 };
            if (announced.has_value() && announced->has_value()) zombie = std::stoll(announced->value());
            expect(fatal(zombie > 0)) << "the shell did not name its child";
            std::this_thread::sleep_for(std::chrono::milliseconds { 900 });   // the child has exited by now
            const auto described = os::process::describe(static_cast<std::uint64_t>(zombie));
            expect(!described.has_value() || described->ended) << "a zombie is not running";
            expect(mcppls::orchestrator::lease_owner_gone(zombie, "ok:1")) << "and a lease it holds is taken over";
            shell->kill();
            (void)shell->wait();
        }
    };

    "a live program is never said to be gone, on Windows too"_test = [&] {
        if (const auto me = os::process::self()) {
            const auto described = os::process::describe(me->id);
            expect(!described || !described->ended) << "this process is running, wherever the system can say it or not";
        }
        const auto gone = os::process::describe(999999999);
        expect(!gone.has_value() || gone->ended) << "an unanswerable identifier is gone or unknown, never alive";
    };

    "exit status is propagated"_test = [&] {
        auto result = os::process::run(child({ "--exit", "7" }), os::process::bounds { .hard = std::chrono::seconds { 60 } });
        expect(fatal(result.has_value()));
        expect(result->status.code == 7 && !result->status.by_environment) << result->status.code;
        expect(!result->timed_out);
    };

    "standard error is captured separately"_test = [&] {
        auto result = os::process::run(child({ "--stderr" }), os::process::bounds { .hard = std::chrono::seconds { 60 } });
        expect(fatal(result.has_value()));
        expect(result->status.code == 3);
        expect(result->err == "to-stderr") << result->err;
        expect(result->out.empty());
    };

    "a bound on waiting ends a sleeping child"_test = [&] {
        const auto started = std::chrono::steady_clock::now();
        auto result = os::process::run(child({ "--sleep" }), os::process::bounds { .hard = std::chrono::milliseconds { 500 } });
        const auto elapsed = std::chrono::steady_clock::now() - started;
        expect(fatal(result.has_value()));
        expect(result->timed_out);
        expect(elapsed < std::chrono::seconds { 20 });
    };

    "environment reaches the child"_test = [&] {
        auto environment = os::env::variables();
        environment.push_back("MCPPLS_TEST_VARIABLE=from-parent");
        auto options = child({ "--env", "MCPPLS_TEST_VARIABLE" });
        options.clear_env = true;
        options.env = os::env::assignments(environment);
        auto result = os::process::run(std::move(options), os::process::bounds { .hard = std::chrono::seconds { 60 } });
        expect(fatal(result.has_value()));
        expect(result->out == "from-parent") << result->out;
    };

    // The project a tool runs for is its working directory (mcpp docs/50): a variable an editor
    // inherited from another project's terminal, which a version manager's shim would read when
    // the directory has no pin of its own, does not reach it.
    "a tool run does not carry another project's XLINGS_PROJECT_DIR"_test = [&] {
        auto environment = os::env::variables();
        environment.push_back("XLINGS_PROJECT_DIR=/some/other/project");
        environment.push_back("MCPPLS_TEST_VARIABLE=kept");
        const auto args = child({ "--env", "XLINGS_PROJECT_DIR" });
        mcppls::toolrun::Request request;
        request.program = args.program.string();
        request.arguments = args.args;
        request.workDirectory = base::current_directory();
        request.purpose = "test";
        request.environment = environment;
        request.bounds = os::process::bounds { .hard = std::chrono::seconds { 60 } };
        const auto result = mcppls::toolrun::run(request);
        expect(fatal(result.has_value()));
        expect(result->out == "<unset>") << result->out;
        request.arguments = { "--env", "MCPPLS_TEST_VARIABLE" };
        const auto kept = mcppls::toolrun::run(request);
        expect(fatal(kept.has_value()));
        expect(kept->out == "kept") << kept->out;
    };

    // A value is reported as it was set, backslashes included: a program that
    // starts the command interpreter from %ComSpec% received a forward-slashed
    // name, which the interpreter reads as switches.
    "an environment value arrives unaltered"_test = [&] {
        auto environment = os::env::variables();
        environment.push_back("MCPPLS_TEST_PATHS=C:\\dir\\sub;\\\\host\\share\\x;a\"b");
        auto options = child({ "--env", "MCPPLS_TEST_PATHS" });
        options.clear_env = true;
        options.env = os::env::assignments(environment);
        auto result = os::process::run(std::move(options), os::process::bounds { .hard = std::chrono::seconds { 60 } });
        expect(fatal(result.has_value()));
        expect(result->out == "C:\\dir\\sub;\\\\host\\share\\x;a\"b") << result->out;
    };

    "work directory is where relative names resolve"_test = [&] {
        const std::string directory { base::join_path(base::spelled(os::dirs::temp()),
            std::format("mcppls-test-process-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        expect(fatal(std::filesystem::create_directories(directory) || std::filesystem::is_directory(directory))) << directory;
        expect(os::fs::write(base::join_path(directory, "marker.txt"), "marker-content\n").has_value());
        auto options = child({ "--read-relative", "marker.txt" });
        options.cwd = directory;
        auto result = os::process::run(std::move(options), os::process::bounds { .hard = std::chrono::seconds { 60 } });
        expect(fatal(result.has_value()));
        expect(result->out == "marker-content") << result->out;
        std::filesystem::remove_all(directory);
    };

    // The command interpreter refuses a current directory given in the `\\?\` form
    // ("UNC paths are not supported") and runs in the Windows directory instead,
    // so a batch file started in a project ran somewhere else.
    "the command interpreter runs in the work directory"_test = [&] {
        if (os::host() != os::platform::windows) {
            return;
        } else {
            const std::string directory { base::join_path(base::spelled(os::dirs::temp()),
                std::format("mcppls-test-cmd-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
            expect(fatal(std::filesystem::create_directories(directory) || std::filesystem::is_directory(directory))) << directory;
            expect(os::fs::write(base::join_path(directory, "marker.txt"), "marker-content").has_value());
            const std::string interpreter { base::normalize_path(os::env::get("ComSpec").value_or("C:/Windows/System32/cmd.exe")) };
            auto result = os::process::run({ .program = interpreter, .args = { "/d", "/c", "type", "marker.txt" }, .cwd = directory,
                                             .bound_lifetime = true },
                                           os::process::bounds { .hard = std::chrono::seconds { 60 } });
            expect(fatal(result.has_value()));
            expect(base::trim(result->out) == "marker-content") << result->out << result->err;
            expect(!result->err.contains("UNC")) << result->err;
            std::filesystem::remove_all(directory);
        }
    };

    // The failure this runner exists for: a program leaves behind something that inherited the
    // caller's pipe. Reading to the end of the stream then waits for the descendant, which is how
    // one hung index refresh kept a language server without a build database for twelve minutes.
    "a descendant holding a pipe does not hold the caller, and the unit is ended"_test = [&] {
        const std::string directory { base::join_path(base::spelled(os::dirs::temp()),
            std::format("mcppls-test-hold-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        expect(fatal(std::filesystem::create_directories(directory) || std::filesystem::is_directory(directory))) << directory;
        const auto started = std::chrono::steady_clock::now();
        auto result = os::process::run(child({ "--hold-pipe", directory }),
                                       os::process::bounds { .hard = std::chrono::seconds { 30 }, .drain = std::chrono::seconds { 1 } });
        const auto elapsed = std::chrono::steady_clock::now() - started;
        expect(fatal(result.has_value()));
        expect(result->status.success()) << result->status.code;
        expect(!result->timed_out);
        expect(result->held_open) << "the descendant held the pipe and should have been reported";
        expect(elapsed < std::chrono::seconds { 3 }) << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        expect(result->out == "started") << result->out;
        // `held_open` is itself the proof that the descendant was alive and holding the pipe
        // through the drain: had it gone with its parent, the stream would have ended and there
        // would have been nothing to report. What happens to it afterwards is the system's own
        // business --- a unit of its own outlives the caller on POSIX and is collected with its
        // job on Windows --- and is not what this test is about.
        std::this_thread::sleep_for(std::chrono::seconds { 4 });
        std::filesystem::remove_all(directory);
    };

    // The child leaves politely when asked; what it started does not end with it (no lifetime
    // binding, as on macOS and as for anything a build tool starts through a shell). Only ending
    // the unit reaches that, and the bound must do it whether or not the child went quietly.
    "the hard bound ends what the tool started, not only the tool"_test = [&] {
        const std::string directory { base::join_path(base::spelled(os::dirs::temp()),
            std::format("mcppls-test-unit-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        expect(fatal(std::filesystem::create_directories(directory) || std::filesystem::is_directory(directory))) << directory;
        const auto started = std::chrono::steady_clock::now();
        auto result = os::process::run(child({ "--hold-pipe-and-sleep", directory }),
                                       os::process::bounds { .hard = std::chrono::milliseconds { 300 },
                                                             .grace = std::chrono::milliseconds { 200 },
                                                             .drain = std::chrono::milliseconds { 500 } });
        const auto elapsed = std::chrono::steady_clock::now() - started;
        expect(fatal(result.has_value()));
        expect(result->timed_out);
        expect(elapsed < std::chrono::seconds { 5 }) << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        std::this_thread::sleep_for(std::chrono::seconds { 4 });
        std::error_code ignored;
        expect(!std::filesystem::exists(base::join_path(directory, "late.txt"), ignored));
        std::filesystem::remove_all(directory);
    };

    "output above the limit is truncated and said to be"_test = [&] {
        auto result = os::process::run(child({ "--arguments", std::string(4096, 'x') }),
                                       os::process::bounds { .hard = std::chrono::seconds { 60 }, .out_limit = 512 });
        expect(fatal(result.has_value()));
        expect(result->out_truncated);
        expect(result->out.size() == 512) << result->out.size();
    };

    "the last lines of a stream are what a record keeps"_test = [] {
        expect(base::last_lines("a\nb\nc\n", 2) == "b\nc") << base::last_lines("a\nb\nc\n", 2);
        expect(base::last_lines("only", 3) == "only");
        expect(base::last_lines("", 3).empty());
    };

    "a program that is not there is not started"_test = [] {
        auto process = os::process::process::spawn({ .program = "relative/program" });
        expect(!process.has_value());
    };

    "this program's own name lies beneath a directory it was given"_test = [&] {
        expect(os::fs::locate(std::filesystem::path { self }).has_value()) << self;
    };

    return report();
}
