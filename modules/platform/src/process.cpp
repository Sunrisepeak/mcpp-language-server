module mcppls.platform.process;

import std;
import openkal.types;
import openkal.fs;
import openkal.stream;
import openkal.process;
import openkal.timeout;
import mcppls.os;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.platform.preopen;

namespace mcppls::platform {

namespace {

// Bit positions from openkal/process.h. The C header states them as macros,
// which a module does not carry; the layout is frozen by the specification.
constexpr kal_uintptr SPAWN_BOUND_LIFETIME { kal_uintptr { 1 } << 0 };
constexpr kal_uintptr PROP_BOUND_LIFETIME { kal_uintptr { 1 } << 5 };
constexpr kal_uintptr PROP_JOB { kal_uintptr { 1 } << 6 };

std::string describe(int code) {
    switch (code) {
    case kal_err_invalid: return "invalid argument";
    case kal_err_again: return "would block";
    case kal_err_io: return "input/output error";
    case kal_err_no_memory: return "out of memory";
    case kal_err_permission: return "permission denied";
    case kal_err_not_supported: return "not supported";
    case kal_err_closed: return "closed";
    case kal_err_not_found: return "not found";
    default: return std::format("openkal error {}", code);
    }
}

} // namespace

std::optional<PreopenMatch> match_preopen(std::string_view absolutePath) {
    auto resolved = resolve_name(absolutePath);
    if (!resolved) return std::nullopt;
    return PreopenMatch { resolved->preopenName, resolved->remainder };
}

struct Process::State {
    kal_process handle {};
    kal_stream input {};
    kal_stream output {};
    kal_stream error {};
    bool inputOpen { false };
    bool outputOpen { false };
    bool errorOpen { false };
    bool exited { false };
    int exitCode { -1 };
    kal_job job {};
    bool hasJob { false };
    std::mutex writeMutex;
    std::mutex waitMutex;
};

Process::Process() = default;
Process::Process(Process&& other) noexcept = default;
Process& Process::operator=(Process&& other) noexcept = default;

Process::~Process() {
    if (!state_) return;
    close_input();
    if (!state_->exited) {
        kal_process_terminate(state_->handle);
        int status { 0 };
        int terminated { 0 };
        kal_process_wait(state_->handle, &status, &terminated);
    }
    if (state_->outputOpen) kal_process_channel_close(state_->output);
    if (state_->errorOpen) kal_process_channel_close(state_->error);
    kal_process_close(state_->handle);
}

bool Process::valid() const { return static_cast<bool>(state_); }

std::optional<std::int64_t> Process::native_pid() const {
    if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) return std::nullopt;
    if (!state_ || state_->exited) return std::nullopt;
    return static_cast<std::int64_t>(state_->handle.h);
}

