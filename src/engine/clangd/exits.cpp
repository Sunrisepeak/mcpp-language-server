module mcppls.engine.clangd.exits;

import std;
import nlohmann.json;

namespace mcppls::engine::clangd {

namespace {

struct Named {
    std::uint32_t number;
    std::string_view name;
    ExitKind kind;
};

// The statuses a program is ended with on Windows that a report of clangd has shown or can show.
constexpr std::array<Named, 16> NTSTATUS_NAMES { {
    { 0xC000001Du, "illegal instruction", ExitKind::fault },
    { 0xC0000005u, "access violation", ExitKind::fault },
    { 0xC00000FDu, "stack overflow", ExitKind::fault },
    { 0xC0000409u, "stack buffer overrun", ExitKind::fault },
    { 0xC0000374u, "heap corruption", ExitKind::fault },
    { 0xC0000094u, "integer division by zero", ExitKind::fault },
    { 0xC0000096u, "privileged instruction", ExitKind::fault },
    { 0xC0000006u, "in-page error", ExitKind::fault },
    { 0xC0000017u, "no memory", ExitKind::fault },
    { 0xC000012Du, "commit limit", ExitKind::fault },
    { 0xC0000135u, "DLL not found", ExitKind::fault },
    { 0xC0000142u, "DLL initialization failed", ExitKind::fault },
    { 0x80000003u, "breakpoint", ExitKind::fault },
    { 0x80000002u, "datatype misalignment", ExitKind::fault },
    { 0xC000013Au, "console interrupt", ExitKind::outside },
    { 0xFFFFFFFFu, "terminated", ExitKind::outside },
} };

// Signals by the numbers Linux and macOS share, and the few they do not (the name then says both).
constexpr std::array<Named, 14> SIGNAL_NAMES { {
    { 1, "SIGHUP", ExitKind::outside },
    { 2, "SIGINT", ExitKind::outside },
    { 3, "SIGQUIT", ExitKind::outside },
    { 4, "SIGILL", ExitKind::fault },
    { 5, "SIGTRAP", ExitKind::fault },
    { 6, "SIGABRT", ExitKind::fault },
    { 7, "SIGBUS or SIGEMT", ExitKind::fault },
    { 8, "SIGFPE", ExitKind::fault },
    { 9, "SIGKILL", ExitKind::outside },
    { 10, "SIGUSR1 or SIGBUS", ExitKind::fault },
    { 11, "SIGSEGV", ExitKind::fault },
    { 13, "SIGPIPE", ExitKind::outside },
    { 15, "SIGTERM", ExitKind::outside },
    { 31, "SIGSYS", ExitKind::fault },
} };

std::int64_t seconds_of(LedgerClock::time_point at) {
    return std::chrono::duration_cast<std::chrono::seconds>(at.time_since_epoch()).count();
}

LedgerClock::time_point time_of(std::int64_t seconds) {
    return LedgerClock::time_point { std::chrono::duration_cast<LedgerClock::duration>(std::chrono::seconds { seconds }) };
}

} // namespace

std::string ExitCondition::number_text() const {
    if (kind == ExitKind::returned) return std::format("{}", static_cast<std::int32_t>(number));
    return windows ? std::format("0x{:08X}", number) : std::format("{}", number);
}

std::string ExitCondition::text() const {
    if (kind == ExitKind::returned) return std::format("status {}", static_cast<std::int32_t>(number));
    if (windows) {
        if (name.empty()) return std::format("status 0x{:08X}", number);
        const bool vowel { name.front() == 'a' || name.front() == 'i' };
        return std::format("{} {} (0x{:08X})", vowel ? "an" : "a", name, number);
    }
    return name.empty() ? std::format("signal {}", number) : std::format("{} (signal {})", name, number);
}

ExitCondition exit_condition(int code, bool byEnvironment, ExitHost host) {
    ExitCondition condition;
    condition.windows = host == ExitHost::windows;
    if (!byEnvironment) {
        condition.number = static_cast<std::uint32_t>(code);
        // A program on Windows that ends by a status of the system's is reported so by the platform; one whose starter
        // does not go through it (a runner that returns its image's status) still shows the number.
        if (condition.windows && (condition.number & 0xF0000000u) == 0xC0000000u) {
            condition.kind = ExitKind::fault;
            for (const auto& named : NTSTATUS_NAMES) {
                if (named.number == condition.number) {
                    condition.name = named.name;
                    condition.kind = named.kind;
                }
            }
        }
        return condition;
    }
    if (condition.windows) {
        // The platform reports `status & 0x7FFFFFFF`: the top bit is put back, except for the two warnings a fault
        // can end a program with, which have it and nothing else above bit 16 (0x80000002, 0x80000003).
        const std::uint32_t reported { static_cast<std::uint32_t>(code) };
        condition.number = reported == 0x7FFFFFFFu ? 0xFFFFFFFFu : (reported | 0x80000000u);
        condition.kind = ExitKind::fault;
        for (const auto& named : NTSTATUS_NAMES) {
            if (named.number == condition.number) {
                condition.name = named.name;
                condition.kind = named.kind;
            }
        }
        return condition;
    }
    condition.number = static_cast<std::uint32_t>(code) & 0x7Fu;
    condition.kind = ExitKind::fault;
    for (const auto& named : SIGNAL_NAMES) {
        if (named.number == condition.number) {
            condition.name = named.name;
            condition.kind = named.kind;
        }
    }
    return condition;
}

std::optional<RunnerFaultLine> runner_fault_line(std::string_view line) {
    constexpr std::string_view FAULT { "openkal: fault: " };
    constexpr std::string_view FRAME { "openkal:   #" };
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.remove_suffix(1);
    if (const auto at = line.find(FAULT); at != std::string_view::npos) {
        std::string_view what { line.substr(at + FAULT.size()) };
        // The address is the process's own and names nothing: the condition is what a report keeps.
        if (const auto address = what.rfind(" at 0x"); address != std::string_view::npos) what = what.substr(0, address);
        if (what.empty()) return std::nullopt;
        return RunnerFaultLine { std::string { what }, {} };
    }
    if (const auto at = line.find(FRAME); at != std::string_view::npos) {
        // "#n <address> <path>+<offset>": the last word, without its directories.
        const auto word = line.rfind(' ');
        if (word == std::string_view::npos || word < at + FRAME.size()) return std::nullopt;
        std::string_view where { line.substr(word + 1) };
        if (where.find("+0x") == std::string_view::npos) return std::nullopt;
        if (const auto slash = where.find_last_of("/\\"); slash != std::string_view::npos) where = where.substr(slash + 1);
        return RunnerFaultLine { {}, std::string { where } };
    }
    return std::nullopt;
}

void BuildsInFlight::began(std::string_view key, std::string_view path) {
    for (auto& [known, spelled] : order_) {
        if (known == key) {
            spelled = std::string { path };
            return;
        }
    }
    order_.emplace_back(std::string { key }, std::string { path });
}

void BuildsInFlight::ended(std::string_view key) {
    std::erase_if(order_, [&](const auto& item) { return item.first == key; });
}

void BuildsInFlight::clear() { order_.clear(); }

std::vector<std::string> BuildsInFlight::files() const {
    std::vector<std::string> files;
    for (const auto& item : order_) files.push_back(item.second);
    return files;
}

bool BuildsInFlight::contains(std::string_view key) const {
    return std::ranges::any_of(order_, [&](const auto& item) { return item.first == key; });
}

std::string_view to_string(ExitSuspects::Rule rule) {
    switch (rule) {
    case ExitSuspects::Rule::named: return "named";
    case ExitSuspects::Rule::in_flight: return "in-flight";
    case ExitSuspects::Rule::guessed: return "guessed";
    case ExitSuspects::Rule::nobody: return "nobody";
    }
    return "nobody";
}

ExitSuspects exit_suspects(const ExitCondition& condition, std::optional<std::string> named, std::vector<std::string> inFlight,
                           std::vector<std::string> guessed) {
    ExitSuspects suspects;
    // clangd's own word comes first, whatever ended it: it printed the context because it was crashing.
    if (named && !named->empty()) {
        suspects.rule = ExitSuspects::Rule::named;
        suspects.files.push_back(std::move(*named));
        suspects.certain = true;
        return suspects;
    }
    // Ended from outside: whatever it was building, the build did not end it.
    if (condition.kind == ExitKind::outside) return suspects;
    if (!inFlight.empty()) {
        suspects.rule = ExitSuspects::Rule::in_flight;
        suspects.certain = inFlight.size() == 1 && condition.kind == ExitKind::fault;
        suspects.files = std::move(inFlight);
        return suspects;
    }
    if (!guessed.empty()) {
        suspects.rule = ExitSuspects::Rule::guessed;
        suspects.files = std::move(guessed);
    }
    return suspects;
}

const CrashRecord& CrashLedger::exited(std::string_view key, std::string_view file, std::string_view engine, std::string_view condition,
                                       std::string_view content, std::string_view closure, LedgerClock::time_point now) {
    auto found = records_.find(key);
    if (found != records_.end() && found->second.record.engine != engine) {
        records_.erase(found);
        found = records_.end();
    }
    if (found == records_.end()) {
        found = records_.emplace(std::string { key }, Entry {}).first;
        found->second.record.firstAt = now;
    }
    Entry& entry { found->second };
    CrashRecord& record { entry.record };
    entry.asked = false;
    record.file = std::string { file };
    record.engine = std::string { engine };
    record.condition = std::string { condition };
    record.exits += 1;
    record.lastAt = now;
    record.term = TERMS[std::min<std::size_t>(static_cast<std::size_t>(record.exits) - 1, TERMS.size() - 1)];
    record.retryAt = now + record.term;
    record.content = std::string { content };
    record.closure = std::string { closure };
    return record;
}

CrashLedger::Verdict CrashLedger::verdict(std::string_view key, std::string_view engine, std::string_view content, std::string_view closure,
                                          LedgerClock::time_point now) const {
    const auto found = records_.find(key);
    if (found == records_.end()) return Verdict::clean;
    const CrashRecord& record { found->second.record };
    if (record.engine != engine) return Verdict::clean;
    if (found->second.asked) return Verdict::retry;
    if (now < record.retryAt) return Verdict::held;
    if (record.exits >= 2 && record.content == content && record.closure == closure) return Verdict::held;
    return Verdict::retry;
}

bool CrashLedger::survived(std::string_view key) { return erase(key); }

bool CrashLedger::try_again(std::string_view key) {
    const auto found = records_.find(key);
    if (found == records_.end()) return false;
    found->second.asked = true;
    return true;
}

void CrashLedger::try_all_again() {
    for (auto& [key, entry] : records_) entry.asked = true;
}

bool CrashLedger::erase(std::string_view key) {
    const auto found = records_.find(key);
    if (found == records_.end()) return false;
    records_.erase(found);
    return true;
}

bool CrashLedger::prune(std::string_view engine, LedgerClock::time_point now) {
    return std::erase_if(records_, [&](const auto& item) {
        return item.second.record.engine != engine || now - item.second.record.lastAt > KEPT;
    }) > 0;
}

const CrashRecord* CrashLedger::find(std::string_view key) const {
    const auto found = records_.find(key);
    return found == records_.end() ? nullptr : &found->second.record;
}

std::vector<std::string> CrashLedger::keys() const {
    std::vector<std::string> keys;
    for (const auto& item : records_) keys.push_back(item.first);
    return keys;
}

Json CrashLedger::to_json() const {
    Json files = Json::object();
    for (const auto& [key, entry] : records_) {
        const CrashRecord& record { entry.record };
        files[key] = Json { { "file", record.file }, { "engine", record.engine }, { "condition", record.condition }, { "exits", record.exits },
                            { "firstAt", seconds_of(record.firstAt) }, { "lastAt", seconds_of(record.lastAt) }, { "term", record.term.count() },
                            { "retryAt", seconds_of(record.retryAt) }, { "contentAtLastExit", record.content }, { "closureAtLastExit", record.closure } };
    }
    return Json { { "version", 1 }, { "files", std::move(files) } };
}

void CrashLedger::from_json(const Json& value) {
    records_.clear();
    if (!value.is_object() || value.value("version", 0) != 1 || !value.contains("files") || !value["files"].is_object()) return;
    const Json& files = value["files"];
    for (auto at = files.begin(); at != files.end(); ++at) {
        const std::string key { at.key() };
        const Json& item = at.value();
        if (!item.is_object()) continue;
        Entry entry;
        CrashRecord& record { entry.record };
        const auto text = [&](const char* name) { return item.contains(name) && item[name].is_string() ? item[name].get<std::string>() : std::string {}; };
        const auto number = [&](const char* name) { return item.contains(name) && item[name].is_number_integer() ? item[name].get<std::int64_t>() : std::int64_t { 0 }; };
        record.file = text("file");
        record.engine = text("engine");
        record.condition = text("condition");
        record.exits = static_cast<int>(number("exits"));
        record.firstAt = time_of(number("firstAt"));
        record.lastAt = time_of(number("lastAt"));
        record.term = std::chrono::minutes { number("term") };
        record.retryAt = time_of(number("retryAt"));
        record.content = text("contentAtLastExit");
        record.closure = text("closureAtLastExit");
        if (record.file.empty() || record.engine.empty() || record.exits <= 0) continue;
        records_.emplace(key, std::move(entry));
    }
}

void StartBudget::started(std::size_t readOnlyGrants) {
    ++starts_;
    spent_ += readOnlyGrants;
}

std::optional<std::size_t> StartBudget::left(std::size_t readOnlyGrants) const {
    if (capacity_ == 0) return std::nullopt;
    if (readOnlyGrants == 0) return std::nullopt;
    return spent_ >= capacity_ ? 0 : (capacity_ - spent_) / readOnlyGrants;
}

bool StartBudget::automatic(std::size_t readOnlyGrants) const {
    const auto fits = left(readOnlyGrants);
    return !fits || *fits > RESERVE;
}

bool StartBudget::by_person(std::size_t readOnlyGrants) const {
    const auto fits = left(readOnlyGrants);
    return !fits || *fits >= 1;
}

} // namespace mcppls::engine::clangd
