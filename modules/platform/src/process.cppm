// Child processes over openkal.process: start a program, talk to it through
// pipes, wait for it with or without a bound, and end it.
export module mcppls.platform.process;

import std;
import mcppls.base.error;

export namespace mcppls::platform {

struct SpawnOptions {
    std::string program;                                   // absolute path of the executable
    std::vector<std::string> arguments;                    // without argv[0]; argv[0] is `program`
    std::string workDirectory;                             // absolute; empty means the current directory
    std::optional<std::vector<std::string>> environment;   // "NAME=value"; nullopt inherits this process's
    bool pipeInput { true };
    bool pipeOutput { true };                              // false: the child writes to this process's stderr,
                                                           // because this process's stdout carries the protocol
    bool pipeError { false };                              // false: the child writes to this process's stderr
    bool ownUnit { false };                                // the child starts a unit that kill() ends with everything in it
    bool detached { false };                               // the child outlives this process (the workspace daemon)
    // Where the system can bind a child's lifetime to this process's, it is bound. False models what
    // a program started through popen or a shell is: in this process's unit, but not ending with it.
    // (macOS binds no lifetimes at all, so a child there always outlives its parent this way.)
    bool boundLifetime { true };
    // No standard streams given at all (pipe* are ignored). On Windows the child then inherits no handle
    // of this process: a start that places streams passes every inheritable handle along, this
    // process's own standard streams among them, and a detached child would hold them open.
    bool noStreams { false };
};

class Process {
public:
    Process();
    Process(Process&& other) noexcept;
    Process& operator=(Process&& other) noexcept;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    ~Process();                                            // terminates and reaps a child still running

public:
    static base::Result<Process> spawn(const SpawnOptions& options);

    bool valid() const;
    // The OS process id, best-effort: on the POSIX backends (Linux, macOS) openkal's process
    // handle IS the pid it waits on with wait4/waitpid, so a caller that needs to look at this
    // child from outside (a stress check's CPU/RSS sampler, for one) can. nullopt on Windows,
    // where the handle is not a pid, and whenever there is no process to ask about.
    std::optional<std::int64_t> native_pid() const;
    base::Result<void> write(std::string_view bytes);      // thread-safe
    // Blocks until bytes arrive. An empty string means the stream ended.
    base::Result<std::string> read_output();
    base::Result<std::string> read_error();
    // Bounded forms: nullopt when the bound expired with no bytes, an empty string when the
    // stream ended. An implementation that cannot bound a read of this resource blocks instead
    // and says so through `bounded`, so a caller knows the thread cannot be stopped by waiting.
    base::Result<std::optional<std::string>> read_output_for(std::chrono::milliseconds timeout, bool* bounded = nullptr);
    base::Result<std::optional<std::string>> read_error_for(std::chrono::milliseconds timeout, bool* bounded = nullptr);
    void close_input();
    base::Result<int> wait();
    // nullopt when the bound expired and the child is still running.
    base::Result<std::optional<int>> wait_for(std::chrono::milliseconds timeout);
    void terminate();
    // Lets the child go on without this process: its channels are closed, and nothing waits for it or ends it.
    void detach();
    // Ends the child without asking: a child that does not end when asked (clangd stuck building a
    // module does not) is killed, with its unit when it has one.
    void kill();
    // Ends everything else in the child's unit, whether or not the child itself is still running.
    // A tool that left something behind is exactly the case where the child is already gone and the
    // unit is not empty, so this does not check for that. Nothing happens without a unit.
    void end_unit();

private:
    struct State;
    std::unique_ptr<State> state_;
};

// What a run of an external program may cost. A tool the server starts to learn about a
// project must never be able to hold the server: every wait here has an end, including the
// one after the child is gone (a descendant that inherited the pipe keeps it open, which is
// how one hung `xlings update` kept a language server without a database for twelve minutes).
struct RunBounds {
    std::chrono::milliseconds hard { std::chrono::seconds { 60 } };   // ask the unit to end at this point
    std::chrono::milliseconds grace { std::chrono::seconds { 2 } };   // then end it without asking
    std::chrono::milliseconds drain { std::chrono::seconds { 1 } };   // read this long after the child is gone
    std::size_t outputLimit { std::size_t { 256 } * 1024 * 1024 };
    std::size_t errorLimit { std::size_t { 1 } * 1024 * 1024 };
};

struct RunResult {
    int exitCode { -1 };
    std::string output;
    std::string error;
    bool timedOut { false };
    // The child was gone and a stream was still open past the drain: something it started holds
    // the pipe. The unit was ended and the read abandoned; what had arrived by then is here.
    bool outputHeldOpen { false };
    bool outputTruncated { false };
    bool errorTruncated { false };
    std::chrono::milliseconds duration { 0 };
};

// Runs a program to completion with its standard output and error captured. `input` is written
// to its standard input, which is then closed; a program given nothing sees an empty input.
base::Result<RunResult> run(SpawnOptions options, RunBounds bounds, std::string_view input);
base::Result<RunResult> run(SpawnOptions options, RunBounds bounds);
base::Result<RunResult> run(SpawnOptions options, std::chrono::milliseconds timeout);

// The last lines of a stream, for a record that must stay small.
std::string last_lines(std::string_view text, std::size_t lines);

// The openkal preopened directory that contains an absolute path, and the path
// beneath it. Exposed for tests and for callers that need to explain a failure.
struct PreopenMatch {
    std::string preopenName;
    std::string remainder;
};
std::optional<PreopenMatch> match_preopen(std::string_view absolutePath);

} // namespace mcppls::platform