base::Result<Process> Process::spawn(const SpawnOptions& options) {
    if (!base::is_absolute_path(options.program)) {
        return base::fail("spawn-program", std::format("program path must be absolute: {}", options.program));
    }
    const auto program = resolve_name(options.program);
    if (!program) {
        return base::fail("spawn-program", std::format("program is outside every preopened directory: {}", options.program));
    }
    const std::string workPath { options.workDirectory.empty() ? fs::current_directory() : options.workDirectory };
    const auto work = resolve_name(workPath);
    if (!work) {
        return base::fail("spawn-directory", std::format("work directory is outside every preopened directory: {}", workPath));
    }

    kal_dir workDir { work->directory };
    bool closeWorkDir { false };
    if (!work->remainder.empty()) {
        const int opened { kal_fs_open_dir(work->directory, work->remainder.data(), work->remainder.size(), &workDir) };
        if (opened != kal_ok) {
            return base::fail("spawn-directory", std::format("cannot open work directory {}: {}", workPath, describe(opened)));
        }
        closeWorkDir = true;
    }
    auto closeWork = [&] { if (closeWorkDir) kal_fs_close_dir(workDir); };

    auto state = std::make_unique<State>();
    kal_stream childIn {};
    kal_stream childOut {};
    kal_stream childErr {};
    // A channel is one-way: the first stream reads what the second writes.
    if (options.pipeInput) {
        if (kal_process_channel(&childIn, &state->input) != kal_ok) {
            closeWork();
            return base::fail("spawn-channel", "cannot create the input channel");
        }
        state->inputOpen = true;
    }
    if (options.pipeOutput) {
        if (kal_process_channel(&state->output, &childOut) != kal_ok) {
            closeWork();
            return base::fail("spawn-channel", "cannot create the output channel");
        }
        state->outputOpen = true;
    }
    if (options.pipeError) {
        if (kal_process_channel(&state->error, &childErr) != kal_ok) {
            closeWork();
            return base::fail("spawn-channel", "cannot create the error channel");
        }
        state->errorOpen = true;
    }

    std::vector<std::string> argvStorage;
    argvStorage.reserve(options.arguments.size() + 1);
    // The name the program observes as its own, in its system's spelling. A
    // Windows program that reads its own command line --- the command
    // interpreter, and every batch file through it --- takes each slash in a
    // forward-slashed name for a switch: `C:/Windows/System32/cmd.exe` ran
    // `md.exe`.
    std::string ownName { options.program };
    if constexpr (base::NATIVE_PATH_STYLE == base::PathStyle::windows) std::ranges::replace(ownName, '/', '\\');
    argvStorage.push_back(std::move(ownName));
    for (const auto& argument : options.arguments) argvStorage.push_back(argument);
    std::vector<const char*> argv;
    std::vector<kal_uintptr> argvLengths;
    for (const auto& argument : argvStorage) {
        argv.push_back(argument.data());
        argvLengths.push_back(argument.size());
    }

    // A null environment starts the child with an empty one, so inheritance is explicit.
    const std::vector<std::string> environmentStorage { options.environment ? *options.environment : env::variables() };
    std::vector<const char*> envp;
    std::vector<kal_uintptr> envpLengths;
    for (const auto& variable : environmentStorage) {
        envp.push_back(variable.data());
        envpLengths.push_back(variable.size());
    }

    const kal_uintptr flags { !options.detached && options.boundLifetime && (kal_process_props() & PROP_BOUND_LIFETIME) != 0
                                  ? SPAWN_BOUND_LIFETIME : kal_uintptr { 0 } };
    kal_spawn how { program->directory, workDir, nullptr, nullptr, 0, flags };
    kal_job job {};
    // A detached child leads a unit of its own too, so what ends this program's unit does not end it.
    const bool unit { (options.ownUnit || options.detached) && (kal_process_props() & PROP_JOB) != 0 };
    if (unit) how.job = &job;
    // A zero stream inherits the parent's; kal_stdin() is zero on openkal-linux, which is the same act.
    const kal_spawn_streams streams {
        options.noStreams ? kal_stream {} : options.pipeInput ? childIn : kal_stream {},
        options.noStreams ? kal_stream {} : options.pipeOutput ? childOut : kal_stderr(),
        options.noStreams ? kal_stream {} : options.pipeError ? childErr : kal_stderr(),
    };
    const int result { kal_process_spawn(&how, program->remainder.data(), program->remainder.size(),
                                         argv.data(), argvLengths.data(), argv.size(),
                                         envp.data(), envpLengths.data(), envp.size(),
                                         &streams, &state->handle) };
    if (options.pipeInput) kal_process_channel_close(childIn);
    if (options.pipeOutput) kal_process_channel_close(childOut);
    if (options.pipeError) kal_process_channel_close(childErr);
    closeWork();
    if (result != kal_ok) {
        if (state->inputOpen) kal_process_channel_close(state->input);
        if (state->outputOpen) kal_process_channel_close(state->output);
        if (state->errorOpen) kal_process_channel_close(state->error);
        return base::fail("spawn-failed", std::format("cannot start {}: {}", options.program, describe(result)));
    }

    if (unit) {
        state->job = job;
        state->hasJob = true;
    }
    Process process;
    process.state_ = std::move(state);
    return process;
}

base::Result<void> Process::write(std::string_view bytes) {
    if (!state_ || !state_->inputOpen) return base::fail("process-write", "input is not open");
    std::lock_guard lock { state_->writeMutex };
    std::size_t done { 0 };
    while (done < bytes.size()) {
        const kal_intptr written { kal_stream_write(state_->input, bytes.data() + done, bytes.size() - done) };
        if (written <= 0) {
            return base::fail("process-write", std::format("write failed: {}", describe(static_cast<int>(-written))));
        }
        done += static_cast<std::size_t>(written);
    }
    return {};
}

