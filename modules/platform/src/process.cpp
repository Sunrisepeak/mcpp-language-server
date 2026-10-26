module mcppls.platform.process;

import std;
import mcpplibs.os;
import mcppls.os;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.fs;

// A child is a mcpplibs.os.process: the argument vector arrives unaltered, the child receives
// the three streams and nothing else, a unit ends with everything its members started, and a
// bound lifetime ends a child with this program. What is here is mcppls' own shape of it: the
// streams a language server's child may have (its output may not reach standard output, which
// carries the protocol), the exit code as one integer, and the bounded run.
//
// What is no longer here, and why. The Darwin pipe protection (WA-PLATFORM-001): openkal-macos
// ignores SIGPIPE for its own writes since 0.15 and a closed peer is a write that fails. The
// PRoot start by `execve' and the patched openkal-linux: openkal-linux 0.18 starts a program that
// way from the first start under a translator it recognises. The Win32 declarations for process
// times and identity: mcpplibs.os answers both on every system (openkal 0.17 kal_program_info).

namespace mcppls::platform {

namespace osp = mcpplibs::os::process;

std::optional<PreopenMatch> match_preopen(std::string_view absolutePath) {
    const auto located = mcpplibs::os::fs::locate(std::filesystem::path { std::string { absolutePath } });
    if (!located) return std::nullopt;
    return PreopenMatch { base::normalize_path(located->preopen), located->relative == "." ? std::string {} : located->relative };
}

struct Process::State {
    osp::process child;
    bool exited { false };
    int exitCode { -1 };
    std::mutex waitMutex;
};

namespace {

// The exit code of mcppls' interface: the status the child returned, or -1 when its environment
// ended it (a signal, a forced end).
int code_of(const osp::exit_status& status) { return status.by_environment ? -1 : status.code; }

} // namespace

Process::Process() = default;
Process::Process(Process&& other) noexcept = default;
Process& Process::operator=(Process&& other) noexcept = default;

// A child still running is asked to end and awaited (mcpplibs.os: ended outright with its unit
// when it has one), unless it was detached.
Process::~Process() {
    if (!state_) return;
    state_->child.close_input();
    if (!state_->exited) {
        state_->child.terminate();
        (void)state_->child.wait();
    }
}

bool Process::valid() const { return static_cast<bool>(state_); }

std::optional<std::int64_t> Process::native_pid() const {
    if (!state_ || state_->exited) return std::nullopt;
    const auto id = state_->child.id();
    if (id == 0) return std::nullopt;
    return static_cast<std::int64_t>(id);
}

base::Result<Process> Process::spawn(const SpawnOptions& options) {
    if (!base::is_absolute_path(options.program)) {
        return base::fail("spawn-program", std::format("program path must be absolute: {}", options.program));
    }
    if (!mcpplibs::os::fs::locate(std::filesystem::path { options.program })) {
        return base::fail("spawn-program", std::format("program is outside every preopened directory: {}", options.program));
    }
    const std::string workPath { options.workDirectory.empty() ? fs::current_directory() : options.workDirectory };
    if (!mcpplibs::os::fs::locate(std::filesystem::path { workPath })) {
        return base::fail("spawn-directory", std::format("work directory is outside every preopened directory: {}", workPath));
    }

    osp::spawn_options how;
    how.program = std::filesystem::path { options.program };
    how.args = options.arguments;
    how.cwd = std::filesystem::path { workPath };
    // A stated environment is the whole of it, so inheritance is explicit.
    if (options.environment) {
        how.clear_env = true;
        for (const auto& variable : *options.environment) {
            const auto equals = variable.find('=', 1);
            if (equals == std::string::npos) continue;
            how.env.emplace_back(variable.substr(0, equals), variable.substr(equals + 1));
        }
    }
    // The streams. Standard output carries the protocol, so a child that is not piped writes to
    // this process's standard error. With no streams at all the child holds none of this
    // process's: a detached child would otherwise keep the protocol's pipe open.
    if (options.noStreams) {
        how.in = how.out = how.err = osp::stdio::null;
    } else {
        how.in = options.pipeInput ? osp::stdio::pipe : osp::stdio::inherit;
        how.out = options.pipeOutput ? osp::stdio::pipe : osp::stdio::error;
        how.err = options.pipeError ? osp::stdio::pipe : osp::stdio::error;
    }
    // A detached child leads a unit of its own too, so what ends this program's unit does not end it.
    how.own_unit = options.ownUnit || options.detached;
    how.bound_lifetime = !options.detached && options.boundLifetime;

    auto started = osp::process::spawn(how);
    if (!started) return base::fail("spawn-failed", std::format("cannot start {}: {}", options.program, started.error().message()));
    Process process;
    process.state_ = std::make_unique<State>();
    process.state_->child = std::move(*started);
    return process;
}

base::Result<void> Process::write(std::string_view bytes) {
    if (!state_) return base::fail("process-write", "input is not open");
    if (auto written = state_->child.write(bytes); !written) return base::fail("process-write", written.error().message());
    return {};
}

base::Result<std::string> Process::read_output() {
    if (!state_) return std::string {};
    auto got = state_->child.read_output();
    if (!got) return base::fail("process-read", std::format("read failed: {}", got.error().message()));
    return std::move(*got);
}

base::Result<std::string> Process::read_error() {
    if (!state_) return std::string {};
    auto got = state_->child.read_error();
    if (!got) return base::fail("process-read", std::format("read failed: {}", got.error().message()));
    return std::move(*got);
}

// A read that gives up. `bounded` reports whether this resource could be bounded at all: where
// it could not, the read below it blocked, and a caller waiting for this thread to notice a
// stop flag would wait as long as the stream stays open.
base::Result<std::optional<std::string>> Process::read_output_for(std::chrono::milliseconds timeout, bool* bounded) {
    if (!state_) return std::optional<std::string> { std::string {} };
    auto got = state_->child.read_output_for(std::max(timeout, std::chrono::milliseconds { 1 }));
    if (bounded) *bounded = state_->child.reads_bounded();
    if (!got) return base::fail("process-read", std::format("read failed: {}", got.error().message()));
    return std::move(*got);
}

base::Result<std::optional<std::string>> Process::read_error_for(std::chrono::milliseconds timeout, bool* bounded) {
    if (!state_) return std::optional<std::string> { std::string {} };
    auto got = state_->child.read_error_for(std::max(timeout, std::chrono::milliseconds { 1 }));
    if (bounded) *bounded = state_->child.reads_bounded();
    if (!got) return base::fail("process-read", std::format("read failed: {}", got.error().message()));
    return std::move(*got);
}

void Process::close_input() {
    if (state_) state_->child.close_input();
}

base::Result<int> Process::wait() {
    if (!state_) return base::fail("process-wait", "no process");
    std::lock_guard lock { state_->waitMutex };
    if (state_->exited) return state_->exitCode;
    auto status = state_->child.wait();
    if (!status) return base::fail("process-wait", status.error().message());
    state_->exited = true;
    state_->exitCode = code_of(*status);
    return state_->exitCode;
}

base::Result<std::optional<int>> Process::wait_for(std::chrono::milliseconds timeout) {
    if (!state_) return base::fail("process-wait", "no process");
    std::lock_guard lock { state_->waitMutex };
    if (state_->exited) return std::optional<int> { state_->exitCode };
    auto status = state_->child.wait_for(std::max(timeout, std::chrono::milliseconds { 1 }));
    if (!status) return base::fail("process-wait", status.error().message());
    if (!status->has_value()) return std::optional<int> {};
    state_->exited = true;
    state_->exitCode = code_of(**status);
    return std::optional<int> { state_->exitCode };
}

void Process::terminate() {
    if (!state_) return;
    std::lock_guard lock { state_->waitMutex };
    if (!state_->exited) state_->child.terminate();
}

void Process::detach() {
    if (!state_) return;
    state_->child.detach();
    state_.reset();
}

void Process::kill() {
    if (!state_) return;
    std::lock_guard lock { state_->waitMutex };
    if (!state_->exited) state_->child.kill();
}

void Process::end_unit() {
    if (!state_) return;
    std::lock_guard lock { state_->waitMutex };
    state_->child.end_unit();
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

std::optional<bool> process_alive(std::int64_t pid) {
    if (pid <= 0) return std::nullopt;
    const auto described = osp::describe(static_cast<std::uint64_t>(pid));
    if (!described) {
        if (described.error().code == mcpplibs::os::errc::not_found) return false;
        return std::nullopt;
    }
    return !described->ended;
}

std::optional<double> cpu_seconds(std::int64_t pid) {
    if (pid <= 0) return std::nullopt;
    const auto described = osp::describe(static_cast<std::uint64_t>(pid));
    if (!described || described->ended) return std::nullopt;
    return static_cast<double>(described->cpu_ns) / 1e9;
}

namespace {

constexpr std::string_view IDENTITY_SCHEME { "ok:" };

std::optional<ProcessIdentity> identity_of(const osp::program& who) {
    if (who.id == 0 || who.started_ns == 0) return std::nullopt;
    return ProcessIdentity { static_cast<std::int64_t>(who.id), std::format("{}{}", IDENTITY_SCHEME, who.started_ns) };
}

} // namespace

// A zombie has no identity: the question "is this the process I knew" has no answer for one that
// has ended, and a lease whose owner was killed would otherwise read as "the same live process".
std::optional<ProcessIdentity> process_identity(std::int64_t pid) {
    if (pid <= 0) return std::nullopt;
    const auto described = osp::describe(static_cast<std::uint64_t>(pid));
    if (!described || described->ended) return std::nullopt;
    return identity_of(described->who);
}

// Named as the system's /proc names this process where there is one (openkal-linux 0.18): under a
// sandbox that numbers its programs apart from the /proc it shows, getpid() answers 1 for every
// server, and a killed server and its restart read as the same live process.
std::optional<ProcessIdentity> process_self() {
    const auto self = osp::self();
    if (!self) return std::nullopt;
    return identity_of(*self);
}

bool same_identity_scheme(std::string_view a, std::string_view b) {
    return a.starts_with(IDENTITY_SCHEME) == b.starts_with(IDENTITY_SCHEME);
}

namespace {

std::optional<std::int64_t> parse_count(std::string_view text) {
    std::int64_t value { 0 };
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc {} || end != text.data() + text.size() || value < 0) return std::nullopt;
    return value;
}

} // namespace

std::vector<ThreadCpu> thread_cpu(std::int64_t pid) {
    std::vector<ThreadCpu> threads;
    if (mcppls::os::family() == mcppls::os::Family::linux) {
        if (pid <= 0) return threads;
        constexpr double TICKS_PER_SECOND { 100 };
        for (const auto& task : fs::list_directory(std::format("/proc/{}/task", pid))) {
            const auto stat = fs::read_file(task + "/stat");
            if (!stat) continue;
            // "<tid> (<comm>) <state> ...": comm may hold spaces and parentheses, so the last ')' ends it.
            const auto open = stat->find('(');
            const auto close = stat->rfind(')');
            if (open == std::string::npos || close == std::string::npos || close < open || close + 2 >= stat->size()) continue;
            std::vector<std::string_view> fields;
            for (auto field : base::split(std::string_view { *stat }.substr(close + 2), ' ')) {
                if (!field.empty()) fields.push_back(field);
            }
            if (fields.size() < 13) continue;
            const auto id = parse_count(std::string_view { *stat }.substr(0, open > 0 ? open - 1 : 0));
            const auto user = parse_count(fields[11]);
            const auto system = parse_count(fields[12]);
            if (!id || !user || !system) continue;
            threads.push_back(ThreadCpu { *id, stat->substr(open + 1, close - open - 1), static_cast<double>(*user + *system) / TICKS_PER_SECOND });
        }
    }
    (void)pid;
    return threads;
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
