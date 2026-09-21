module mcppls.platform.toolrun;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.log;
import mcppls.platform.process;
import mcppls.platform.toolenv;

namespace mcppls::platform::toolrun {

namespace {

constexpr std::size_t ERROR_TAIL_LINES { 20 };

struct Store {
    std::mutex mutex;
    std::deque<Record> records;
    std::uint64_t nextId { 1 };
    std::function<void(const Record&)> recorder;
};

Store& store() {
    static Store value;
    return value;
}

void keep(const Record& record) {
    std::function<void(const Record&)> recorder;
    {
        auto& value = store();
        std::lock_guard lock { value.mutex };
        value.records.push_back(record);
        while (value.records.size() > KEPT) value.records.pop_front();
        recorder = value.recorder;
    }
    if (recorder) recorder(record);
}

// `MCPP_OFFLINE` is mcpp's own switch and means nothing to any other program, so setting it for
// every offline run costs nothing and covers the one tool that would otherwise refresh an index.
void put(std::vector<std::string>& environment, std::string_view name, std::string_view value) {
    const std::string prefix { std::format("{}=", name) };
    for (auto& variable : environment) {
        if (variable.starts_with(prefix)) {
            variable = std::format("{}{}", prefix, value);
            return;
        }
    }
    environment.push_back(std::format("{}{}", prefix, value));
}

} // namespace

base::Result<RunResult> run(const Request& request, Record* out) {
    Record record;
    {
        auto& value = store();
        std::lock_guard lock { value.mutex };
        record.id = value.nextId++;
    }
    record.program = request.program;
    record.arguments = request.arguments;
    record.workDirectory = request.workDirectory;
    record.purpose = request.purpose;
    record.root = request.root;
    record.offline = request.network == Network::offline;

    // S2-6-3, S2-6-5: the user's session environment, and nothing added to it. No credential of
    // this server's own ever reaches a program it starts; the only variable it adds is the offline
    // switch below, which takes nothing and grants nothing.
    std::vector<std::string> environment;
    if (request.environment) {
        environment = *request.environment;
        record.environmentSource = "given";
    } else {
        const auto resolved = toolenv::get(request.environmentWait);
        environment = resolved.variables;
        record.environmentSource = resolved.source;
    }
    if (record.offline) put(environment, "MCPP_OFFLINE", "1");

    SpawnOptions options {
        .program = request.program,
        .arguments = request.arguments,
        .workDirectory = request.workDirectory,
        .environment = std::move(environment),
    };

    // Says once that the program is taking long, so the editor can say so too. It never ends
    // anything: only the hard bound does that.
    std::mutex finishedMutex;
    std::condition_variable finishedChanged;
    bool finished { false };
    const auto started = std::chrono::steady_clock::now();
    std::jthread soft;
    if (request.soft > std::chrono::milliseconds { 0 } && request.onSoftDeadline) {
        soft = std::jthread { [&] {
            std::unique_lock lock { finishedMutex };
            if (finishedChanged.wait_for(lock, request.soft, [&] { return finished; })) return;
            lock.unlock();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
            base::log::info("{} is still running after {} ms", request.program, elapsed.count());
            request.onSoftDeadline(elapsed);
        } };
    }
    auto result = run(std::move(options), request.bounds, request.input);
    {
        std::lock_guard lock { finishedMutex };
        finished = true;
    }
    finishedChanged.notify_all();
    if (soft.joinable()) soft.join();

    if (!result) {
        record.startFailed = true;
        record.failure = result.error().message;
        record.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        keep(record);
        if (out) *out = record;
        return result;
    }

    record.durationMs = result->duration.count();
    record.exitCode = result->exitCode;
    record.timedOut = result->timedOut;
    record.unitEnded = result->timedOut || result->outputHeldOpen;
    record.outputHeldOpen = result->outputHeldOpen;
    record.outputTruncated = result->outputTruncated;
    record.errorTruncated = result->errorTruncated;
    record.errorTail = last_lines(result->error, ERROR_TAIL_LINES);
    keep(record);
    if (out) *out = record;

    base::log::debug("{} {} finished in {} ms with {}{}", request.purpose, request.program, record.durationMs,
                     record.exitCode, record.outputHeldOpen ? " (its output was held open)" : "");
    if (result->outputHeldOpen) {
        base::log::warning("{} left something holding its output; the unit was ended after {} ms",
                           request.program, record.durationMs);
    } else if (result->timedOut) {
        base::log::warning("{} did not finish within {} ms; the unit was ended",
                           request.program, request.bounds.hard.count());
    }
    return result;
}

void observed_network(std::uint64_t id) {
    Record amended;
    bool found { false };
    std::function<void(const Record&)> recorder;
    {
        auto& value = store();
        std::lock_guard lock { value.mutex };
        for (auto& record : value.records) {
            if (record.id != id) continue;
            record.networkObserved = true;
            amended = record;
            found = true;
            break;
        }
        recorder = value.recorder;
    }
    if (!found) return;
    if (amended.offline) {
        // P3 says an implicit run does not reach the network. The program itself says it did.
        base::log::warning("{} reported network access on a run the server asked to be offline", amended.program);
    }
    if (recorder) recorder(amended);
}

nlohmann::json to_json(const Record& record) {
    nlohmann::json value {
        { "id", record.id },
        { "program", record.program },
        { "arguments", record.arguments },
        { "purpose", record.purpose },
        { "environment", record.environmentSource },
        { "offline", record.offline },
        { "durationMs", record.durationMs },
        { "exitCode", record.exitCode },
    };
    if (!record.workDirectory.empty()) value["workDirectory"] = record.workDirectory;
    if (record.timedOut) value["timedOut"] = true;
    if (record.unitEnded) value["unitEnded"] = true;
    if (record.outputHeldOpen) value["outputHeldOpen"] = true;
    if (record.outputTruncated) value["outputTruncated"] = true;
    if (record.errorTruncated) value["errorTruncated"] = true;
    if (record.startFailed) value["startFailed"] = true;
    if (record.networkObserved) value["network"] = true;
    if (!record.failure.empty()) value["failure"] = record.failure;
    if (!record.errorTail.empty()) value["stderrTail"] = record.errorTail;
    return value;
}

std::vector<Record> recent(std::size_t limit) {
    auto& value = store();
    std::lock_guard lock { value.mutex };
    const std::size_t count { std::min(limit, value.records.size()) };
    return std::vector<Record> { value.records.end() - static_cast<std::ptrdiff_t>(count), value.records.end() };
}

void set_recorder(std::function<void(const Record&)> recorder) {
    auto& value = store();
    std::lock_guard lock { value.mutex };
    value.recorder = std::move(recorder);
}

} // namespace mcppls::platform::toolrun