namespace {

base::Result<std::string> read_stream(kal_stream stream, bool open) {
    if (!open) return std::string {};
    std::array<char, 16384> buffer {};
    const kal_intptr got { kal_stream_read(stream, buffer.data(), buffer.size()) };
    if (got < 0) {
        if (static_cast<int>(-got) == kal_err_closed) return std::string {};
        return base::fail("process-read", std::format("read failed: {}", describe(static_cast<int>(-got))));
    }
    return std::string { buffer.data(), static_cast<std::size_t>(got) };
}

// A read that gives up. `bounded` reports whether this resource could be bounded at all: where
// it could not, the read below it blocked, and a caller waiting for this thread to notice a
// stop flag would wait as long as the stream stays open.
base::Result<std::optional<std::string>> read_stream_for(kal_stream stream, bool open,
                                                         std::chrono::milliseconds timeout, bool* bounded) {
    if (bounded) *bounded = true;
    if (!open) return std::optional<std::string> { std::string {} };
    std::array<char, 16384> buffer {};
    const auto nanoseconds = static_cast<kal_u64>(
        std::max<std::int64_t>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count()));
    kal_intptr got { kal_timeout_read(stream, buffer.data(), buffer.size(), nanoseconds) };
    if (got < 0) {
        const int code { static_cast<int>(-got) };
        if (code == kal_err_again) return std::optional<std::string> {};
        if (code == kal_err_closed) return std::optional<std::string> { std::string {} };
        if (code != kal_err_not_supported) {
            return base::fail("process-read", std::format("read failed: {}", describe(code)));
        }
        // This implementation cannot bound a read of this resource. Reading unbounded is the
        // only remaining way to make progress; the caller is told so it can abandon the thread
        // instead of waiting for it.
        if (bounded) *bounded = false;
        return read_stream(stream, open).transform([](std::string text) { return std::optional<std::string> { std::move(text) }; });
    }
    return std::optional<std::string> { std::string { buffer.data(), static_cast<std::size_t>(got) } };
}

} // namespace

base::Result<std::string> Process::read_output() {
    if (!state_) return std::string {};
    return read_stream(state_->output, state_->outputOpen);
}

base::Result<std::string> Process::read_error() {
    if (!state_) return std::string {};
    return read_stream(state_->error, state_->errorOpen);
}

base::Result<std::optional<std::string>> Process::read_output_for(std::chrono::milliseconds timeout, bool* bounded) {
    if (!state_) return std::optional<std::string> { std::string {} };
    return read_stream_for(state_->output, state_->outputOpen, timeout, bounded);
}

base::Result<std::optional<std::string>> Process::read_error_for(std::chrono::milliseconds timeout, bool* bounded) {
    if (!state_) return std::optional<std::string> { std::string {} };
    return read_stream_for(state_->error, state_->errorOpen, timeout, bounded);
}

void Process::close_input() {
    if (!state_) return;
    std::lock_guard lock { state_->writeMutex };
    if (state_->inputOpen) {
        kal_process_channel_close(state_->input);
        state_->inputOpen = false;
    }
}

base::Result<int> Process::wait() {
    if (!state_) return base::fail("process-wait", "no process");
    std::lock_guard lock { state_->waitMutex };
    if (state_->exited) return state_->exitCode;
    int status { 0 };
    int terminated { 0 };
    const int result { kal_process_wait(state_->handle, &status, &terminated) };
    if (result != kal_ok) return base::fail("process-wait", describe(result));
    state_->exited = true;
    state_->exitCode = terminated != 0 ? -1 : status;
    return state_->exitCode;
}

base::Result<std::optional<int>> Process::wait_for(std::chrono::milliseconds timeout) {
    if (!state_) return base::fail("process-wait", "no process");
    std::lock_guard lock { state_->waitMutex };
    if (state_->exited) return std::optional<int> { state_->exitCode };
    int status { 0 };
    int terminated { 0 };
    const auto nanoseconds = std::max<std::int64_t>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count());
    const int result { kal_timeout_wait_process(state_->handle, static_cast<kal_u64>(nanoseconds), &status, &terminated) };
    if (result == kal_err_again) return std::optional<int> {};
    if (result != kal_ok) return base::fail("process-wait", describe(result));
    state_->exited = true;
    state_->exitCode = terminated != 0 ? -1 : status;
    return std::optional<int> { state_->exitCode };
}

void Process::terminate() {
    if (!state_) return;
    std::lock_guard lock { state_->waitMutex };
    if (!state_->exited) kal_process_terminate(state_->handle);
}

void Process::detach() {
    if (!state_) return;
    close_input();
    if (state_->outputOpen) kal_process_channel_close(state_->output);
    if (state_->errorOpen) kal_process_channel_close(state_->error);
    kal_process_close(state_->handle);
    state_.reset();
}

void Process::kill() {
    if (!state_) return;
    std::lock_guard lock { state_->waitMutex };
    if (state_->exited) return;
    if (!state_->hasJob || kal_process_job_terminate(state_->job) != kal_ok) kal_process_terminate(state_->handle);
}

void Process::end_unit() {
    if (!state_ || !state_->hasJob) return;
    std::lock_guard lock { state_->waitMutex };
    kal_process_job_terminate(state_->job);
}

namespace {

// One stream being collected. The readers are detached: where a descendant holds a pipe open,
// nothing can make the read return, so the thread is left behind with a share of the process
// and of this buffer rather than joined.
struct Capture {
    std::mutex mutex;
    std::string data;
    std::size_t limit { 0 };
    bool truncated { false };
    bool ended { false };
};

struct Collection {
    std::mutex mutex;
    std::condition_variable changed;
    int open { 0 };
    std::atomic<bool> stop { false };
};

constexpr std::chrono::milliseconds READ_SLICE { 50 };

void collect(std::shared_ptr<Process> process, std::shared_ptr<Capture> capture,
             std::shared_ptr<Collection> collection, bool isOutput) {
    while (!collection->stop.load(std::memory_order_relaxed)) {
        bool bounded { true };
        auto chunk = isOutput ? process->read_output_for(READ_SLICE, &bounded)
                              : process->read_error_for(READ_SLICE, &bounded);
        if (!chunk) break;                       // the stream failed: nothing more will arrive
        if (!chunk->has_value()) continue;       // the slice expired, look at the stop flag again
        if ((*chunk)->empty()) break;            // end of stream
        std::lock_guard lock { capture->mutex };
        if (capture->data.size() >= capture->limit) {
            capture->truncated = true;           // keep reading so the child is never blocked on a full pipe
            continue;
        }
        const std::size_t room { capture->limit - capture->data.size() };
        if ((*chunk)->size() > room) {
            capture->data.append((*chunk)->data(), room);
            capture->truncated = true;
        } else {
            capture->data.append(**chunk);
        }
        (void) bounded;
    }
    {
        std::lock_guard lock { capture->mutex };
        capture->ended = true;
    }
    {
        std::lock_guard lock { collection->mutex };
        --collection->open;
    }
    collection->changed.notify_all();
}

} // namespace

base::Result<RunResult> run(SpawnOptions options, RunBounds bounds, std::string_view input) {
    options.pipeInput = true;
    options.pipeOutput = true;
    options.pipeError = true;
    // The bound ends what this program started, not just the program: a tool that leaves a
    // child behind holding the pipe is exactly the failure this runner exists for.
    options.ownUnit = true;

    const auto started = std::chrono::steady_clock::now();
    auto spawned = Process::spawn(options);
    if (!spawned) return std::unexpected { spawned.error() };
    auto process = std::make_shared<Process>(std::move(*spawned));
    if (!input.empty()) (void) process->write(input);
    process->close_input();

    auto output = std::make_shared<Capture>();
    output->limit = bounds.outputLimit;
    auto error = std::make_shared<Capture>();
    error->limit = bounds.errorLimit;
    auto collection = std::make_shared<Collection>();
    collection->open = 2;
    std::thread { collect, process, output, collection, true }.detach();
    std::thread { collect, process, error, collection, false }.detach();

    RunResult result;
    auto waited = process->wait_for(bounds.hard);
    if (!waited) return std::unexpected { waited.error() };
    if (!waited->has_value()) {
        result.timedOut = true;
        process->terminate();                                  // ask
        (void) process->wait_for(bounds.grace);
        // Then end the unit, whether or not the child itself went when asked. What the bound is
        // there for is everything the tool started, and on a system with no lifetime binding
        // (macOS) those outlive a child that left politely.
        process->end_unit();
        auto code = process->wait();
        result.exitCode = code.value_or(-1);
    } else {
        result.exitCode = **waited;
    }

    // The child is gone. Anything still writing to these pipes is something it started.
    {
        std::unique_lock lock { collection->mutex };
        collection->changed.wait_for(lock, bounds.drain, [&] { return collection->open == 0; });
        if (collection->open != 0) result.outputHeldOpen = true;
    }
    if (result.outputHeldOpen) {
        process->end_unit();                                   // the child is gone; its unit is not
        collection->stop.store(true, std::memory_order_relaxed);
        // One more slice for what the kill released, then the readers are left to themselves:
        // they hold a share of the process, so the channels outlive this call if they must.
        std::unique_lock lock { collection->mutex };
        collection->changed.wait_for(lock, READ_SLICE * 4, [&] { return collection->open == 0; });
    }

    {
        std::lock_guard lock { output->mutex };
        result.output = output->data;
        result.outputTruncated = output->truncated;
    }
    {
        std::lock_guard lock { error->mutex };
        result.error = error->data;
        result.errorTruncated = error->truncated;
    }
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    return result;
}

base::Result<RunResult> run(SpawnOptions options, RunBounds bounds) {
    return run(std::move(options), bounds, std::string_view {});
}

base::Result<RunResult> run(SpawnOptions options, std::chrono::milliseconds timeout) {
    return run(std::move(options), RunBounds { .hard = timeout }, std::string_view {});
}

std::string last_lines(std::string_view text, std::size_t lines) {
    if (lines == 0 || text.empty()) return std::string {};
    std::size_t end { text.size() };
    while (end > 0 && (text[end - 1] == '\n' || text[end - 1] == '\r')) --end;
    std::size_t start { end };
    std::size_t seen { 0 };
    while (start > 0) {
        if (text[start - 1] == '\n') {
            ++seen;
            if (seen == lines) break;
        }
        --start;
    }
    return std::string { text.substr(start, end - start) };
}

} // namespace mcppls::platform
